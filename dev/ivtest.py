#!/usr/bin/env python3
# ivtest.py - the IVs of the data mesoblocks do not repeat, also not when the
# seqnos repeat: when the base device is restored from a backup and used
# again, or when a torn write is redone (the IV is ESSIV(seqno^nonce, slot,
# block), with a random nonce for every write, see README, Encryption)
#
#  1. a backup of the base device is made, the partition is written, the
#     backup is restored and the partition is written again (the seqnos
#     repeat, the first write goes to the same block as before)
#  2. kill -9 with a torn write at a random moment, a restart, more writes
#     (the torn block is ignored, its write is redone with the same seqno)
#  3. a base device written by an older scubed3 (SSS3v0.4, no nonce) is
#     opened, read and written: both versions of blocks are read
#
# after every fsync the indices of the partition are read from the base
# device (with the key), every write of a block is recorded as (block,
# seqno^nonce, hash of its index); the test fails if one (block,
# seqno^nonce) belongs to two different writes
#
# usage: ivtest.py DAEMON-crash SEED [OLD-DAEMON]
# (small blocks: -m 12 -M 16, 64 KiB macroblocks; with OLD-DAEMON, a build
# of an older version, part 3 creates the base device with it; without it,
# part 3 is skipped)
import os, sys, time, socket, random, hashlib, subprocess, errno, struct, atexit
import shutil
FUSERMOUNT = shutil.which("fusermount3") or "fusermount"   # libfuse 3 or 2
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

B = os.environ.get("S3BUILD", os.getcwd())
MNT, SOCK = "/mnt/s3", "/run/scubed3/control"
M_LOG, MM_LOG = 12, 16
MESO, MACRO = 1 << M_LOG, 1 << MM_LOG
MMPM = MACRO // MESO - 1
TOTAL = 120
NB = 40
daemon, seed = sys.argv[1], int(sys.argv[2])
old = sys.argv[3] if len(sys.argv) > 3 else None
rng = random.Random(seed)
base, logname = B + "/iv.base", B + "/iv.log"
log = open(logname, "w")
P = None
atexit.register(lambda: P and P.poll() is None and P.kill())

def key(name): return hashlib.pbkdf2_hmac("sha256", name.encode(), b"scubed3_prod", 1000, 32).hex()
def reserved(n): return max(1, n // 4)
def usable(n): return (n - reserved(n)) * MMPM
K = key("a")

def start(d=None, crash_after=None, tear=False):
    global P
    subprocess.run([FUSERMOUNT, "-u", "-z", MNT], capture_output=True)
    if os.path.exists(SOCK): os.unlink(SOCK)
    env = dict(os.environ)
    if crash_after: env["SCUBED3_CRASH_AFTER"] = str(crash_after)
    if tear: env["SCUBED3_TEAR"] = "1"
    P = subprocess.Popen([d or (B + "/" + daemon), "-b", base, "-m", str(M_LOG), "-M", str(MM_LOG),
        "-f", "-o", "direct_io", MNT], stdout=log, stderr=log, env=env)
    while not os.path.exists(SOCK):
        if P.poll() is not None: raise RuntimeError("daemon did not start")
        time.sleep(0.05)
    time.sleep(0.1)

def stop():
    subprocess.run([FUSERMOUNT, "-u", MNT], capture_output=True)
    try: P.wait(timeout=60)
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

kb = bytes.fromhex(K)
ess = Cipher(algorithms.AES(hashlib.sha256(kb).digest()), modes.ECB()).encryptor()
def indices():
    """the valid indices of a on the base device: (block, seqno, iv0, magic, index hash)"""
    out = []
    with open(base, "rb") as f:
        for mid in range(TOTAL):
            f.seek(mid * MACRO); c = f.read(MESO)
            iv = ess.update(struct.pack(">QII", 0, 0, mid))
            p = Cipher(algorithms.AES(kb), modes.CBC(iv)).decryptor().update(c)
            if p[0x70:0x78] not in (b"SSS3v0.5", b"SSS3v0.4"): continue
            if hashlib.sha256(p[32:]).digest() != p[:32]: continue
            seq = struct.unpack(">Q", p[0x60:0x68])[0]
            nonce = struct.unpack(">Q", p[0x20:0x28])[0] if p[0x70:0x78] == b"SSS3v0.5" else 0
            out.append((mid, seq, seq ^ nonce, p[0x70:0x78], p[:32]))
    return out

writes = {}     # (block, iv0) -> set of index hashes
seqs = {}       # (block, seqno) -> set of index hashes
def record():
    for mid, seq, iv0, mg, h in indices():
        writes.setdefault((mid, iv0), set()).add(h)
        seqs.setdefault((mid, seq), set()).add(h)

model = {}
def write_round(fd, n):
    for _ in range(n):
        m = rng.randrange(usable(NB)); d = os.urandom(MESO)
        os.pwrite(fd, d, m * MESO); model[m] = d
    os.fsync(fd)

def verify(when):
    fd = opent("a", os.O_RDONLY); bad = 0
    for m in range(usable(NB)):
        if os.pread(fd, MESO, m * MESO) != model.get(m, bytes(MESO)): bad += 1
    os.close(fd)
    check(not bad, "%s: a is intact (%d bad mesoblocks)" % (when, bad))

errors = []
def check(cond, what):
    print(("   ok: " if cond else "   FAILED: ") + what, flush=True)
    if not cond: errors.append(what)

def repeats():
    """writes of a block with the same seqno, and with the same IV"""
    return (sum(1 for v in seqs.values() if len(v) > 1),
            sum(1 for v in writes.values() if len(v) > 1))

def fresh():
    writes.clear(); seqs.clear(); model.clear()
    with open(base, "wb") as f: f.write(os.urandom(TOTAL * MACRO))

def create(d=None):
    start(d)
    cmd("create-internal a CBC_ESSIV(AES256) %s" % K)
    cmd("resize-internal a %d %d" % (NB, reserved(NB)))

# ------------------------------------------------------------- 1. backup
print("1. a backup of the base device is restored and used again", flush=True)
fresh(); create()
fd = opent("a", os.O_RDWR)
for _ in range(5): write_round(fd, 40); record()
os.close(fd); cmd("close a"); stop()
shutil.copyfile(base, base + ".backup"); saved = dict(model)
for timeline in (1, 2):
    if timeline == 2:
        shutil.copyfile(base + ".backup", base); model = dict(saved)
    start(); cmd("open-internal a CBC_ESSIV(AES256) %s" % K)
    fd = opent("a", os.O_RDWR)
    for _ in range(20): write_round(fd, 30); record()
    os.close(fd); verify("timeline %d" % timeline); cmd("close a"); stop()
os.unlink(base + ".backup")
s, i = repeats()
print("   %d writes of a block with a seqno that was used before for it" % s, flush=True)
check(s > 0, "the seqnos repeat (the test makes sense)")
check(i == 0, "no IV is used twice (%d)" % i)

# ------------------------------------------------------------- 2. torn
print("2. kill -9 with a torn write, the write is redone", flush=True)
fresh(); create()
fd = opent("a", os.O_RDWR); write_round(fd, 200); os.close(fd)
cmd("close a"); stop(); record()
torn = 0
for r in range(10):
    start(crash_after=rng.randint(1, 15), tear=True)
    cmd("open-internal a CBC_ESSIV(AES256) %s" % K)
    try:
        fd = opent("a", os.O_RDWR)
        for _ in range(10): write_round(fd, 20)
        os.close(fd)
    except OSError: pass
    try: P.wait(timeout=10)
    except subprocess.TimeoutExpired: P.kill(); P.wait()
    subprocess.run([FUSERMOUNT, "-u", "-z", MNT], capture_output=True)
    record()                        # the torn block (if its index got through)
    start(); cmd("open-internal a CBC_ESSIV(AES256) %s" % K)
    fd = opent("a", os.O_RDWR); write_round(fd, 20); os.close(fd); record()
    cmd("close a"); stop()
s, i = repeats()
print("   %d writes of a block with a seqno that was used before for it" % s, flush=True)
check(s > 0, "the seqnos repeat (the test makes sense)")
check(i == 0, "no IV is used twice (%d)" % i)

# ------------------------------------------------------------- 3. v0.4
if old:
    print("3. a base device written by an older scubed3 (SSS3v0.4)", flush=True)
    fresh(); create(old)
    fd = opent("a", os.O_RDWR); write_round(fd, 300); os.close(fd)
    cmd("close a"); stop()
    v = [x[3] for x in indices()]
    check(v and all(m == b"SSS3v0.4" for m in v), "%d blocks of SSS3v0.4" % len(v))
    start(); cmd("open-internal a CBC_ESSIV(AES256) %s" % K)
    verify("opened by this version")
    fd = opent("a", os.O_RDWR); write_round(fd, 60); os.close(fd)
    verify("written by this version")
    cmd("close a"); stop()
    v = [x[3] for x in indices()]
    print("   %d blocks of SSS3v0.4, %d of SSS3v0.5" % (v.count(b"SSS3v0.4"), v.count(b"SSS3v0.5")),
          flush=True)
    check(v.count(b"SSS3v0.4") and v.count(b"SSS3v0.5"), "both versions of blocks")
    start(); cmd("open-internal a CBC_ESSIV(AES256) %s" % K)
    verify("after a restart")
    cmd("close a"); stop()
else: print("3. skipped (no OLD-DAEMON)", flush=True)

os.unlink(base)
print("IVTEST %s" % ("OK" if not errors else "FAILED: %s" % errors), flush=True)
