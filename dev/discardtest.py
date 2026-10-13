#!/usr/bin/env python3
# discardtest.py - discard (fallocate PUNCH_HOLE / ZERO_RANGE, fstrim)
#
#  A. byte exact model: unaligned writes, punch holes, zero ranges and
#     allocates through the FUSE file, clean restarts: the partition must
#     match the model exactly (discarded data must never come back)
#  B. blocks that are re-added by a grow while they still have an index of
#     ours on disk (removed by a shrink, their discarded mesoblocks lost
#     their tombstones meanwhile): after a restart nothing comes back
#  C. ext2 (with the ext4 driver), ext3, ext4: fill, delete, fstrim (and
#     mount -o discard): mapped mesoblocks go down and stay down after a
#     restart, the files that are left are intact, fsck is clean; the cost
#     of writing into a partition full of deleted data, with and without
#     fstrim
#  D. XFS, if mkfs.xfs is available (MKFS_XFS=path, xfs_repair is expected in
#     ../repair/ relative to it, as in an xfsprogs build tree)
#  (PARTS=ABCD selects parts)
#
# usage: discardtest.py DAEMON SEED
import os, sys, time, socket, random, hashlib, subprocess, errno, ctypes, shutil

B = os.environ.get("S3BUILD", os.getcwd())
MNT, FS, SOCK, MESO = "/mnt/s3", "/mnt/dt", "/run/scubed3/control", 1 << 14
PW = b"dpw"
KEY = hashlib.pbkdf2_hmac("sha256", PW, b"scubed3_prod", 1000, 32).hex()
daemon, seed = sys.argv[1], int(sys.argv[2])
rng = random.Random(seed)
base = B + "/discard.base"
log = open(B + "/discard.log", "w")
P = None
libc = ctypes.CDLL(None, use_errno=True)
KEEP, PUNCH, ZERO = 1, 2, 0x10

def start():
    global P
    subprocess.run(["fusermount", "-u", "-z", MNT], capture_output=True)
    if os.path.exists(SOCK): os.unlink(SOCK)
    P = subprocess.Popen([B + "/" + daemon, "-b", base, "-f", MNT], stdout=log, stderr=log)
    while not os.path.exists(SOCK): time.sleep(0.05)
    time.sleep(0.1)
    if P.poll() is not None: raise RuntimeError("daemon did not start, see log")

def stop():
    subprocess.run(["fusermount", "-u", MNT], check=True); P.wait(timeout=60)

def cmd(line):
    s = socket.socket(socket.AF_UNIX); s.connect(SOCK)
    s.sendall((line + "\nexit\n").encode()); buf = b""
    while True:
        d = s.recv(65536)
        if not d: break
        buf += d
    t = buf.decode()
    if not t.startswith("OK"): raise RuntimeError("%s: %r" % (line, t))
    return t

def info(): return {k: int(v) for k, v in (l.split("=") for l in cmd("info t").split("\n")
    if "=" in l)}

def opn(): cmd("open-internal t CBC_ESSIV(AES256) " + KEY)

def restart(): cmd("close t"); stop(); start(); opn()

def run(*a):
    r = subprocess.run(a, capture_output=True, text=True)
    if r.returncode: raise RuntimeError("%s: %s %s" % (a, r.stdout, r.stderr))
    return r.stdout

def fallocate(fd, mode, off, ln):
    if libc.fallocate(fd, mode, ctypes.c_long(off), ctypes.c_long(ln)) != 0:
        e = ctypes.get_errno(); raise OSError(e, os.strerror(e))

def new_store(nblocks):
    with open(base, "wb") as f: f.write(os.urandom(nblocks << 22))

def resize(n): cmd("resize-internal t %d %d" % (n, (n - 1) // 4 + 1))

# ---------------------------------------------------------------- A
def part_a(rounds):
    new_store(16); start()
    cmd("create-internal t CBC_ESSIV(AES256) " + KEY); resize(8)
    S = os.stat(MNT + "/t").st_size
    model = bytearray(S)
    ops = dict(write=0, punch=0, zero=0, alloc=0, restart=0)
    for r in range(rounds):
        fd = os.open(MNT + "/t", os.O_RDWR)
        for _ in range(rng.randint(5, 40)):
            op = rng.choice(["write", "write", "punch", "zero", "alloc"])
            off = rng.randrange(S)
            ln = min(S - off, rng.choice([1, 100, 4096, MESO, 3 * MESO + 77,
                200_000, 2_000_000, S]))
            if rng.random() < 0.5: off &= ~4095
            ln = max(1, min(ln, S - off))
            ops[op] += 1
            if op == "write":
                d = os.urandom(ln); os.pwrite(fd, d, off); model[off:off + ln] = d
            elif op == "punch":
                fallocate(fd, PUNCH | KEEP, off, ln); model[off:off + ln] = bytes(ln)
            elif op == "zero":
                fallocate(fd, ZERO, off, ln); model[off:off + ln] = bytes(ln)
            else:
                fallocate(fd, 0, off, ln)
        # allocating past the end is impossible, punching past it is fine
        try: fallocate(fd, 0, S - 10, 100); raise RuntimeError("allocate past end")
        except OSError as e: assert e.errno == errno.ENOSPC, e
        fallocate(fd, PUNCH | KEEP, S, 100000)
        try: fallocate(fd, 0x20, 0, 4096); raise RuntimeError("collapse range")
        except OSError as e: assert e.errno == errno.EOPNOTSUPP, e
        if os.pread(fd, S, 0) != model: raise RuntimeError("A: mismatch before restart %d" % r)
        os.close(fd)
        if rng.random() < 0.6:
            restart(); ops["restart"] += 1
            if open(MNT + "/t", "rb").read() != model:
                raise RuntimeError("A: mismatch after restart %d" % r)
        if rng.random() < 0.3: cmd("cycle t %d" % rng.randint(1, 20))
    restart()
    if open(MNT + "/t", "rb").read() != model: raise RuntimeError("A: final mismatch")
    i = info()
    print("A: byte exact model OK %s, mapped %d of %d mesoblocks, tombstone slots %d"
            % (ops, i["mapped_mesoblocks"], S // MESO, i["tombstone_slots"]), flush=True)
    cmd("close t"); stop()

# ---------------------------------------------------------------- B
def part_b():
    # a store that is exactly as large as the partition: a grow re-adds
    # the blocks that the shrink removed
    new_store(10); start()
    cmd("create-internal t CBC_ESSIV(AES256) " + KEY); resize(10)
    S = os.stat(MNT + "/t").st_size
    data = os.urandom(S)
    with open(MNT + "/t", "r+b") as f:
        f.write(data)
        for _ in range(3):      # several versions of everything
            for m in rng.sample(range(S // MESO), S // MESO // 2):
                f.seek(m * MESO); f.write(data[m * MESO:(m + 1) * MESO])
    resize(6)
    for _ in range(200):
        if not info()["pending_removals"]: break
        cmd("cycle t 1")
    S6 = os.stat(MNT + "/t").st_size
    fd = os.open(MNT + "/t", os.O_RDWR)
    fallocate(fd, PUNCH | KEEP, 0, S6)
    os.close(fd)
    cmd("cycle t 1")    # the tombstones are written
    peak = info()["tombstone_slots"]
    if not peak: raise RuntimeError("B: no tombstones")
    for c in range(3000):
        i = info()
        if not i["tombstone_slots"] and not i["tombstones_pending"]: break
        cmd("cycle t 1")
    else: raise RuntimeError("B: tombstones do not die out")
    resize(10)          # re-adds the removed blocks, with their old indices
    readded = open(B + "/discard.log").read().count("indices of ours")
    restart()
    d = open(MNT + "/t", "rb").read()
    if d[:S6] != bytes(S6): raise RuntimeError("B: discarded data came back after grow")
    i = info()
    print("B: %d tombstone slots died out after %d cycles, re-added blocks with old "
            "indices: %d, nothing came back (mapped %d)" % (peak, c, readded,
            i["mapped_mesoblocks"]), flush=True)
    cmd("close t"); stop()

def part_b2(tries=6):
    """discard first, then shrink: the removed blocks keep old versions on
    disk, their tombstones die out while the trim of the shrink is still
    there (so the removed blocks are not below the trim floor), then a grow
    re-adds them: without tombstones for them, a restart would bring the
    discarded data back"""
    for t in range(tries):
        new_store(12); start()
        cmd("create-internal t CBC_ESSIV(AES256) " + KEY); resize(12)
        S = os.stat(MNT + "/t").st_size
        with open(MNT + "/t", "r+b") as f:
            for _ in range(2): f.seek(0); f.write(os.urandom(S))
        cmd("cycle t 1")
        fd = os.open(MNT + "/t", os.O_RDWR); fallocate(fd, PUNCH | KEEP, 0, S); os.close(fd)
        cmd("cycle t 1")
        peak = info()["tombstone_slots"]
        resize(8)
        for _ in range(50):
            if not info()["pending_removals"]: break
            cmd("cycle t 1")
        # a few cycles: some tombstones die, the trim must survive
        for c in range(rng.randint(1, 4)): cmd("cycle t 1")
        i = info()
        if not i["trims"]:
            cmd("close t"); stop(); continue
        before = open(B + "/discard.log").read().count("indices of ours")
        resize(12)
        readded = open(B + "/discard.log").read().count("indices of ours") - before
        pending = info()["tombstones_pending"]
        restart()
        d = open(MNT + "/t", "rb").read()
        bad = sum(1 for m in range(len(d) // MESO) if d[m * MESO:(m + 1) * MESO] != bytes(MESO))
        cmd("close t"); stop()
        if bad: raise RuntimeError("B2: %d discarded mesoblocks came back after grow" % bad)
        print("B2: tombstone slots %d -> %d (trims %d), %d re-added blocks with old indices "
                "got %d tombstones, nothing came back" % (peak, i["tombstone_slots"],
                i["trims"], readded, pending), flush=True)
        if readded and pending: return
    raise RuntimeError("B2: scenario not reached in %d tries" % tries)

def part_full():
    """a partition that is completely full of live data (partition of 2
    blocks, 1 reserved): every discard needs a tombstone, there is never a
    free slot, the slot of the discarded mesoblock makes room"""
    for nb, res in ((2, 1), (3, 1), (4, 1)):
        new_store(nb + 1); start()
        cmd("create-internal t CBC_ESSIV(AES256) " + KEY)
        cmd("resize-internal t %d %d" % (nb, res))
        S = os.stat(MNT + "/t").st_size
        model = bytearray(os.urandom(S))
        with open(MNT + "/t", "r+b") as f:
            f.write(model); f.seek(0); f.write(model)
        for r in range(6):
            fd = os.open(MNT + "/t", os.O_RDWR)
            for _ in range(20):
                m = rng.randrange(S // MESO)
                if rng.random() < 0.5:
                    fallocate(fd, PUNCH | KEEP, m * MESO, MESO); model[m * MESO:(m + 1) * MESO] = bytes(MESO)
                else:
                    d = os.urandom(MESO); os.pwrite(fd, d, m * MESO); model[m * MESO:(m + 1) * MESO] = d
            os.close(fd)
            restart()
            if open(MNT + "/t", "rb").read() != model:
                raise RuntimeError("full %d/%d: mismatch after restart %d" % (nb, res, r))
        print("full partition (%d blocks, %d reserved): discards and writes OK, writes=%d"
                % (nb, res, info()["writes"]), flush=True)
        cmd("close t"); stop()

# ---------------------------------------------------------------- C/D
def loop_attach():
    for _ in range(50):
        r = subprocess.run(["losetup", "-f", "--show", MNT + "/t"], capture_output=True, text=True)
        if r.returncode == 0: return r.stdout.strip()
        time.sleep(0.2)
    raise RuntimeError("losetup")

def loop_detach(dev): subprocess.run(["losetup", "-d", dev]); time.sleep(0.3)

def fs_check(fstype):
    dev = loop_attach()
    try:
        if fstype == "xfs": r = subprocess.run([XFS_REPAIR, "-n", dev], capture_output=True, text=True)
        else: r = subprocess.run(["e2fsck", "-fn", dev], capture_output=True, text=True)
        if r.returncode: raise RuntimeError("fsck %s: %s %s" % (fstype, r.stdout[-2000:], r.stderr[-2000:]))
    finally: loop_detach(dev)

def mount(fstype, opts=""):
    o = "loop" + ("," + opts if opts else "")
    run("mount", "-t", "ext4" if fstype == "ext2" else fstype, "-o", o, MNT + "/t", FS)

def umount(): run("umount", FS); time.sleep(0.3)

def fill(files, frac):
    st = os.statvfs(FS); want = int(st.f_bavail * st.f_frsize * frac); n = 0
    while want > 0:
        sz = min(want, rng.choice([100_000, 1_000_000, 4_000_000]))
        d = os.urandom(sz); name = "f%06d" % rng.randrange(10**6)
        open(FS + "/" + name, "wb").write(d); files[name] = hashlib.sha256(d).hexdigest()
        want -= sz; n += 1
    os.sync()

def check_files(files):
    for name, h in files.items():
        if hashlib.sha256(open(FS + "/" + name, "rb").read()).hexdigest() != h:
            raise RuntimeError("file %s corrupt" % name)
    if set(os.listdir(FS)) - {"lost+found"} != set(files):
        raise RuntimeError("files differ")

def write_cost(mb):
    """macroblock writes needed to write mb MiB (and sync)"""
    w0 = info()["writes"]
    with open(FS + "/cost", "wb") as f:
        for _ in range(mb): f.write(os.urandom(1 << 20))
    os.sync(); os.unlink(FS + "/cost"); os.sync()
    return info()["writes"] - w0

def part_fs(fstype, mkfs):
    new_store(48); start()
    cmd("create-internal t CBC_ESSIV(AES256) " + KEY); resize(40)
    run(*mkfs, MNT + "/t")
    i0 = info()
    mount(fstype, "discard" if fstype == "ext4" else "")
    files = {}
    fill(files, 0.85)
    full = info()["mapped_mesoblocks"]
    # delete most files, then measure the cost of writing without trim
    for name in rng.sample(sorted(files), len(files) * 4 // 5):
        os.unlink(FS + "/" + name); del files[name]
    os.sync()
    if fstype == "ext4":    # mounted with -o discard: freed at commit
        time.sleep(6); os.sync()
        after_rm = info()["mapped_mesoblocks"]
        run("fstrim", FS)
    else:
        after_rm = info()["mapped_mesoblocks"]
        cost_untrimmed = write_cost(60)
        run("fstrim", FS)
    trimmed = info()["mapped_mesoblocks"]
    cost_trimmed = write_cost(60)
    time.sleep(1); os.sync()
    run("fstrim", FS)   # the file written for the cost is deleted
    check_files(files)
    umount()
    trimmed = before = info()["mapped_mesoblocks"]
    h1 = hashlib.sha256(open(MNT + "/t", "rb").read()).hexdigest()
    restart()
    after_restart = info()["mapped_mesoblocks"]
    h2 = hashlib.sha256(open(MNT + "/t", "rb").read()).hexdigest()
    if h1 != h2: raise RuntimeError("%s: contents changed by restart (mapped %d -> %d)"
            % (fstype, before, after_restart))
    fs_check(fstype)
    mount(fstype); check_files(files); umount()
    i = info()
    msg = ("%s: mapped mesoblocks after mkfs %d, full %d, after rm %d, after fstrim %d, "
           "after umount and restart %d; tombstone slots %d" % (fstype, i0["mapped_mesoblocks"], full,
           after_rm, trimmed, after_restart, i["tombstone_slots"]))
    if fstype != "ext4":
        msg += "; writing 60 MiB took %d block writes untrimmed, %d trimmed" % (
                cost_untrimmed, cost_trimmed)
    print(msg, flush=True)
    if not trimmed < full * 0.5: raise RuntimeError("%s: fstrim did not free much" % fstype)
    if after_restart != before:
        raise RuntimeError("%s: mapped mesoblocks changed at restart (%d -> %d)" %
                (fstype, before, after_restart))
    cmd("close t"); stop()

os.makedirs(FS, exist_ok=True)
MKFS_XFS = os.environ.get("MKFS_XFS")
# xfs_repair: $XFS_REPAIR, or next to mkfs.xfs in an xfsprogs build tree, or
# in $PATH
XFS_REPAIR = os.environ.get("XFS_REPAIR") or (MKFS_XFS and
        os.path.exists(os.path.dirname(MKFS_XFS) + "/../repair/xfs_repair") and
        os.path.dirname(MKFS_XFS) + "/../repair/xfs_repair") or "xfs_repair"
parts = os.environ.get("PARTS", "ABC" + ("D" if MKFS_XFS else ""))
if "A" in parts: part_a(25); part_full()
if "B" in parts: part_b(); part_b2()
if "C" in parts:
    part_fs("ext2", ["mkfs.ext2", "-q", "-F"])
    part_fs("ext3", ["mkfs.ext3", "-q", "-F"])
    part_fs("ext4", ["mkfs.ext4", "-q", "-F"])
if "D" in parts: part_fs("xfs", [MKFS_XFS, "-q", "-f"])
print("DISCARDTEST OK", flush=True)
