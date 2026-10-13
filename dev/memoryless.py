#!/usr/bin/env python3
# memoryless.py - level 1, one partition, no resizes: the written blocks must
# be i.i.d. uniform over the blocks of the partition, so the block that is
# written must not depend on how long ago it was written (its age rank among
# the blocks of the partition must be uniform)
# usage: memoryless.py DAEMON-crash WRITES [fsync-probability]
import os, sys, time, socket, hashlib, subprocess, errno, random, atexit, collections, math
import shutil
FUSERMOUNT = shutil.which("fusermount3") or "fusermount"   # libfuse 3 or 2
B = os.environ.get("S3BUILD", os.getcwd())
MNT, SOCK = "/mnt/s3", "/run/scubed3/control"
MESO, MACRO = 1 << 12, 1 << 16
TOTAL, NB, RES = 100, 40, 10
daemon, writes = sys.argv[1], int(sys.argv[2])
pf = float(sys.argv[3]) if len(sys.argv) > 3 else 0.0
base, wlog = B + "/ml.base", B + "/ml.wlog"
rng = random.Random(1)
def cmd(line):
    s = socket.socket(socket.AF_UNIX); s.connect(SOCK)
    s.sendall((line + "\nexit\n").encode()); buf = b""
    while True:
        x = s.recv(65536)
        if not x: break
        buf += x
    if not buf.startswith(b"OK"): raise RuntimeError(buf)
subprocess.run([FUSERMOUNT, "-u", "-z", MNT], capture_output=True)
if os.path.exists(SOCK): os.unlink(SOCK)
with open(base, "wb") as f: f.write(os.urandom(TOTAL * MACRO))
if os.path.exists(wlog): os.unlink(wlog)
P = subprocess.Popen([B + "/" + daemon, "-b", base, "-m", "12", "-M", "16", "-f", "-o", "direct_io", MNT],
    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=dict(os.environ, SCUBED3_WRITE_LOG=wlog))
atexit.register(lambda: P.poll() is None and P.kill())
while not os.path.exists(SOCK): time.sleep(0.05)
time.sleep(0.1)
key = hashlib.sha256(b"a").hexdigest()
cmd("create-internal a CBC_ESSIV(AES256) " + key); cmd("resize-internal a %d %d" % (NB, RES))
size = (NB - RES) * 15
fd = os.open(MNT + "/a", os.O_RDWR)
n0 = 0
def nw():
    try: return sum(1 for _ in open(wlog))
    except FileNotFoundError: return 0
while nw() < writes:
    for _ in range(50):
        os.pwrite(fd, os.urandom(MESO), rng.randrange(int(size * 0.7)) * MESO)
        if rng.random() < pf: os.fsync(fd)
os.close(fd)
cmd("close a"); subprocess.run([FUSERMOUNT, "-u", MNT]); P.wait()
wl = [int(x) // MACRO for x in open(wlog)]
pool = sorted(set(wl)); n = len(pool)
last = {b: -1 for b in pool}; ranks = collections.Counter()
for i, b in enumerate(wl):
    if i > 2000:
        order = sorted(pool, key=lambda x: last[x])
        ranks[order.index(b) * 4 // n] += 1
    last[b] = i
tot = sum(ranks.values()); e = tot / 4
c2 = sum((ranks[q] - e) ** 2 / e for q in range(4))
print("%s: %d writes on %d blocks, age quartile of the written block (oldest first): %s, chi2=%.1f (df 3)" %
      (daemon, len(wl), n, [ranks[q] for q in range(4)], c2))
os.unlink(base)
# chi2 > 16.27 happens with probability 0.001 for i.i.d. writes
print("MEMORYLESS %s" % ("OK" if c2 < 16.27 else "FAILED"))
