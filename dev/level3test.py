#!/usr/bin/env python3
# level3test.py - paranoia level 3: the block writes happen at random moments,
# on average at the configured rate, whatever is written
#
#  1. idle: the writes come at the configured rate, the times between them are
#     exponentially distributed (chi-square over 10 bins of equal probability)
#  2. busy (a writer writes as fast as it can to one partition): the same
#     rate and distribution of the times between writes (the number of writes
#     is compared with the idle period), the written blocks uniform over the
#     base device, the throughput of the partition at most its share of the
#     rate
#  3. data correct with fsync, a clean restart and kill -9
#  4. a writer that waits for its turn does not block the paranoia command:
#     switching to level 1 lets it finish at once
#  5. scubed3ctl paranoia 3 RATE, the status shows the rate, closing a
#     partition switches back to level 1 and the writes stop
#
# usage: level3test.py DAEMON-crash SCUBED3CTL SEED
# (small blocks are used: -m 12 -M 16, 64 KiB macroblocks)
import os, sys, time, socket, random, hashlib, subprocess, errno, math, threading, atexit
import shutil
FUSERMOUNT = shutil.which("fusermount3") or "fusermount"   # libfuse 3 or 2

B = os.environ.get("S3BUILD", os.getcwd())
MNT, SOCK = "/mnt/s3", "/run/scubed3/control"
M_LOG, MM_LOG = 12, 16
MESO, MACRO = 1 << M_LOG, 1 << MM_LOG
MMPM = MACRO // MESO - 1
TOTAL = 300
PARTS = {"a": 60, "b": 120}             # blocks (free: 120)
KIBPS = 640                             # 10 block writes per second
MEAN = MACRO / (KIBPS << 10)
daemon, ctl, seed = sys.argv[1], sys.argv[2], int(sys.argv[3])
rng = random.Random(seed)
base, wlog, logname = B + "/level3.base", B + "/level3.wlog", B + "/level3.log"
log = open(logname, "w")
P = None
atexit.register(lambda: P and P.poll() is None and P.kill())

def key(name): return hashlib.pbkdf2_hmac("sha256", name.encode(), b"scubed3_prod", 1000, 32).hex()
def reserved(n): return n // 4
def size(n): return (PARTS[n] - reserved(PARTS[n])) * MMPM

def start():
    global P
    subprocess.run([FUSERMOUNT, "-u", "-z", MNT], capture_output=True)
    if os.path.exists(SOCK): os.unlink(SOCK)
    P = subprocess.Popen([B + "/" + daemon, "-b", base, "-m", str(M_LOG), "-M", str(MM_LOG),
        "-f", "-o", "direct_io", MNT], stdout=log, stderr=log,
        env=dict(os.environ, SCUBED3_WRITE_LOG=wlog, SCUBED3_WRITE_LOG_TIME="1"))
    while not os.path.exists(SOCK):
        if P.poll() is not None: raise RuntimeError("daemon did not start")
        time.sleep(0.05)
    time.sleep(0.1)

def stop():
    for n in PARTS: cmd("close " + n, ok=False)
    subprocess.run([FUSERMOUNT, "-u", MNT], check=True); P.wait(timeout=60)

def kill():
    P.kill(); P.wait()
    subprocess.run([FUSERMOUNT, "-u", "-z", MNT], capture_output=True)

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
    for _ in range(200):
        try: return os.open(MNT + "/" + name, flags)
        except OSError as e:
            if e.errno != errno.EBUSY: raise
            time.sleep(0.05)
    raise RuntimeError("busy")

def open_all():
    for n in PARTS: cmd("open-internal %s CBC_ESSIV(AES256) %s" % (n, key(n)))

def wlog_read():
    """[(block, time)]"""
    try: return [(int(o) // MACRO, float(t)) for o, t in (l.split() for l in open(wlog))]
    except FileNotFoundError: return []

def gaps_chi2(times):
    """chi-square of the times between writes against the exponential
    distribution with mean MEAN, 10 bins of equal probability"""
    g = [b - a for a, b in zip(times, times[1:])]
    edges = [-MEAN * math.log(1 - k / 10) for k in range(1, 10)]
    counts = [0] * 10
    for x in g: counts[sum(x > e for e in edges)] += 1
    e = len(g) / 10
    c2 = sum((c - e) ** 2 / e for c in counts)
    if os.environ.get("VERBOSE"): print("   bins of the times between writes:", counts)
    return len(g), sum(g) / max(len(g), 1), c2, (c2 - 9) / math.sqrt(18)

def zpois(n, expected): return (n - expected) / math.sqrt(expected)

errors = []
def check(cond, what):
    print(("   ok: " if cond else "   FAILED: ") + what, flush=True)
    if not cond: errors.append(what)

# ------------------------------------------------------------- setup
with open(base, "wb") as f: f.write(os.urandom(TOTAL * MACRO))
if os.path.exists(wlog): os.unlink(wlog)
start()
model = {n: {} for n in PARTS}
for n, nb in PARTS.items():
    cmd("create-internal %s CBC_ESSIV(AES256) %s" % (n, key(n)))
    cmd("resize-internal %s %d %d" % (n, nb, reserved(nb)))
    fd = opent(n, os.O_RDWR)
    for m in rng.sample(range(size(n)), size(n) // 2):
        d = os.urandom(MESO); os.pwrite(fd, d, m * MESO); model[n][m] = d
    os.fsync(fd); os.close(fd)
cmd("paranoia 3 %d" % KIBPS)
st = cmd("p")
check("paranoia level 3" in st and "level 3 rate %d KiB/s" % KIBPS in st, "level 3 with %d KiB/s "
      "(one 64 KiB block every %.2fs)" % (KIBPS, MEAN))

# ------------------------------------------------------------- 1. idle
T = 30
time.sleep(1); w0 = len(wlog_read()); t0 = time.monotonic()
time.sleep(T)
idle = wlog_read()[w0:]
n, mean, c2, zg = gaps_chi2([t for _, t in idle])
z = zpois(len(idle), T / MEAN)
print("1. idle for %ds: %d writes (expected %.0f, z=%.2f), mean time between writes %.3fs, "
      "exponential: chi2=%.1f df=9 z=%.2f" % (T, len(idle), T / MEAN, z, mean, c2, zg), flush=True)
check(abs(z) < 4 and abs(zg) < 4, "idle writes at the rate, exponential times between them")

# ------------------------------------------------------------- 2. busy
stop_writer = threading.Event(); written = [0]
def writer():
    fd = opent("a", os.O_RDWR)
    while not stop_writer.is_set():
        m = rng.randrange(size("a")); d = os.urandom(MESO)
        os.pwrite(fd, d, m * MESO); model["a"][m] = d; written[0] += 1
        if rng.random() < 0.02: os.fsync(fd)
    os.fsync(fd); os.close(fd)
w0 = len(wlog_read()); t0 = time.monotonic()
th = threading.Thread(target=writer); th.start()
time.sleep(T)
busy = [x for x in wlog_read()[w0:]]
stop_writer.set(); th.join(60)
if th.is_alive(): raise RuntimeError("writer does not finish")
n, mean, c2, zg = gaps_chi2([t for _, t in busy])
z = zpois(len(busy), T / MEAN)
share = PARTS["a"] / TOTAL * KIBPS
print("2. writing to a as fast as possible for %ds: %d writes (expected %.0f, z=%.2f), mean "
      "time between writes %.3fs, exponential: chi2=%.1f df=9 z=%.2f" % (T, len(busy), T / MEAN, z,
      mean, c2, zg), flush=True)
check(abs(z) < 4 and abs(zg) < 4, "busy: the same rate and distribution")
zc = (len(busy) - len(idle)) / math.sqrt(len(busy) + len(idle))
check(abs(zc) < 4, "idle and busy: same number of writes (z=%.2f)" % zc)
kib = written[0] * MESO / 1024 / T
print("   a wrote %.1f KiB/s, its share of the rate is %.1f KiB/s" % (kib, share), flush=True)
check(kib <= share * 1.3 and kib > 0, "the throughput of a is at most its share")
# locations: 10 groups of blocks, everything since level 3
allw = idle + busy
counts = [0] * 10
for b, _ in allw: counts[b * 10 // TOTAL] += 1
e = len(allw) / 10
c2 = sum((c - e) ** 2 / e for c in counts)
check(abs((c2 - 9) / math.sqrt(18)) < 4, "written blocks uniform over the base device "
      "(10 groups, chi2=%.1f df=9)" % c2)

# ------------------------------------------------------------- 3. correctness
def verify(n):
    fd = opent(n, os.O_RDONLY); data = os.pread(fd, size(n) * MESO, 0); os.close(fd)
    return [m for m in range(size(n)) if data[m * MESO:(m + 1) * MESO] != model[n].get(m, bytes(MESO))]
bad = sum(len(verify(n)) for n in PARTS)
check(bad == 0, "data correct in level 3")
stop(); start(); open_all(); cmd("paranoia 3 %d" % KIBPS)
bad = sum(len(verify(n)) for n in PARTS)
check(bad == 0, "data correct after a clean restart")
for n in PARTS:
    fd = opent(n, os.O_RDWR)
    for _ in range(40):
        m = rng.randrange(size(n)); d = os.urandom(MESO); os.pwrite(fd, d, m * MESO); model[n][m] = d
    os.fsync(fd); os.close(fd)
kill(); start(); open_all()
bad = sum(len(verify(n)) for n in PARTS)
check(bad == 0, "fsynced data correct after kill -9")
cmd("paranoia 3 %d" % KIBPS)

# ------------------------------------------------------------- 4. leaving level 3
cmd("paranoia 3 1")                     # one block every 64 s on average
done = threading.Event()
def slow():
    fd = opent("a", os.O_RDWR)
    for _ in range(3 * MMPM):           # more than a block: it has to wait
        m = rng.randrange(size("a")); d = os.urandom(MESO); os.pwrite(fd, d, m * MESO); model["a"][m] = d
    os.fsync(fd); os.close(fd); done.set()
th = threading.Thread(target=slow); th.start()
time.sleep(2)
waiting = not done.is_set()
t1 = time.monotonic(); cmd("paranoia 1"); tc = time.monotonic() - t1
done.wait(10); tw = time.monotonic() - t1
th.join(10)
check(waiting and tc < 1 and done.is_set() and tw < 5, "a writer waits for its turn, the paranoia "
      "command answers at once (%.2fs), the writer finishes in level 1 (%.2fs)" % (tc, tw))
check(sum(len(verify(n)) for n in PARTS) == 0, "data correct")

# ------------------------------------------------------------- 5. scubed3ctl, close
r = subprocess.run([ctl, "-q", "-YYY", "-c", "paranoia 3 %d" % KIBPS], capture_output=True, text=True)
st = cmd("p")
check("paranoia level 3" in st and "level 3 rate %d KiB/s" % KIBPS in st, "scubed3ctl paranoia 3 %d"
      % KIBPS)
cmd("close b")
st = cmd("p")
time.sleep(0.5); w0 = len(wlog_read()); time.sleep(3); w1 = len(wlog_read())
check("paranoia level 1" in st and w1 == w0, "closing a partition switches back to level 1, "
      "no more writes (%d)" % (w1 - w0))
cmd("close a")
subprocess.run([FUSERMOUNT, "-u", MNT]); P.wait(timeout=60)
os.unlink(base)
print("LEVEL3TEST %s" % ("OK" if not errors else "FAILED: %s" % errors), flush=True)
