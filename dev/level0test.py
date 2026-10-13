#!/usr/bin/env python3
# level0test.py - paranoia level 0: the next block is the emptiest block
#
# a partition is filled to 80%, then random mesoblocks are overwritten, once
# in level 1 and once in level 0: level 0 must need fewer block writes for
# the same data (the garbage collector copies less), the data must be
# correct (also after a restart), the next block that the daemon chooses in
# level 0 must be a block with the least live data (from verbose-juggler)
#
# usage: level0test.py DAEMON-crash SEED
import os, sys, time, socket, random, hashlib, subprocess, errno, re, atexit

B = os.environ.get("S3BUILD", os.getcwd())
MNT, SOCK = "/mnt/s3", "/run/scubed3/control"
M_LOG, MM_LOG = 12, 16
MESO, MACRO = 1 << M_LOG, 1 << MM_LOG
MMPM = MACRO // MESO - 1
TOTAL, NB, RES = 200, 100, 20
daemon, seed = sys.argv[1], int(sys.argv[2])
rng = random.Random(seed)
base, wlog, logname = B + "/level0.base", B + "/level0.wlog", B + "/level0.log"
log = open(logname, "w")
P = None
KEY = hashlib.pbkdf2_hmac("sha256", b"a", b"scubed3_prod", 1000, 32).hex()
SIZE = (NB - RES) * MMPM

def cmd(line, ok=True):
    s = socket.socket(socket.AF_UNIX); s.connect(SOCK)
    s.sendall((line + "\nexit\n").encode()); buf = b""
    while True:
        d = s.recv(65536)
        if not d: break
        buf += d
    t = buf.decode()
    if ok and not t.startswith("OK"): raise RuntimeError("%s: %r" % (line, t))
    return t

def start():
    global P
    subprocess.run(["fusermount", "-u", "-z", MNT], capture_output=True)
    if os.path.exists(SOCK): os.unlink(SOCK)
    P = subprocess.Popen([B + "/" + daemon, "-b", base, "-m", str(M_LOG), "-M", str(MM_LOG),
        "-f", "-o", "direct_io", MNT], stdout=log, stderr=log,
        env=dict(os.environ, SCUBED3_WRITE_LOG=wlog))
    while not os.path.exists(SOCK):
        if P.poll() is not None: raise RuntimeError("daemon did not start")
        time.sleep(0.05)
    time.sleep(0.1)

def stop():
    cmd("close a"); subprocess.run(["fusermount", "-u", MNT], check=True); P.wait(timeout=60)

atexit.register(lambda: P and P.poll() is None and P.kill())

def opent(flags):
    for _ in range(100):
        try: return os.open(MNT + "/a", flags)
        except OSError as e:
            if e.errno != errno.EBUSY: raise
            time.sleep(0.05)
    raise RuntimeError("busy")

def writes():
    try: return sum(1 for _ in open(wlog))
    except FileNotFoundError: return 0

def juggler():
    """(pool: block -> live mesoblocks, next block) from verbose-juggler"""
    off = os.path.getsize(logname)
    cmd("verbose-juggler a")
    for _ in range(200):
        with open(logname, "rb") as f:
            f.seek(off); txt = f.read().decode(errors="replace")
        m = re.search(r"--juggler--.*?no_pool=(\d+), seqno=\d+, next=(-?\d+)", txt, re.S)
        if m:
            e = re.findall(r":\d+ \[(\d+) +\d+ (\d+)\]", txt[m.end():])
            if len(e) >= int(m.group(1)):
                return {int(b): int(l) for b, l in e[:int(m.group(1))]}, int(m.group(2))
        time.sleep(0.02)
    raise RuntimeError("no juggler output")

model = {}
def overwrite(n):
    fd = opent(os.O_RDWR); w0 = writes()
    for _ in range(n):
        m = rng.randrange(int(SIZE * 0.8)); d = os.urandom(MESO)
        os.pwrite(fd, d, m * MESO); model[m] = d
    os.fsync(fd); os.close(fd)
    return writes() - w0

def verify():
    fd = opent(os.O_RDONLY); data = os.pread(fd, SIZE * MESO, 0); os.close(fd)
    bad = [m for m in range(SIZE) if data[m * MESO:(m + 1) * MESO] != model.get(m, bytes(MESO))]
    if bad: raise RuntimeError("%d mesoblocks wrong, first %d" % (len(bad), bad[0]))

with open(base, "wb") as f: f.write(os.urandom(TOTAL * MACRO))
if os.path.exists(wlog): os.unlink(wlog)
start()
cmd("create-internal a CBC_ESSIV(AES256) " + KEY)
cmd("resize-internal a %d %d" % (NB, RES))
fd = opent(os.O_RDWR)
for m in range(int(SIZE * 0.8)):
    d = os.urandom(MESO); os.pwrite(fd, d, m * MESO); model[m] = d
os.fsync(fd); os.close(fd)

N = 6000
overwrite(N)                            # steady state in level 1
w1 = overwrite(N)
cmd("paranoia 0")
if "paranoia level 0" not in cmd("p"): raise RuntimeError("level 0 not on")
overwrite(N)                            # steady state in level 0
w0 = overwrite(N)
print("%d mesoblock writes on a partition that is 80%% full: level 1 %d block writes, "
      "level 0 %d block writes (%d mesoblocks per block)" % (N, w1, w0, MMPM), flush=True)
if not w0 < w1: raise RuntimeError("level 0 does not write less")

# the next block is the emptiest block (the choice is made after the write
# of the current block, data written since then changes the counts, so
# check right after an fsync, at which the current block is written)
ok = 0
for i in range(20):
    overwrite(rng.randrange(1, 40))
    pool, nxt = juggler()
    if nxt < 0: raise RuntimeError("no next block")
    # nothing was written since the fsync, so the counts are those of the
    # choice; the current block was excluded (it is empty then), so at most
    # one block may have less live data
    if sum(1 for b, l in pool.items() if b != nxt and l < pool[nxt]) <= 1: ok += 1
print("next block has the least live data: %d of 20 times" % ok, flush=True)
if ok < 18: raise RuntimeError("level 0 does not choose the emptiest block")

verify()
stop(); start()
cmd("open-internal a CBC_ESSIV(AES256) " + KEY)
verify()
print("data correct, also after a restart", flush=True)
stop()
os.unlink(base)
print("LEVEL0TEST OK", flush=True)
