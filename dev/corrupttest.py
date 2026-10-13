#!/usr/bin/env python3
# corrupttest.py - per mesoblock integrity: corrupt ciphertext on disk, then
# reads of those mesoblocks must give EIO (and only those), check-data-integrity
# must report exactly them, they must stay corrupt when the GC moves them,
# writing a 4KiB part of a corrupt mesoblock repairs that part (a write that
# covers a corrupt part partially gives EIO), and everything must survive
# restarts; the offline reader must agree
#
# usage: corrupttest.py DAEMON ROUNDS SEED
import os, sys, time, socket, random, hashlib, subprocess, errno
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import s3read

B = os.environ.get("S3BUILD", os.getcwd())
MNT, SOCK, MESO, PART = "/mnt/s3", "/run/scubed3/control", 1 << 14, 1 << 12
PW = b"cpw"
KEY = hashlib.pbkdf2_hmac("sha256", PW, b"scubed3_prod", 1000, 32).hex()
daemon, rounds, seed = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
rng = random.Random(seed)
base = B + "/corrupt.base"
log = open(B + "/corrupt.log", "w")
P = None

def start():
    global P
    subprocess.run(["fusermount", "-u", "-z", MNT], capture_output=True)
    if os.path.exists(SOCK): os.unlink(SOCK)
    P = subprocess.Popen([B + "/" + daemon, "-b", base, "-f", MNT], stdout=log, stderr=log)
    while not os.path.exists(SOCK): time.sleep(0.05)
    time.sleep(0.1)

def stop():
    subprocess.run(["fusermount", "-u", MNT], check=True); P.wait(timeout=30)

def cmd(line):
    for _ in range(100):
        s = socket.socket(socket.AF_UNIX); s.connect(SOCK)
        s.sendall((line + "\nexit\n").encode())
        buf = b""
        while True:
            d = s.recv(65536)
            if not d: break
            buf += d
        t = buf.decode()
        if "busy" in t: time.sleep(0.1); continue
        if not t.startswith("OK"): raise RuntimeError("%s: %r" % (line, t))
        return t
    raise RuntimeError("busy")

def opn(): cmd("open-internal t CBC_ESSIV(AES256) " + KEY)

def check():
    """the set of corrupt parts (mesoblock, part) reported by the daemon"""
    t = cmd("check-data-integrity t"); out = set()
    for l in t.split("\n"):
        if l.startswith("corrupt="):
            off, size = map(int, l.split("=")[1].split("+"))
            assert size == PART
            out.add((off // MESO, off % MESO // PART))
    return out

def read_all():
    """returns the set of parts that give EIO, verifies the others"""
    eio = set()
    fd = opent(os.O_RDONLY)
    os.posix_fadvise(fd, 0, 0, os.POSIX_FADV_RANDOM)
    for m in range(size):
        for p in range(4):
            try:
                d = os.pread(fd, PART, m * MESO + p * PART)
            except OSError as e:
                if e.errno != errno.EIO: raise
                eio.add((m, p)); continue
            if hashlib.sha256(d).digest() != model[m * 4 + p]:
                raise RuntimeError("part %d of mesoblock %d has wrong contents" % (p, m))
    os.close(fd)
    return eio

def opent(flags):
    # scubed3 allows one opener, the release of the previous one is async
    for _ in range(100):
        try: return os.open(MNT + "/t", flags)
        except OSError as e:
            if e.errno != errno.EBUSY: raise
            time.sleep(0.05)
    raise RuntimeError("busy")

def write(off, data):
    fd = opent(os.O_WRONLY)
    try: os.pwrite(fd, data, off)
    finally: os.close(fd)
    for i in range(0, len(data), PART):
        model[(off + i) // PART] = hashlib.sha256(data[i:i + PART]).digest()

def offline_bad():
    st = s3read.Store(base, PW, 1000)
    return {(m, p) for m in range(size) for p in range(4) if st.read(m)[1] >> p & 1}

def expect(what):
    c, e = check(), read_all()
    if c != bad or e != bad:
        raise RuntimeError("%s: check %s eio %s expected %s" % (what, sorted(c), sorted(e), sorted(bad)))

# setup: a partition filled with random data, rewritten partly a few times so
# that the mesoblocks are scattered over the blocks
with open(base, "wb") as f: f.write(os.urandom(24 << 22))
start()
cmd("create-internal t CBC_ESSIV(AES256) " + KEY)
cmd("resize-internal t 12 3")
size = os.stat(MNT + "/t").st_size // MESO
model = [None] * (size * 4)     # sha256 of each 4KiB part
for rep in range(3):
    for m in (range(size) if rep == 0 else rng.sample(range(size), size // 3)):
        write(m * MESO, os.urandom(MESO))
bad = set()     # corrupt parts (mesoblock, part)

for rnd in range(rounds):
    # corrupt some live mesoblocks on disk (not in the most recent block:
    # damage there is indistinguishable from an interrupted write, the
    # whole block would be ignored)
    cmd("close t"); stop()
    st = s3read.Store(base, PW, 1000)
    latest = st.blocks[-1][1]
    bad_meso = {m for m, p in bad}
    cands = [m for m, (blk, k, sq, h) in st.loc.items() if blk != latest and m not in bad_meso]
    new_bad = set(rng.sample(cands, rng.randint(1, 4)))
    with open(base, "r+b") as f:
        for m in new_bad:
            blk, k, sq, h = st.loc[m]
            f.seek(blk * (1 << 22) + (k + 1) * MESO + rng.randrange(MESO))
            b = f.read(1); f.seek(-1, 1); f.write(bytes([b[0] ^ (1 << rng.randrange(8))]))
    bad |= {(m, p) for m in new_bad for p in range(4)}
    if offline_bad() != bad: raise RuntimeError("offline reader: %s != %s" % (offline_bad(), bad))

    start(); opn()
    expect("after corruption")

    # let the GC move everything around (corrupt mesoblocks are copied
    # into new blocks, they must stay corrupt)
    cmd("cycle t %d" % rng.randint(10, 40))
    expect("after GC")

    # writes that cover a corrupt part partially fail, nothing changes
    for m, p in rng.sample(sorted(bad), min(len(bad), 6)):
        o = rng.randrange(PART - 512) & ~511
        try:
            write(m * MESO + p * PART + o, os.urandom(512))
            raise RuntimeError("partial write into corrupt part %d of %d succeeded" % (p, m))
        except OSError as ex:
            if ex.errno != errno.EIO: raise
    expect("after failed writes")

    # repair: whole mesoblocks, single parts and ranges of parts
    for m, p in rng.sample(sorted(bad), rng.randint(0, len(bad))):
        if (m, p) not in bad: continue
        r = rng.random()
        if r < 0.3:
            write(m * MESO, os.urandom(MESO)); bad -= {(m, q) for q in range(4)}
        elif r < 0.7:
            write(m * MESO + p * PART, os.urandom(PART)); bad.discard((m, p))
        else:
            q = rng.randint(p, 3)
            write(m * MESO + p * PART, os.urandom((q - p + 1) * PART))
            bad -= {(m, x) for x in range(p, q + 1)}
    # and writes elsewhere (whole and partial)
    for m in rng.sample(range(size), size // 10):
        if any((m, p) in bad for p in range(4)): continue
        if rng.random() < 0.5: write(m * MESO, os.urandom(MESO))
        else: write(m * MESO + PART * rng.randrange(4), os.urandom(PART))
    expect("after repair")

    # restart: everything stays as it was
    cmd("close t"); stop(); start(); opn()
    expect("after restart")
    print("round %d: corrupted %d mesoblocks, corrupt parts now %d, check/EIO agree"
            % (rnd, len(new_bad), len(bad)), flush=True)

cmd("close t"); stop()
st = s3read.Store(base, PW, 1000)
if offline_bad() != bad: raise RuntimeError("offline reader at end: %s != %s" % (offline_bad(), bad))
for m in range(size):
    d, mask = st.read(m)
    for p in range(4):
        if not mask >> p & 1 and hashlib.sha256(d[p * PART:(p + 1) * PART]).digest() != model[m * 4 + p]:
            raise RuntimeError("offline: part %d of %d" % (p, m))
print("CORRUPTTEST OK: %d rounds, %d parts still corrupt, corrupt warnings in log: %d" %
        (rounds, len(bad), open(B + "/corrupt.log").read().count("is corrupt")), flush=True)
