#!/usr/bin/env python3
# churntest.py - partitions are created, enlarged, shrunk and destroyed while
# other partitions are written as fast as possible, in paranoia level 2 or 3
#
# two partitions (a and b) are written by two threads all the time; a third
# thread creates partition c (with a new passphrase every round), gives it a
# random size, writes and checks it, enlarges it, writes and checks it again,
# shrinks it to 0 blocks (it is destroyed) and closes it; the level must stay
# the same (closing a partition without blocks does not switch it off), the
# block counts of the status must add up, and at the end (also after a
# restart) a and b must contain exactly what was written
#
# usage: churntest.py DAEMON ROUNDS SEED LEVEL [KIBPS]
# (small blocks: -m 12 -M 16, 64 KiB macroblocks; level 3 with KIBPS, default
# 6400: 100 writes per second)
import os, sys, time, socket, random, hashlib, subprocess, errno, threading, atexit

B = os.environ.get("S3BUILD", os.getcwd())
MNT, SOCK = "/mnt/s3", "/run/scubed3/control"
M_LOG, MM_LOG = 12, 16
MESO, MACRO = 1 << M_LOG, 1 << MM_LOG
MMPM = MACRO // MESO - 1
TOTAL = 400
PARTS = {"a": 80, "b": 120}             # free: 200, c takes up to 120 of it
daemon, rounds, seed, level = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4])
kibps = int(sys.argv[5]) if len(sys.argv) > 5 else 6400
LEVEL = "paranoia %d" % level + (" %d" % kibps if level == 3 else "")
rng = random.Random(seed)
base, logname = B + "/churn.base", B + "/churn.log"
log = open(logname, "w")
P = None
atexit.register(lambda: P and P.poll() is None and P.kill())
lock = threading.Lock()                 # for the models

def key(name): return hashlib.pbkdf2_hmac("sha256", name.encode(), b"scubed3_prod", 1000, 32).hex()
def reserved(n): return max(1, n // 4)
def usable(n): return (n - reserved(n)) * MMPM if n else 0

def start():
    global P
    subprocess.run(["fusermount", "-u", "-z", MNT], capture_output=True)
    if os.path.exists(SOCK): os.unlink(SOCK)
    P = subprocess.Popen([B + "/" + daemon, "-b", base, "-m", str(M_LOG), "-M", str(MM_LOG),
        "-f", "-o", "direct_io", MNT], stdout=log, stderr=log)
    while not os.path.exists(SOCK):
        if P.poll() is not None: raise RuntimeError("daemon did not start")
        time.sleep(0.05)
    time.sleep(0.1)

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

def info(n):
    return dict(l.split("=", 1) for l in cmd("info " + n).split("\n") if "=" in l)

def opent(name, flags):
    for _ in range(400):
        try: return os.open(MNT + "/" + name, flags)
        except OSError as e:
            if e.errno != errno.EBUSY: raise
            time.sleep(0.05)
    raise RuntimeError("busy")

def verify(name, sz, model):
    fd = opent(name, os.O_RDONLY); data = os.pread(fd, sz * MESO, 0); os.close(fd)
    if len(data) != sz * MESO: return ["size %d != %d" % (len(data) // MESO, sz)]
    return [m for m in range(sz) if data[m * MESO:(m + 1) * MESO] != model.get(m, bytes(MESO))]

errors = []
def fail(what):
    print("   FAILED: " + what, flush=True); errors.append(what)

def check_counts(names):
    """the blocks of the open partitions and the unclaimed blocks add up"""
    st = cmd("p")
    blocks = sum(int(l.split()[0]) for l in st.split("\n") if " blocks in " in l)
    unclaimed = int([l for l in st.split("\n") if "unclaimed" in l][0].split()[0])
    if blocks + unclaimed != TOTAL: fail("block counts: %d + %d != %d" % (blocks, unclaimed, TOTAL))
    if ("paranoia level %d" % level) not in st: fail("level changed: %s" % st)

# ------------------------------------------------------------- setup
with open(base, "wb") as f: f.write(os.urandom(TOTAL * MACRO))
start()
model = {n: {} for n in PARTS}
for n, nb in PARTS.items():
    cmd("create-internal %s CBC_ESSIV(AES256) %s" % (n, key(n)))
    cmd("resize-internal %s %d %d" % (n, nb, reserved(nb)))
cmd(LEVEL)

stop = threading.Event(); writes = {n: 0 for n in PARTS}
def writer(n):
    r = random.Random(seed * 7 + ord(n))
    fd = opent(n, os.O_RDWR)
    try:
        while not stop.is_set():
            m = r.randrange(usable(PARTS[n])); d = os.urandom(MESO)
            os.pwrite(fd, d, m * MESO)
            with lock: model[n][m] = d
            writes[n] += 1
            if r.random() < 0.01: os.fsync(fd)
    except Exception as e: fail("writer %s: %s" % (n, e))
    finally: os.close(fd)
th = [threading.Thread(target=writer, args=(n,)) for n in PARTS]
for t in th: t.start()

# ------------------------------------------------------------- churn
t0 = time.monotonic(); created = 0
for r in range(rounds):
    name, k = "c", key("c%d-%d" % (seed, r))
    try:
        cmd("create-internal %s CBC_ESSIV(AES256) %s" % (name, k))
        nb = rng.randint(2, 60)
        cmd("resize-internal %s %d %d" % (name, nb, reserved(nb)))
        cm = {}
        for phase in (0, 1):
            fd = opent(name, os.O_RDWR)
            for m in rng.sample(range(usable(nb)), min(usable(nb), rng.randint(1, 200))):
                d = os.urandom(MESO); os.pwrite(fd, d, m * MESO); cm[m] = d
            os.fsync(fd); os.close(fd)
            bad = verify(name, usable(nb), cm)
            if bad: fail("round %d: c (%d blocks) has %d bad mesoblocks" % (r, nb, len(bad)))
            if phase == 0:              # enlarge
                nb = min(120, nb + rng.randint(1, 60))
                cmd("resize-internal %s %d %d" % (name, nb, reserved(nb)))
        check_counts(("a", "b", "c"))
        # destroy: shrink to 0, the removal needs writes of c
        cmd("resize-internal %s 0 0" % name)
        for _ in range(2000):
            i = info(name)
            if int(i["pending_removals"]) == 0 and int(i["no_macroblocks"]) == 0: break
            cmd("cycle %s 1" % name)
        else: fail("round %d: c does not shrink to 0" % r)
        cmd("close " + name)
        check_counts(("a", "b"))
        created += 1
    except RuntimeError as e:
        fail("round %d: %s" % (r, e)); break
    if P.poll() is not None: fail("daemon died"); break
el = time.monotonic() - t0
stop.set()
for t in th: t.join(120)
print("%d rounds of create/enlarge/destroy of c in %.0fs, while a and b were written "
      "(%d and %d mesoblock writes)" % (created, el, writes["a"], writes["b"]), flush=True)

# ------------------------------------------------------------- check
for n in PARTS:
    bad = verify(n, usable(PARTS[n]), model[n])
    if bad: fail("%s: %d bad mesoblocks" % (n, len(bad)))
for n in PARTS: cmd("close " + n)
subprocess.run(["fusermount", "-u", MNT]); P.wait(timeout=120)
start()
for n in PARTS:
    cmd("open-internal %s CBC_ESSIV(AES256) %s" % (n, key(n)))
    bad = verify(n, usable(PARTS[n]), model[n])
    if bad: fail("%s after restart: %d bad mesoblocks" % (n, len(bad)))
st = cmd("p")
unclaimed = int([l for l in st.split("\n") if "unclaimed" in l][0].split()[0])
if unclaimed != TOTAL - sum(PARTS.values()):
    fail("after restart: %d blocks unclaimed, expected %d" % (unclaimed, TOTAL - sum(PARTS.values())))
for n in PARTS: cmd("close " + n)
subprocess.run(["fusermount", "-u", MNT]); P.wait(timeout=120)
os.unlink(base)
print("CHURNTEST %s" % ("OK" if not errors else "FAILED: %s" % errors[:5]), flush=True)
