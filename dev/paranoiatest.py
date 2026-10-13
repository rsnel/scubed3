#!/usr/bin/env python3
# paranoiatest.py - paranoia level 2: all writes go to uniformly random
# blocks of the whole base device, whichever partition is written
#
#  1. statistics: three partitions and free space, writes to one partition
#     only, then to another one only: the written blocks must be uniform over
#     the base device (chi-square), consecutive writes independent
#     (contingency table), the same for both workloads
#  2. correctness: random writes, discards and fsyncs to all partitions,
#     restarts and kill -9, every partition checked against its model
#  3. scubed3ctl paranoia 2, closing a partition switches back to level 1,
#     a partition that is not open is overwritten in level 2 (as documented)
#
# usage: paranoiatest.py DAEMON-crash SCUBED3CTL SEED
# (small blocks are used: -m 12 -M 16, 64 KiB macroblocks)
import os, sys, time, socket, random, hashlib, subprocess, errno, ctypes, math
import shutil
FUSERMOUNT = shutil.which("fusermount3") or "fusermount"   # libfuse 3 or 2

B = os.environ.get("S3BUILD", os.getcwd())
MNT, SOCK = "/mnt/s3", "/run/scubed3/control"
M_LOG, MM_LOG = 12, 16
MESO, MACRO = 1 << M_LOG, 1 << MM_LOG
MMPM = MACRO // MESO - 1
TOTAL = 600
daemon, ctl, seed = sys.argv[1], sys.argv[2], int(sys.argv[3])
rng = random.Random(seed)
base, wlog = B + "/paranoia.base", B + "/paranoia.wlog"
log = open(B + "/paranoia.log", "w")
P = None
libc = ctypes.CDLL(None, use_errno=True)

def key(name): return hashlib.pbkdf2_hmac("sha256", name.encode(), b"scubed3_prod", 1000, 32).hex()

def start():
    global P
    subprocess.run([FUSERMOUNT, "-u", "-z", MNT], capture_output=True)
    if os.path.exists(SOCK): os.unlink(SOCK)
    P = subprocess.Popen([B + "/" + daemon, "-b", base, "-m", str(M_LOG), "-M", str(MM_LOG),
        "-f", "-o", "direct_io", MNT], stdout=log, stderr=log,
        env=dict(os.environ, SCUBED3_WRITE_LOG=wlog))
    while not os.path.exists(SOCK):
        if P.poll() is not None: raise RuntimeError("daemon did not start")
        time.sleep(0.05)
    time.sleep(0.1)

def stop():
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

def status(): return cmd("p")

def opent(name, flags):
    for _ in range(100):
        try: return os.open(MNT + "/" + name, flags)
        except OSError as e:
            if e.errno != errno.EBUSY: raise
            time.sleep(0.05)
    raise RuntimeError("busy")

PARTS = {"a": 60, "b": 180, "c": 120}          # blocks (free: 240)
def reserved(n): return n // 4

def open_all(names=PARTS):
    for n in names: cmd("open-internal %s CBC_ESSIV(AES256) %s" % (n, key(n)))

def size(n): return (PARTS[n] - reserved(PARTS[n])) * MMPM

def wlog_read():
    try: return [int(l) // MACRO for l in open(wlog)]
    except FileNotFoundError: return []

# ------------------------------------------------------------- setup
with open(base, "wb") as f: f.write(os.urandom(TOTAL * MACRO))
if os.path.exists(wlog): os.unlink(wlog)
start()
for n, nb in PARTS.items():
    cmd("create-internal %s CBC_ESSIV(AES256) %s" % (n, key(n)))
    cmd("resize-internal %s %d %d" % (n, nb, reserved(nb)))
model = {n: {} for n in PARTS}         # mesoblock -> data
for n in PARTS:                         # fill a part of every partition
    fd = opent(n, os.O_RDWR)
    for m in rng.sample(range(size(n)), size(n) // 2):
        d = os.urandom(MESO); os.pwrite(fd, d, m * MESO); model[n][m] = d
    os.fsync(fd); os.close(fd)

# ------------------------------------------------------------- 1. statistics
def chi2_uniform(counts, n):
    e = n / len(counts)
    return sum((c - e) ** 2 / e for c in counts), len(counts) - 1

def z(chi2, df): return (chi2 - df) / math.sqrt(2 * df)

def workload(name, writes):
    """write random mesoblocks of one partition until writes blocks were
    written to the base device, returns the written blocks"""
    before = len(wlog_read())
    fd = opent(name, os.O_RDWR)
    while len(wlog_read()) - before < writes:
        for _ in range(50):
            m = rng.randrange(size(name)); d = os.urandom(MESO)
            os.pwrite(fd, d, m * MESO); model[name][m] = d
        if rng.random() < 0.3: os.fsync(fd)
    os.close(fd)
    return wlog_read()[before:before + writes]

def stats(w):
    counts = [0] * TOTAL
    for b in w: counts[b] += 1
    c2, df = chi2_uniform(counts, len(w))
    return c2, df

# for comparison: level 1 writes only blocks of the partition that is written
w = workload("a", 3 * TOTAL)
c2, df = stats(w)
print("0. level 1 for comparison, writing only to a: %d block writes on %d distinct "
        "blocks, chi2=%.0f df=%d z=%.0f (not uniform over the base device)" %
        (len(w), len(set(w)), c2, df, z(c2, df)), flush=True)

cmd("paranoia 2")
if "paranoia level 2" not in status(): raise RuntimeError("level 2 not on")
results = []
for name in ("a", "c"):
    w = workload(name, 30 * TOTAL)
    counts = [0] * TOTAL
    for b in w: counts[b] += 1
    c2, df = chi2_uniform(counts, len(w))
    # consecutive writes: 10 groups of blocks, 10x10 contingency table
    G = 10
    tab = [[0] * G for _ in range(G)]
    for x, y in zip(w, w[1:]): tab[x * G // TOTAL][y * G // TOTAL] += 1
    rows = [sum(r) for r in tab]; cols = [sum(c) for c in zip(*tab)]; N = sum(rows)
    c2p = sum((tab[i][j] - rows[i] * cols[j] / N) ** 2 / (rows[i] * cols[j] / N)
            for i in range(G) for j in range(G))
    dfp = (G - 1) ** 2
    results.append((name, len(w), z(c2, df), z(c2p, dfp)))
    print("1. writing only to %s: %d block writes, uniform over all %d blocks: "
            "chi2=%.0f df=%d z=%.2f; consecutive writes independent: chi2=%.0f df=%d "
            "z=%.2f" % (name, len(w), TOTAL, c2, df, z(c2, df), c2p, dfp, z(c2p, dfp)),
            flush=True)
    if abs(z(c2, df)) > 4 or abs(z(c2p, dfp)) > 4:
        raise RuntimeError("writes are not uniform/independent")
st = status()
print("   " + [l for l in st.split("\n") if "paranoia" in l][0], flush=True)

# ------------------------------------------------------------- 2. correctness
def verify(n, allowed=None):
    fd = opent(n, os.O_RDONLY)
    data = os.pread(fd, size(n) * MESO, 0); os.close(fd)
    for m in range(size(n)):
        got = data[m * MESO:(m + 1) * MESO]
        if allowed is None:
            if got != model[n].get(m, bytes(MESO)):
                raise RuntimeError("%s: mesoblock %d wrong" % (n, m))
        elif got not in allowed[n].get(m, {model[n].get(m, bytes(MESO))}):
            raise RuntimeError("%s: mesoblock %d wrong after crash" % (n, m))
        else: model[n][m] = got

durable = {n: dict(model[n]) for n in PARTS}
window = {n: {} for n in PARTS}
def mark_durable(n): durable[n] = dict(model[n]); window[n] = {}

ops = dict(writes=0, discards=0, fsyncs=0, restarts=0, kills=0)
for rnd in range(30):
    for _ in range(rng.randint(1, 6)):
        n = rng.choice(list(PARTS)); fd = opent(n, os.O_RDWR)
        for _ in range(rng.randint(1, 80)):
            m = rng.randrange(size(n))
            if rng.random() < 0.15:
                k = rng.randint(1, 40); k = min(k, size(n) - m)
                if libc.fallocate(fd, 3, ctypes.c_long(m * MESO), ctypes.c_long(k * MESO)):
                    raise OSError(ctypes.get_errno(), "fallocate")
                for i in range(m, m + k):
                    model[n].pop(i, None); window[n].setdefault(i, set()).add(bytes(MESO))
                ops["discards"] += 1
            else:
                d = os.urandom(MESO); os.pwrite(fd, d, m * MESO); model[n][m] = d
                window[n].setdefault(m, set()).add(d); ops["writes"] += 1
        if rng.random() < 0.5: os.fsync(fd); mark_durable(n); ops["fsyncs"] += 1
        os.close(fd)
    r = rng.random()
    if r < 0.25:
        for n in PARTS: cmd("close " + n)
        for n in PARTS: mark_durable(n)
        stop(); start(); open_all(); cmd("paranoia 2"); ops["restarts"] += 1
        for n in PARTS: verify(n)
    elif r < 0.45:
        kill(); start(); open_all(); cmd("paranoia 2"); ops["kills"] += 1
        for n in PARTS:
            allowed = {m: {durable[n].get(m, bytes(MESO))} | window[n].get(m, set())
                    for m in range(size(n))}
            verify(n, {n: allowed}); mark_durable(n)
for n in PARTS: verify(n)
print("2. correctness with three partitions in level 2: %s, all data correct" % ops,
        flush=True)

# ------------------------------------------------------------- 2b. concurrency
import threading
errors = []
def writer(n, secs):
    try:
        end = time.time() + secs
        while time.time() < end:
            fd = opent(n, os.O_RDWR)
            for _ in range(rng.randint(1, 40)):
                m = rng.randrange(size(n)); d = os.urandom(MESO)
                os.pwrite(fd, d, m * MESO); model[n][m] = d
            if rng.random() < 0.5: os.fsync(fd)
            os.close(fd)
    except Exception as e: errors.append((n, e))
def prober(secs):
    try:
        end = time.time() + secs
        while time.time() < end:
            cmd("info " + rng.choice(list(PARTS))); status(); time.sleep(0.05)
    except Exception as e: errors.append(("prober", e))
w0 = len(wlog_read())
th = [threading.Thread(target=writer, args=(n, 40)) for n in PARTS] + \
     [threading.Thread(target=prober, args=(40,))]
for t in th: t.start()
for t in th: t.join(120)
if any(t.is_alive() for t in th): raise RuntimeError("deadlock: threads still running")
if errors: raise RuntimeError("concurrency errors: %s" % errors)
for n in PARTS: verify(n)
print("2b. three writers and a control prober at the same time for 40s: %d block "
        "writes, no deadlock, all data correct" % (len(wlog_read()) - w0), flush=True)

# ------------------------------------------------------------- 3. scubed3ctl, close
cmd("paranoia 1")
r = subprocess.run([ctl, "-q", "-YYY", "-c", "paranoia 2"], capture_output=True, text=True)
if "paranoia level 2" not in status(): raise RuntimeError("scubed3ctl paranoia 2: %s %s"
        % (r.stdout, r.stderr))
cmd("close c")
if "paranoia level 1" not in status(): raise RuntimeError("level not back to 1 at close")
print("3. scubed3ctl paranoia 2 OK, closing a partition switches back to level 1",
        flush=True)
# c is not open: level 2 overwrites its blocks (that is why all partitions
# must be open), a with level 1 would not
cmd("paranoia 2")
workload("a", 4 * TOTAL)
for n in ("a", "b"): verify(n)
try: t = cmd("open-internal c CBC_ESSIV(AES256) %s" % key("c"), ok=False)
except OSError: t = "daemon died"
time.sleep(0.5)
died = P.poll() is not None
lost = died or "ERR" in t
if not lost:
    fd = opent("c", os.O_RDONLY); data = os.pread(fd, size("c") * MESO, 0); os.close(fd)
    lost = any(data[m * MESO:(m + 1) * MESO] != model["c"].get(m, bytes(MESO)) for m in range(size("c")))
print("   c was not open during level 2: %s (as documented)" %
        ("its blocks were overwritten" if lost else "survived?!"), flush=True)
if not lost: raise RuntimeError("c survived level 2 while closed?")
if died: subprocess.run([FUSERMOUNT, "-u", "-z", MNT], capture_output=True)
else:
    for n in ("a", "b"): cmd("close " + n)
    try: cmd("close c")
    except RuntimeError: pass
    stop()
os.unlink(base)
print("PARANOIATEST OK", flush=True)
