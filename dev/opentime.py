#!/usr/bin/env python3
# opentime.py - how long it takes to open a partition on a large base device
#
# a base device of BLOCKS macroblocks (32 KiB: -m 12 -M 15, default 29000,
# the most -m 12 allows) with three small partitions; the page cache is
# dropped, then every partition is opened (and closed) twice, and a
# passphrase that has no partition; the first open reads the start of the
# index of every block from the base device, the later ones only decrypt it
# (scubed3 keeps it in memory); the partitions must contain what was written
#
# usage: opentime.py DAEMON [BLOCKS]
import os, sys, time, socket, hashlib, subprocess, shutil, atexit
FUSERMOUNT = shutil.which("fusermount3") or "fusermount"   # libfuse 3 or 2

B = os.environ.get("S3BUILD", os.getcwd())
MNT, SOCK = "/mnt/s3", "/run/scubed3/control"
M_LOG, MM_LOG = 12, 15
MESO, MACRO = 1 << M_LOG, 1 << MM_LOG
MMPM = MACRO // MESO - 1
daemon = sys.argv[1]
TOTAL = int(sys.argv[2]) if len(sys.argv) > 2 else 29000
base = B + "/opentime.base"
log = open(B + "/opentime.log", "w")
P = None
atexit.register(lambda: P and P.poll() is None and P.kill())

def key(name): return hashlib.pbkdf2_hmac("sha256", name.encode(), b"scubed3_prod", 1000, 32).hex()

def start():
    global P
    subprocess.run([FUSERMOUNT, "-u", "-z", MNT], capture_output=True)
    if os.path.exists(SOCK): os.unlink(SOCK)
    P = subprocess.Popen([B + "/" + daemon, "-b", base, "-m", str(M_LOG), "-M", str(MM_LOG),
        "-f", MNT], stdout=log, stderr=log)
    while not os.path.exists(SOCK): time.sleep(0.05)
    time.sleep(0.1)

def stop():
    subprocess.run([FUSERMOUNT, "-u", MNT], capture_output=True); P.wait(timeout=60)

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

with open(base, "wb") as f:
    for _ in range(TOTAL // 1024): f.write(os.urandom(1024 * MACRO))
    f.write(os.urandom((TOTAL % 1024) * MACRO))
start()
data = {}
for n in ("a", "b", "c"):
    cmd("create-internal %s CBC_ESSIV(AES256) %s" % (n, key(n)))
    cmd("resize-internal %s 20 5" % n)
    data[n] = os.urandom(15 * MMPM * MESO)
    with open(MNT + "/" + n, "r+b") as f: f.write(data[n]); f.flush(); os.fsync(f.fileno())
    cmd("close " + n)
stop()
subprocess.run(["sync"]); open("/proc/sys/vm/drop_caches", "w").write("1\n")

start()
times, bad = [], 0
for rnd in (1, 2):
    for n in ("a", "b", "c", "nothing"):
        t0 = time.monotonic()
        r = cmd("open-internal %s CBC_ESSIV(AES256) %s" % (n, key(n)), ok=False)
        times.append((rnd, n, time.monotonic() - t0))
        if n == "nothing":
            if r.startswith("OK"): bad += 1
            continue
        with open(MNT + "/" + n, "rb") as f:
            if f.read(len(data[n])) != data[n]: bad += 1
        cmd("close " + n)
stop()
os.unlink(base)
for rnd, n, t in times: print("open %d of %-8s %7.3fs" % (rnd, n, t), flush=True)
print("%d blocks of %d KiB: first open %.3fs, later opens %.3fs on average" % (TOTAL,
      MACRO >> 10, times[0][2], sum(t for _, _, t in times[1:]) / (len(times) - 1)), flush=True)
print("OPENTIME %s" % ("OK" if not bad else "FAILED: %d partitions wrong" % bad), flush=True)
