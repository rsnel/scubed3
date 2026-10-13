#!/usr/bin/env python3
# resizestat.py - paranoia level 2 while partitions are resized: the written
# blocks must stay uniform over the base device
#
# a partition is grown and shrunk again many times while it is written to;
# the blocks that are added by a grow are checked in the writes right after
# the grow (they must be hit as often as any other block), the blocks that
# are removed by a shrink are checked while the shrink is running, and all
# writes together must be uniform (chi-square) and consecutive writes
# independent
#
# (a partition cannot be resized while it is in use, so the writes stop
# for a moment)
#
# usage: resizestat.py DAEMON-crash ROUNDS SEED
# (small blocks are used: -m 12 -M 16, 64 KiB macroblocks)
import os, sys, time, socket, random, hashlib, subprocess, errno, math, re

B = os.environ.get("S3BUILD", os.getcwd())
MNT, SOCK = "/mnt/s3", "/run/scubed3/control"
M_LOG, MM_LOG = 12, 16
MESO, MACRO = 1 << M_LOG, 1 << MM_LOG
MMPM = MACRO // MESO - 1
TOTAL = 400
daemon, rounds, seed = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
rng = random.Random(seed)
base, wlog, logname = B + "/resizestat.base", B + "/resizestat.wlog", B + "/resizestat.log"
log = open(logname, "w")
P = None
PARTS = {"a": 40, "b": 120}     # blocks (free: 240)
GROW = 20                       # a: 40 <-> 60
K = 30                          # writes after a grow that are checked
S = 60                          # writes during a shrink that are checked
S0 = 40                         # after the first S0 writes (see below)

def key(name): return hashlib.pbkdf2_hmac("sha256", name.encode(), b"scubed3_prod", 1000, 32).hex()
def reserved(n): return max(1, n // 4)

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

def wlog_read():
    try: return [int(l) // MACRO for l in open(wlog)]
    except FileNotFoundError: return []

def pool(name, live=False):
    """the blocks of the partition (from verbose-juggler in the log), with
    live: a dict block -> number of live mesoblocks"""
    off = os.path.getsize(logname)
    cmd("verbose-juggler " + name)
    for _ in range(200):
        with open(logname, "rb") as f:
            f.seek(off); txt = f.read().decode(errors="replace")
        m = re.search(r"--juggler--.*?no_pool=(\d+)", txt, re.S)
        if m:
            e = re.findall(r":\d+ \[(\d+) +\d+ (\d+)\]", txt[m.end():])
            if len(e) >= int(m.group(1)):
                e = {int(b): int(l) for b, l in e[:int(m.group(1))]}
                return e if live else set(e)
        time.sleep(0.02)
    raise RuntimeError("no juggler output")

def opent(name, flags):
    for _ in range(100):
        try: return os.open(MNT + "/" + name, flags)
        except OSError as e:
            if e.errno != errno.EBUSY: raise
            time.sleep(0.05)
    raise RuntimeError("busy")

def usable(nb): return (nb - reserved(nb)) * MMPM

def write_some(fd, nb, n=5):
    for _ in range(n):
        os.pwrite(fd, os.urandom(MESO), rng.randrange(usable(nb)) * MESO)

subprocess.run(["fusermount", "-u", "-z", MNT], capture_output=True)
if os.path.exists(SOCK): os.unlink(SOCK)
with open(base, "wb") as f: f.write(os.urandom(TOTAL * MACRO))
if os.path.exists(wlog): os.unlink(wlog)
P = subprocess.Popen([B + "/" + daemon, "-b", base, "-m", str(M_LOG), "-M", str(MM_LOG),
    "-f", "-o", "direct_io", MNT], stdout=log, stderr=log,
    env=dict(os.environ, SCUBED3_WRITE_LOG=wlog))
while not os.path.exists(SOCK):
    if P.poll() is not None: raise RuntimeError("daemon did not start")
    time.sleep(0.05)
time.sleep(0.1)
import atexit
atexit.register(lambda: P.poll() is None and P.kill())
for n, nb in PARTS.items():
    cmd("create-internal %s CBC_ESSIV(AES256) %s" % (n, key(n)))
    cmd("resize-internal %s %d %d" % (n, nb, reserved(nb)))
    fd = opent(n, os.O_RDWR)
    for m in range(usable(nb)): os.pwrite(fd, os.urandom(MESO), m * MESO)
    os.fsync(fd); os.close(fd)
cmd("paranoia 2")

start = len(wlog_read())
grow_hits = grow_exp = 0.0
shrink_hits = shrink_exp = 0.0
predicted = 0
for r in range(rounds):
    nb = PARTS["a"]
    old = pool("a")
    cmd("resize-internal a %d %d" % (nb + GROW, reserved(nb + GROW)))
    new = pool("a") - old
    assert len(new) == GROW, (len(new), r)
    p0 = len(wlog_read())
    fd = opent("a", os.O_RDWR)
    while len(wlog_read()) < p0 + K: write_some(fd, nb + GROW)
    os.close(fd)
    w = wlog_read()[p0:p0 + K]
    grow_hits += sum(b in new for b in w); grow_exp += K * GROW / TOTAL

    # shrink back: the blocks that will be removed are not known in
    # advance (they are chosen by the amount of live data while shrinking,
    # so a block that is written while shrinking is not removed: counting
    # hits on the blocks that are removed in the end is biased), the GROW
    # blocks of the partition that were written longest ago are counted
    # instead (they hold the least live data, most of them are removed),
    # only things that are known before are used (the number of live
    # mesoblocks is not: the current block and the next block are already
    # drawn, the next block has no live data, so it would be in the set),
    # in S writes (not until the shrink is done: when it is done depends on
    # the writes, a write of the partition to one of these blocks makes it
    # empty); the first S0 writes are skipped: the first write of the
    # partition goes to its next block, which was drawn before the test
    # stopped writing, and when the test stops depends on it: if the next
    # block has little live data, the current block has room for more data,
    # so the test more often stops right after it is drawn (the time between
    # two writes depends on the live data of the next block, that is how
    # the garbage collector works); blocks with little live data are mostly
    # old blocks, so these blocks would be hit too often by that write
    before = pool("a")
    last = {}
    for i, b in enumerate(wlog_read()): last[b] = i
    pred = sorted(before, key=lambda b: (last.get(b, -1), rng.random()))[:GROW]
    cmd("resize-internal a %d %d" % (nb, reserved(nb)))
    s0 = len(wlog_read())
    fd = opent("a", os.O_RDWR)
    while True:
        write_some(fd, nb, 15)
        os.fsync(fd)
        cur = pool("a")
        if len(cur) == nb: break
    os.close(fd)
    s1 = len(wlog_read())
    removed = set(before) - cur
    while len(wlog_read()) < s0 + S0 + S:
        fd = opent("a", os.O_RDWR); write_some(fd, nb); os.close(fd)
    w = wlog_read()[s0 + S0:s0 + S0 + S]
    shrink_hits += sum(b in pred for b in w); shrink_exp += S * GROW / TOTAL
    predicted += len(removed & set(pred))
    if r % 20 == 19:
        print("round %d: grow hits %d expected %.1f, shrink hits %d expected %.1f" %
              (r + 1, grow_hits, grow_exp, shrink_hits, shrink_exp), flush=True)
w = wlog_read()[start:]
counts = [0] * TOTAL
for b in w: counts[b] += 1
e = len(w) / TOTAL
c2 = sum((c - e) ** 2 / e for c in counts); df = TOTAL - 1
G = 10
tab = [[0] * G for _ in range(G)]
for x, y in zip(w, w[1:]): tab[x * G // TOTAL][y * G // TOTAL] += 1
rows = [sum(rr) for rr in tab]; cols = [sum(c) for c in zip(*tab)]; N = sum(rows)
c2p = sum((tab[i][j] - rows[i] * cols[j] / N) ** 2 / (rows[i] * cols[j] / N)
          for i in range(G) for j in range(G))
zg = (grow_hits - grow_exp) / math.sqrt(grow_exp)
zs = (shrink_hits - shrink_exp) / math.sqrt(shrink_exp)
zu = (c2 - df) / math.sqrt(2 * df); zi = (c2p - 81) / math.sqrt(2 * 81)
print("%d rounds of grow %d->%d and shrink back, %d block writes" %
      (rounds, PARTS["a"], PARTS["a"] + GROW, len(w)))
print("added blocks in the %d writes after a grow: %d hits, expected %.1f, z=%.2f" %
      (K, grow_hits, grow_exp, zg))
print("oldest blocks of the partition in %d writes during a shrink: %d hits, expected %.1f, z=%.2f "
      "(%.0f%% of them were removed)" % (S, shrink_hits, shrink_exp, zs,
      100 * predicted / (rounds * GROW)))
print("all writes uniform: chi2=%.0f df=%d z=%.2f; consecutive independent: z=%.2f" %
      (c2, df, zu, zi))
cmd("close a"); cmd("close b")
subprocess.run(["fusermount", "-u", MNT]); P.wait(timeout=60)
os.unlink(base)
bad = [x for x in (zg, zs, zu, zi) if abs(x) > 4]
print("RESIZESTAT %s" % ("FAILED" if bad else "OK"), flush=True)
