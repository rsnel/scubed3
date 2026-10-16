# indexfuzz.py - structure-aware fuzzing of what scubed3 reads from the base
# device: indices of blocks with a valid hash (the threat model allows an
# adversary with the key of a partition and write access to the base
# device), so the index is decrypted, changed, its hash computed again and
# encrypted again; the daemon (best built with -fsanitize=address,undefined)
# opens the partition, reads all of it, writes, closes, stops
#
# a finding: a crash (signal), an assertion, a sanitizer report, a hang, or
# FATAL (the whole daemon stops because of one bad block); a refusal to open
# the partition (an error from open) is fine
#
# usage: python3 indexfuzz.py DAEMON ITERATIONS [SEED] (as root; it uses its
# own mountpoint /mnt/s3fuzz and control socket /run/scubed3/fuzz, so it can
# run next to the other tests); the findings (base file, log, mutation) are
# kept in indexfuzz-findings/
import os, sys, socket, struct, hashlib, random, shutil, subprocess, time
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

DAEMON = os.path.abspath(sys.argv[1]); N = int(sys.argv[2])
SEED = int(sys.argv[3]) if len(sys.argv) > 3 else 1
R = random.Random(SEED)
MESO_LOG, MACRO_LOG = 12, 16
MESO, MACRO = 1 << MESO_LOG, 1 << MACRO_LOG
MMPM = MACRO // MESO - 1
NO_INDICES = 0x100
HASHES = 0x100 + 4 * (MMPM + 1)
BITMAP = HASHES + 8 * (MMPM + 1)
NBLOCKS = 120
MNT, SOCK = "/mnt/s3fuzz", "/run/scubed3/fuzz"
W = os.path.abspath("indexfuzz.work"); FIND = os.path.abspath("indexfuzz-findings")
KEY = bytes(range(32))
FUSERMOUNT = shutil.which("fusermount3") or "fusermount"
subprocess.run([FUSERMOUNT, "-u", "-z", MNT], stderr=subprocess.DEVNULL)
os.makedirs(W, exist_ok=True); os.makedirs(FIND, exist_ok=True); os.makedirs(MNT, exist_ok=True)
ESS = Cipher(algorithms.AES(hashlib.sha256(KEY).digest()), modes.ECB()).encryptor()

def iv(iv0, iv1, iv2): return ESS.update(struct.pack('>QII', iv0, iv1, iv2))
def dec(buf, iv0, iv1, iv2):
    return Cipher(algorithms.AES(KEY), modes.CBC(iv(iv0, iv1, iv2))).decryptor().update(buf)
def enc(buf, iv0, iv1, iv2):
    return Cipher(algorithms.AES(KEY), modes.CBC(iv(iv0, iv1, iv2))).encryptor().update(buf)

class Daemon:
    def __init__(self, base, log):
        subprocess.run([FUSERMOUNT, "-u", "-z", MNT], stderr=subprocess.DEVNULL)
        if os.path.exists(SOCK): os.unlink(SOCK)
        self.log = open(log, "w")
        self.p = subprocess.Popen([DAEMON, "-f", "-b", base, "-a", SOCK, "-m", str(MESO_LOG),
            "-M", str(MACRO_LOG), MNT], stdout=self.log, stderr=subprocess.STDOUT)
        for i in range(400):
            if os.path.exists(SOCK) or self.p.poll() is not None: break
            time.sleep(0.05)
    def cmd(self, c, timeout=30):
        s = socket.socket(socket.AF_UNIX); s.settimeout(timeout); s.connect(SOCK)
        s.sendall((c + "\n").encode()); buf = b""
        while not (buf.endswith(b"\n.\n") or buf == b".\n"):
            d = s.recv(65536)
            if not d: break
            buf += d
        s.close(); t = buf.decode(errors="replace"); return t.startswith("OK"), t
    def stop(self, timeout=60):
        subprocess.run([FUSERMOUNT, "-u", MNT], stderr=subprocess.DEVNULL)
        try: return self.p.wait(timeout)
        except subprocess.TimeoutExpired:
            self.p.kill(); self.p.wait(); return "hang"
        finally: self.log.close()

HEXKEY = KEY.hex()
OPEN = "open-internal f CBC_ESSIV(AES256) " + HEXKEY

def make_seed(base):
    """a base device with partition f (key KEY): writes, discards, a shrink
    (trims), more writes, so that there are tombstones, trims and stale
    blocks"""
    with open(base, "wb") as f: f.write(os.urandom(NBLOCKS * MACRO))
    d = Daemon(base, os.path.join(W, "seed.log"))
    ok, t = d.cmd("create-internal f CBC_ESSIV(AES256) " + HEXKEY); assert ok, t
    ok, t = d.cmd("resize-internal f 40 10"); assert ok, t
    size = 30 * MMPM * MESO
    fd = os.open(MNT + "/f", os.O_RDWR)
    for i in range(300):
        off = R.randrange(size // MESO) * MESO
        os.pwrite(fd, os.urandom(MESO * R.randint(1, 4)), off)
        if i % 50 == 0: os.fsync(fd)
    for i in range(20):     # discards (punched holes): tombstones
        fallocate(fd, R.randrange(size // MESO) * MESO, MESO * 3)
    os.fsync(fd); os.close(fd)
    ok, t = d.cmd("resize-internal f 30 8"); assert ok, t
    fd = os.open(MNT + "/f", os.O_RDWR)
    for i in range(100):
        os.pwrite(fd, os.urandom(MESO), R.randrange(20 * MMPM) * MESO)
    os.fsync(fd); os.close(fd)
    ok, t = d.cmd("close f"); assert ok, t
    assert d.stop() == 0

import ctypes
libc = ctypes.CDLL(None, use_errno=True)
def fallocate(fd, off, length):
    # FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE
    libc.fallocate(fd, 3, ctypes.c_long(off), ctypes.c_long(length))

def ours(base):
    """the blocks of f: (block number, decrypted index)"""
    res = []
    with open(base, "rb") as f:
        for b in range(NBLOCKS):
            f.seek(b * MACRO); idx = dec(f.read(MESO), 0, 0, b)
            if idx[0x70:0x78] == b"SSS3v0.5" and hashlib.sha256(idx[32:]).digest() == idx[:32]:
                res.append((b, bytearray(idx)))
    return res

def u32(buf, off, v): buf[off:off + 4] = struct.pack(">I", v & 0xFFFFFFFF)
def u64(buf, off, v): buf[off:off + 8] = struct.pack(">Q", v & 0xFFFFFFFFFFFFFFFF)
def g32(buf, off): return struct.unpack(">I", buf[off:off + 4])[0]
def g64(buf, off): return struct.unpack(">Q", buf[off:off + 8])[0]

def rnd32():
    return R.choice([0, 1, 2, 7, 8, 9, 15, 16, 29, 30, 31, 39, 40, 41, 119, 120, 121,
        0x7FFFFFFF, 0x80000000, 0xFFFFFFFE, 0xFFFFFFFF, R.getrandbits(32), R.randrange(200)])

def mutate(idx, b, blocks, f, desc):
    """one structured change of index idx (of block b)"""
    m = R.randrange(14)
    if m == 0:
        v = rnd32(); u32(idx, 0x78, v); desc.append(f"no_macroblocks={v}")
    elif m == 1:
        v = rnd32(); u32(idx, 0x7C, v); desc.append(f"reserved={v}")
    elif m == 2:
        v = rnd32(); u32(idx, 0x80, v); desc.append(f"pending_removals={v}")
    elif m == 3:
        v = R.choice([0, 1, 7, 8, 9, 100, 0xFFFFFFFF]); u32(idx, 0x84, v)
        desc.append(f"no_trims={v}")
    elif m == 4:
        n = min(g32(idx, 0x84), 8)
        if n:
            i = R.randrange(n); u64(idx, 0x88 + 12 * i, R.choice([0, 1, g64(idx, 0x60) + 1,
                R.getrandbits(64)])); u32(idx, 0x90 + 12 * i, rnd32()); desc.append(f"trim {i}")
        else:
            u32(idx, 0x84, 1); u64(idx, 0x88, g64(idx, 0x60) + R.randrange(3))
            u32(idx, 0x90, rnd32()); desc.append("a new trim")
    elif m == 5:
        v = R.choice([0, 1, g64(idx, 0x60) + 1, R.getrandbits(64), 0xFFFFFFFFFFFFFFFF])
        u64(idx, 0x40, v); desc.append(f"trim_floor={v}")
    elif m == 6:
        v = R.choice([0, 1, g64(idx, 0x60) + R.randrange(-3, 4), R.getrandbits(64),
            0xFFFFFFFFFFFFFFFF]); u64(idx, 0x60, v); desc.append(f"seqno={v}")
    elif m == 7:
        v = R.choice([b, rnd32(), R.randrange(NBLOCKS), 0xFFFFFFFF])
        u32(idx, 0x68, v); desc.append(f"next_block={v}")
    elif m == 8:
        v = R.choice([0, 1, MMPM - 1, MMPM, MMPM + 1, 0xFFFFFFFF, R.randrange(MMPM + 1)])
        u32(idx, NO_INDICES, v); desc.append(f"no_indices={v}")
    elif m == 9:
        n = min(g32(idx, NO_INDICES), MMPM)
        k = R.randrange(max(n, 1)); v = R.choice([rnd32(), 0xFFFFFFFE, g32(idx, NO_INDICES + 4)])
        u32(idx, NO_INDICES + 4 + 4 * k, v); desc.append(f"index {k}={v:#x}")
    elif m == 10:
        n = R.randrange(1, 40)
        for i in range(n):
            bit = R.randrange((MESO - BITMAP) * 8); o = BITMAP + 4 * (bit // 32)
            u32(idx, o, g32(idx, o) ^ (1 << (bit % 32)))
        desc.append(f"{n} bitmap bits")
    elif m == 11:
        k = R.randrange(MMPM); v = R.getrandbits(64)
        u64(idx, HASHES + 8 * k, v); desc.append(f"hash {k}")
    elif m == 12:
        # a data mesoblock with tombstones (any count and values)
        k = R.randrange(MMPM); u32(idx, NO_INDICES + 4 + 4 * k, 0xFFFFFFFE)
        if g32(idx, NO_INDICES) <= k: u32(idx, NO_INDICES, k + 1)
        cnt = R.choice([0, 1, 5, (MESO - 4) // 4, (MESO - 4) // 4 + 1, 0xFFFFFFFF])
        pt = bytearray(os.urandom(MESO)); u32(pt, 0, cnt)
        for i in range(min(cnt, (MESO - 4) // 4)):
            u32(pt, 4 + 4 * i, R.choice([R.randrange(30 * MMPM), rnd32()]))
        iv0 = g64(idx, 0x60) ^ g64(idx, 0x20)
        ct = enc(bytes(pt), iv0, k + 1, b)
        f.seek(b * MACRO + (k + 1) * MESO); f.write(ct)
        h = struct.unpack(">Q", hashlib.sha256(ct).digest()[:8])[0] & ~0xF
        u64(idx, HASHES + 8 * k, h); desc.append(f"tombstones in slot {k}, count {cnt:#x}")
    else:
        # the index of another block of ours (another seqno, bitmap, ...)
        ob, oidx = R.choice(blocks); idx[:] = oidx; desc.append(f"index of block {ob}")

def run_one(it, seedbase):
    base = os.path.join(W, "base")
    shutil.copyfile(seedbase, base)
    blocks = ours(base)
    desc = []
    with open(base, "r+b") as f:
        for b, idx in R.sample(blocks, R.randint(1, min(3, len(blocks)))):
            for j in range(R.randint(1, 3)):
                mutate(idx, b, blocks, f, desc)
            idx[:32] = hashlib.sha256(bytes(idx[32:])).digest()
            f.seek(b * MACRO); f.write(enc(bytes(idx), 0, 0, b))
            desc.append(f"(block {b})")
    log = os.path.join(W, "daemon.log")
    d = Daemon(base, log)
    phase = "open"; fd = None
    try:
        ok, t = d.cmd(OPEN)
        if ok:
            phase = "read"
            fd = os.open(MNT + "/f", os.O_RDWR)
            size = os.fstat(fd).st_size
            for off in range(0, size, MESO):
                try: os.pread(fd, MESO, off)
                except OSError as e:     # a corrupt mesoblock (its hash)
                    if e.errno != 5: raise
            phase = "write"
            for i in range(30):
                if size: os.pwrite(fd, os.urandom(MESO), R.randrange(size // MESO) * MESO)
            try: os.fsync(fd)
            except OSError as e:
                if e.errno != 5: raise; os.close(fd); fd = None
            phase = "info"; d.cmd("info f"); d.cmd("check-data-integrity f", 120)
            phase = "close"; d.cmd("close f")
    except Exception as e:
        phase += f" ({e.__class__.__name__}: {e})"
    if fd is not None:
        try: os.close(fd)
        except OSError: pass     # (the daemon is gone)
    st = d.stop()
    text = open(log, errors="replace").read()
    bad = None
    if st == "hang": bad = "hang"
    elif st != 0 and st is not None: bad = f"exit status {st}"
    for pat in ("AddressSanitizer", "runtime error:", "LeakSanitizer", "Assertion", "FATAL", ":fatal:"):
        if pat in text: bad = (bad + ", " if bad else "") + pat
    if "Exception" in phase and not bad: bad = "client: " + phase
    if bad:
        n = len(os.listdir(FIND)) // 3
        shutil.copyfile(base, os.path.join(FIND, f"{n}.base"))
        shutil.copyfile(log, os.path.join(FIND, f"{n}.log"))
        open(os.path.join(FIND, f"{n}.txt"), "w").write(
            f"iteration {it} seed {SEED}: {bad}\nphase {phase}\n" + "\n".join(desc) + "\n")
        print(f"{it}: FINDING {n}: {bad} [{phase}] " + "; ".join(desc), flush=True)
    return bad

seedbase = os.path.join(W, "seed.base")
make_seed(seedbase)
print(f"seed: {len(ours(seedbase))} blocks of f", flush=True)
found = 0
for it in range(N):
    bad = run_one(it, seedbase)
    if bad: found += 1
    if it % 50 == 0: print(f"{it} iterations, {found} findings", flush=True)
print(f"INDEXFUZZ {'OK' if not found else 'FINDINGS'}: {N} iterations, {found} findings")
