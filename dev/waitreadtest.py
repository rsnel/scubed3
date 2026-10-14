#!/usr/bin/env python3
# waitreadtest.py - in paranoia level 2 and 3 a writer waits for its turn,
# reads of its partition (and of the other partitions) must go on meanwhile
#
# partition a is written as fast as possible by one thread (in level 3 at a
# low rate it waits for its turn most of the time), other threads read a
# and b and measure how long a read takes; every mesoblock contains its
# index and a version, the rest is derived from them, so a read of a
# mesoblock that is being rewritten must return a complete version of it
# (old or new), never a mix or another mesoblock; at the end (also after a
# restart) a and b must contain exactly what was written
#
# usage: waitreadtest.py DAEMON SECONDS SEED LEVEL [KIBPS] [MAXREAD]
# (small blocks: -m 12 -M 16, 64 KiB macroblocks; level 3 with KIBPS,
# default 640: 10 writes per second; with MAXREAD (seconds) the test fails
# if a read of a takes longer)
import os, sys, time, socket, random, hashlib, subprocess, errno, threading, atexit
import shutil, struct
FUSERMOUNT = shutil.which("fusermount3") or "fusermount"   # libfuse 3 or 2

B = os.environ.get("S3BUILD", os.getcwd())
MNT, SOCK = "/mnt/s3", "/run/scubed3/control"
M_LOG, MM_LOG = 12, 16
MESO, MACRO = 1 << M_LOG, 1 << MM_LOG
MMPM = MACRO // MESO - 1
TOTAL = 200
PARTS = {"a": 40, "b": 40}
daemon, seconds, seed, level = sys.argv[1], float(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4])
kibps = int(sys.argv[5]) if len(sys.argv) > 5 else 640
maxread = float(sys.argv[6]) if len(sys.argv) > 6 else None
LEVEL = "paranoia %d" % level + (" %d" % kibps if level == 3 else "")
base, logname = B + "/waitread.base", B + "/waitread.log"
log = open(logname, "w")
P = None
atexit.register(lambda: P and P.poll() is None and P.kill())

def key(name): return hashlib.pbkdf2_hmac("sha256", name.encode(), b"scubed3_prod", 1000, 32).hex()
def reserved(n): return max(1, n // 4)
def usable(n): return (n - reserved(n)) * MMPM

def start():
    global P
    subprocess.run([FUSERMOUNT, "-u", "-z", MNT], capture_output=True)
    if os.path.exists(SOCK): os.unlink(SOCK)
    P = subprocess.Popen([B + "/" + daemon, "-b", base, "-m", str(M_LOG), "-M", str(MM_LOG),
        "-f", "-o", "direct_io", MNT], stdout=log, stderr=log)
    while not os.path.exists(SOCK):
        if P.poll() is not None: raise RuntimeError("daemon did not start")
        time.sleep(0.05)
    time.sleep(0.1)

def stop():
    subprocess.run([FUSERMOUNT, "-u", MNT], capture_output=True)
    try: P.wait(timeout=120)
    except subprocess.TimeoutExpired: P.kill(); P.wait()

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

def opent(name, flags):
    for _ in range(400):
        try: return os.open(MNT + "/" + name, flags)
        except OSError as e:
            if e.errno != errno.EBUSY: raise
            time.sleep(0.05)
    raise RuntimeError("busy")

def content(name, m, v):
    """mesoblock m of partition name in version v (v = 0: never written)"""
    if v == 0: return bytes(MESO)
    head = struct.pack(">4sII", name.encode().ljust(4), m, v)
    h = hashlib.sha256(head).digest()
    return head + (h * (MESO // 32))[:MESO - len(head)]

def version(name, m, d):
    """the version of mesoblock m in data d, or None if it is not a version"""
    if d == bytes(MESO): return 0
    n, mm, v = struct.unpack(">4sII", d[:12])
    if n != name.encode().ljust(4) or mm != m or d != content(name, m, v): return None
    return v

errors = []
def fail(what):
    print("   FAILED: " + what, flush=True); errors.append(what)

# ------------------------------------------------------------- setup
with open(base, "wb") as f: f.write(os.urandom(TOTAL * MACRO))
start()
ver = {n: [0] * usable(PARTS[n]) for n in PARTS}  # the last version written
lock = threading.Lock()
for n, nb in PARTS.items():
    cmd("create-internal %s CBC_ESSIV(AES256) %s" % (n, key(n)))
    cmd("resize-internal %s %d %d" % (n, nb, reserved(nb)))
    fd = opent(n, os.O_RDWR)
    for m in range(usable(nb)):
        os.pwrite(fd, content(n, m, 1), m * MESO); ver[n][m] = 1
    os.fsync(fd); os.close(fd)
cmd(LEVEL)

stop_ev = threading.Event()
stats = {"writes": 0, "reads": {n: [] for n in PARTS}}

def writer():
    r = random.Random(seed)
    fd = opent("a", os.O_RDWR)
    try:
        while not stop_ev.is_set():
            m = r.randrange(usable(PARTS["a"]))
            with lock: v = ver["a"][m] + 1
            os.pwrite(fd, content("a", m, v), m * MESO)
            with lock: ver["a"][m] = v
            stats["writes"] += 1
    except Exception as e: fail("writer: %s" % e)
    finally: os.close(fd)

def reader(n, k):
    r = random.Random(seed * 13 + k)
    fd = opent(n, os.O_RDONLY)
    try:
        while not stop_ev.is_set():
            m = r.randrange(usable(PARTS[n]))
            with lock: low = ver[n][m]   # the version is at least this one
            t0 = time.monotonic()
            d = os.pread(fd, MESO, m * MESO)
            stats["reads"][n].append(time.monotonic() - t0)
            with lock: high = ver[n][m] + 1  # or one that is being written
            v = version(n, m, d)
            if v is None or not low <= v <= high:
                fail("read of %s mesoblock %d: version %s, expected %d..%d" % (n, m, v, low, high))
                return
            time.sleep(0.02)
    except Exception as e: fail("reader %s: %s" % (n, e))
    finally: os.close(fd)

th = [threading.Thread(target=writer)] + [threading.Thread(target=reader, args=(n, k))
        for k, n in enumerate(["a", "a", "b"])]
for t in th: t.start()
time.sleep(seconds)
stop_ev.set()
for t in th: t.join(300)

for n in PARTS:
    l = sorted(stats["reads"][n])
    if not l: fail("no reads of %s" % n); continue
    print("reads of %s: %d, median %.1f ms, 99%% %.1f ms, max %.1f ms" % (n, len(l),
          1000 * l[len(l) // 2], 1000 * l[int(len(l) * .99)], 1000 * l[-1]), flush=True)
print("%d mesoblock writes to a in %.0fs (level %d)" % (stats["writes"], seconds, level), flush=True)
if maxread is not None and stats["reads"]["a"] and max(stats["reads"]["a"]) > maxread:
    fail("a read of a took %.1fs (more than %.1fs)" % (max(stats["reads"]["a"]), maxread))

# ------------------------------------------------------------- check
def verify(n, when):
    fd = opent(n, os.O_RDONLY)
    for m in range(usable(PARTS[n])):
        d = os.pread(fd, MESO, m * MESO)
        if d != content(n, m, ver[n][m]):
            fail("%s %s: mesoblock %d is version %s, expected %d" % (when, n, m, version(n, m, d), ver[n][m]))
            break
    os.close(fd)
for n in PARTS: verify(n, "at the end")
for n in PARTS: cmd("close " + n)
stop(); start()
for n in PARTS:
    cmd("open-internal %s CBC_ESSIV(AES256) %s" % (n, key(n)))
    verify(n, "after a restart")
for n in PARTS: cmd("close " + n)
stop()
os.unlink(base)
print("WAITREADTEST %s" % ("OK" if not errors else "FAILED: %s" % errors[:5]), flush=True)
