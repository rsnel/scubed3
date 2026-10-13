#!/usr/bin/env python3
# wipetest.py - removing a partition (shrinking it to 0 blocks) wipes every
# block that has an index of it, so it cannot come back with its data
#
#  1. level 1: a partition that was shrunk before (the blocks it gave away
#     still have old indices of it) is removed: afterwards no block has an
#     index of it, opening it says that there are no blocks
#  2. level 3: the same, the blocks are wiped in random order while other
#     partitions are open; it cannot be closed or resized before that is done
#  3. crashes (kill -9 at a random write) while it is wiped in level 3: after
#     a restart, opening it never gives data back
#  4. a partition whose most recent indices are lost (overwritten), with an
#     old index that uses blocks of another open partition: opening it is
#     refused, the daemon keeps running, the other partition is intact
#
# usage: wipetest.py DAEMON-crash SEED
# (small blocks: -m 12 -M 16, 64 KiB macroblocks)
import os, sys, time, socket, random, hashlib, subprocess, errno, struct, atexit
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

B = os.environ.get("S3BUILD", os.getcwd())
MNT, SOCK = "/mnt/s3", "/run/scubed3/control"
M_LOG, MM_LOG = 12, 16
MESO, MACRO = 1 << M_LOG, 1 << MM_LOG
MMPM = MACRO // MESO - 1
TOTAL = 200
daemon, seed = sys.argv[1], int(sys.argv[2])
rng = random.Random(seed)
base, logname = B + "/wipe.base", B + "/wipe.log"
log = open(logname, "w")
P = None
atexit.register(lambda: P and P.poll() is None and P.kill())

def key(name): return hashlib.pbkdf2_hmac("sha256", name.encode(), b"scubed3_prod", 1000, 32).hex()
def reserved(n): return max(1, n // 4)
def usable(n): return (n - reserved(n)) * MMPM

def start(crash_after=None):
    global P
    subprocess.run(["fusermount", "-u", "-z", MNT], capture_output=True)
    if os.path.exists(SOCK): os.unlink(SOCK)
    env = dict(os.environ)
    if crash_after: env["SCUBED3_CRASH_AFTER"] = str(crash_after)
    P = subprocess.Popen([B + "/" + daemon, "-b", base, "-m", str(M_LOG), "-M", str(MM_LOG),
        "-f", "-o", "direct_io", MNT], stdout=log, stderr=log, env=env)
    while not os.path.exists(SOCK):
        if P.poll() is not None: raise RuntimeError("daemon did not start")
        time.sleep(0.05)
    time.sleep(0.1)

def stop():
    subprocess.run(["fusermount", "-u", MNT], capture_output=True)
    try: P.wait(timeout=60)
    except subprocess.TimeoutExpired: P.kill(); P.wait()

def dead():
    try: P.wait(timeout=2); return True
    except subprocess.TimeoutExpired: return False

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

def info(n): return dict(l.split("=", 1) for l in cmd("info " + n).split("\n") if "=" in l)

def opent(name, flags):
    for _ in range(400):
        try: return os.open(MNT + "/" + name, flags)
        except OSError as e:
            if e.errno != errno.EBUSY: raise
            time.sleep(0.05)
    raise RuntimeError("busy")

def create(n, nb, k=None):
    cmd("create-internal %s CBC_ESSIV(AES256) %s" % (n, k or key(n)))
    cmd("resize-internal %s %d %d" % (n, nb, reserved(nb)))

def fill(n, nb, pattern):
    fd = opent(n, os.O_RDWR)
    for m in range(usable(nb)): os.pwrite(fd, pattern, m * MESO)
    os.fsync(fd); os.close(fd)

def remove(n):
    cmd("resize-internal %s 0 0" % n)
    for _ in range(4000):
        i = info(n)
        if int(i["pending_removals"]) == 0 and int(i["no_macroblocks"]) == 0: break
        cmd("cycle %s 1" % n)

def blocks_of(k):
    """the blocks of the base file with a valid index of key k (seqno, size)"""
    k = bytes.fromhex(k); ess = Cipher(algorithms.AES(hashlib.sha256(k).digest()), modes.ECB()).encryptor()
    out = []
    with open(base, "rb") as f:
        for mid in range(TOTAL):
            f.seek(mid * MACRO); c = f.read(MESO)
            iv = ess.update(struct.pack(">QII", 0, 0, mid))
            p = Cipher(algorithms.AES(k), modes.CBC(iv)).decryptor().update(c)
            if p[0x70:0x78] == b"SSS3v0.4" and hashlib.sha256(p[32:]).digest() == p[:32]:
                nmb, res, pend = struct.unpack(">III", p[0x78:0x84])
                out.append((mid, struct.unpack(">Q", p[0x60:0x68])[0], nmb - pend - res))
    return out

errors = []
def check(cond, what):
    print(("   ok: " if cond else "   FAILED: ") + what, flush=True)
    if not cond: errors.append(what)

with open(base, "wb") as f: f.write(os.urandom(TOTAL * MACRO))

# ------------------------------------------------------------- 1. level 1
print("1. level 1, a partition that was shrunk before is removed", flush=True)
start()
create("c", 40); fill("c", 40, b"C" * MESO)
cmd("resize-internal c 10 %d" % reserved(10))
fill("c", 10, b"D" * MESO)
old = len(blocks_of(key("c")))
remove("c")
check(int(info("c")["wipe_left"]) == 0, "wiped at once")
cmd("close c")
left = blocks_of(key("c"))
check(old > 10 and not left, "%d blocks had an index of c, afterwards %d" % (old, len(left)))
t = cmd("open-internal c CBC_ESSIV(AES256) %s" % key("c"), ok=False)
check("no blocks found" in t, "opening c: %s" % t.split("\n")[1])

# ------------------------------------------------------------- 2. level 3
print("2. level 3, the blocks are wiped in random order", flush=True)
create("a", 60); fill("a", 60, b"A" * MESO)
create("c", 40); fill("c", 40, b"C" * MESO)
cmd("resize-internal c 20 %d" % reserved(20))
cmd("paranoia 3 6400")
remove("c")
left = int(info("c")["wipe_left"])
t = cmd("close c", ok=False)
check(left > 0 and "not wiped yet" in t, "close refused while %d blocks are left" % left)
t = cmd("resize-internal c 5 1", ok=False)
check("not wiped yet" in t, "resize refused")
t = cmd("open-internal c2 CBC_ESSIV(AES256) %s" % key("c"), ok=False)
check(t.startswith("ERR"), "the key cannot be used while c is wiped")
t0 = time.monotonic()
while int(info("c")["wipe_left"]) and time.monotonic() - t0 < 120: time.sleep(0.2)
check(int(info("c")["wipe_left"]) == 0, "wiped in %.1fs" % (time.monotonic() - t0))
cmd("close c")
check("paranoia level 3" in cmd("p"), "still level 3")
check(not blocks_of(key("c")), "no block has an index of c")
fd = opent("a", os.O_RDONLY); d = os.pread(fd, usable(60) * MESO, 0); os.close(fd)
check(d == b"A" * (usable(60) * MESO), "a is intact")
cmd("close a"); stop()

# ------------------------------------------------------------- 3. crashes
print("3. kill -9 while the blocks are wiped (level 3)", flush=True)
results = {}
for r in range(12):
    with open(base, "wb") as f: f.write(os.urandom(TOTAL * MACRO))
    start()
    create("a", 60)
    k = key("c%d" % r)
    create("c", 40, k); fill("c", 40, b"C" * MESO)
    cmd("paranoia 3 6400")
    # the crash comes after a random number of further writes
    n = rng.randint(1, 120)
    stop()
    start(crash_after=n)
    cmd("open-internal a CBC_ESSIV(AES256) %s" % key("a"))
    cmd("open-internal c CBC_ESSIV(AES256) %s" % k)
    cmd("paranoia 3 6400")
    try:
        remove("c")
        t0 = time.monotonic()
        while int(info("c")["wipe_left"]) and time.monotonic() - t0 < 60: time.sleep(0.1)
    except (OSError, RuntimeError): pass
    if not dead(): P.kill(); P.wait()
    subprocess.run(["fusermount", "-u", "-z", MNT], capture_output=True)
    start()
    t = cmd("open-internal c CBC_ESSIV(AES256) %s" % k, ok=False)
    if t.startswith("OK"):
        sz = os.stat(MNT + "/c").st_size
        fd = opent("c", os.O_RDONLY); d = os.pread(fd, sz, 0); os.close(fd)
        res = "opened, size %d" % sz
        if b"C" * MESO in d: res = "DATA BACK"
    else: res = "refused (%s)" % t.split("\n")[1][:40]
    results[res] = results.get(res, 0) + 1
    stop()
print("   after the crash, opening c: %s" % results, flush=True)
check(not any("DATA" in r for r in results), "the data never comes back")

# ------------------------------------------------------------- 4. damaged
print("4. old indices that use blocks of another partition", flush=True)
with open(base, "wb") as f: f.write(os.urandom(TOTAL * MACRO))
start()
create("c", 40); fill("c", 40, b"C" * MESO)
cmd("resize-internal c 3 1")            # gives 37 blocks away, still with indices of c
for _ in range(50): cmd("cycle c 1")
create("d", 150); fill("d", 20, b"E" * MESO)
cmd("close c"); cmd("close d"); stop()
# the indices of c after the shrink are lost, an older one (size 40) is left
bl = blocks_of(key("c"))
with open(base, "r+b") as f:
    for mid, seq, size in bl:
        if size != 40 - reserved(40): f.seek(mid * MACRO); f.write(os.urandom(MACRO))
print("   %d blocks have an index of c, %d of them from before the shrink" %
      (len(bl), sum(1 for b in bl if b[2] == 40 - reserved(40))), flush=True)
start()
cmd("open-internal d CBC_ESSIV(AES256) %s" % key("d"))
t = cmd("open-internal c CBC_ESSIV(AES256) %s" % key("c"), ok=False)
alive = not dead()
check(alive and t.startswith("ERR") and "belongs to partition" in t, "opening c is refused: %s" %
      t.split("\n")[1][:100])
if alive:
    fd = opent("d", os.O_RDONLY); d = os.pread(fd, usable(20) * MESO, 0); os.close(fd)
    check(d == b"E" * (usable(20) * MESO), "d is intact")
    cmd("close d"); stop()
os.unlink(base)
print("WIPETEST %s" % ("OK" if not errors else "FAILED: %s" % errors), flush=True)
