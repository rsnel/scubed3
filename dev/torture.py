#!/usr/bin/env python3
# torture.py - randomized grow/shrink/write/restart/crash test for scubed3
#
# usage: torture.py DAEMON STEPS SEED [crash|tear]
# (crash and tear need the daemon built with -DSCUBED3_CRASH_TEST;
# PARANOIA=0 or PARANOIA=2 runs in that paranoia level, in level 2 the free
# space of the base file is overwritten too)
#
# The partition "t" is written through the FUSE file with self-describing
# mesoblocks (index, generation, hash-derived filler).  An oracle tracks what
# every mesoblock must contain:
#  - after a clean restart: exactly the last value written, zero if never
#    written since it (re)entered the partition (resurrection check)
#  - after a crash: the value at the last durable point (cycle/close), or any
#    value written since; a resize has its own window, after a crash the
#    partition is either completely pre-resize or completely post-resize
import os, sys, time, socket, random, hashlib, struct, subprocess, errno, ctypes

B = os.environ.get("S3BUILD", os.getcwd())
MNT = "/mnt/s3"
SOCK = "/run/scubed3/control"
MESO, MMPM = 16384, 255
TOTAL = 40                      # macroblocks in the base file
PW, SALT, ITER = b"tpw", b"scubed3_prod", 1000
KEY = hashlib.pbkdf2_hmac("sha256", PW, SALT, ITER, 32).hex()

daemon_path = os.path.join(B, sys.argv[1])
steps, seed = int(sys.argv[2]), int(sys.argv[3])
crash_mode = len(sys.argv) > 4 and sys.argv[4] in ("crash", "tear")
tear_mode = len(sys.argv) > 4 and sys.argv[4] == "tear"
CRASH_MAX = int(os.environ.get("CRASH_MAX", "12"))
last_pending = 0
rng = random.Random(seed)
base = os.path.join(B, "torture.base")
log = open(os.path.join(B, "torture.log"), "w")

def reserved(n): return (n - 1) // 4 + 1 if n > 0 else 0
def usable(n): return max(0, n - reserved(n)) * MMPM

_pat = {}
def pattern(idx, gen):
    h = hashlib.sha256(b"%d,%d" % (idx, gen)).digest()
    return struct.pack("<4sIQ", b"S3T!", idx, gen) + (h * 512)[16:]

def decode(idx, data):
    if data == bytes(MESO): return 0
    magic, i, gen = struct.unpack("<4sIQ", data[:16])
    if magic != b"S3T!" or i != idx or data != pattern(idx, gen):
        return "CORRUPT"
    return gen

class Crashed(Exception): pass

class Daemon:
    def __init__(self): self.p = None
    def start(self, crash_after=None):
        subprocess.run(["fusermount", "-u", "-z", MNT], stderr=subprocess.DEVNULL)
        if os.path.exists(SOCK): os.unlink(SOCK)
        env = dict(os.environ)
        env.pop("SCUBED3_CRASH_AFTER", None)
        if crash_after: env["SCUBED3_CRASH_AFTER"] = str(crash_after)
        env.pop("SCUBED3_TEAR", None)
        if crash_after and tear_mode: env["SCUBED3_TEAR"] = "1"
        self.p = subprocess.Popen([daemon_path, "-b", base, "-f", "-o",
            "direct_io,attr_timeout=0,entry_timeout=0", MNT],
            stdout=log, stderr=log, env=env)
        for _ in range(100):
            if os.path.exists(SOCK): break
            time.sleep(0.05)
        else: raise RuntimeError("daemon did not start")
        time.sleep(0.1)
        if self.p.poll() is not None and not crash_after:
            raise RuntimeError("daemon exited at start (another one running?)")
    def alive(self): return self.p and self.p.poll() is None
    def dying(self):
        """wait a moment, a crashing daemon may not have exited yet"""
        try: self.p.wait(timeout=3); return True
        except subprocess.TimeoutExpired: return False
    def stop(self):
        if self.alive():
            subprocess.run(["fusermount", "-u", MNT], stderr=subprocess.DEVNULL)
            self.p.wait(timeout=20)
        subprocess.run(["fusermount", "-u", "-z", MNT], stderr=subprocess.DEVNULL)
        self.p = None

D = Daemon()

def cmd(line, ok_errors=()):
    """send one command to the control socket, return (ok, text)"""
    for attempt in range(50):
        try:
            s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            s.connect(SOCK)
            s.sendall(line.encode() + b"\nexit\n")
            buf = b""
            while True:
                d = s.recv(65536)
                if not d: break
                buf += d
            s.close()
        except OSError:
            if D.dying(): raise Crashed()
            raise
        if not buf and D.dying(): raise Crashed()
        if not D.alive() and not buf.endswith(b"OK\n.\n"): raise Crashed()
        text = buf.decode()
        ok = text.startswith("OK")
        if not ok and "busy" in text:   # FUSE release is asynchronous
            time.sleep(0.1); continue
        if not ok and not any(e in text for e in ok_errors):
            raise RuntimeError("command %r failed: %r" % (line, text))
        return ok, text
    raise RuntimeError("partition stays busy")

def fopen():
    for _ in range(100):
        try: return os.open(MNT + "/t", os.O_RDWR)
        except OSError as e:
            if e.errno == errno.EBUSY: time.sleep(0.05); continue
            if D.dying(): raise Crashed()
            raise
    raise RuntimeError("file stays busy")

def fsize():
    try: return os.stat(MNT + "/t").st_size // MESO
    except OSError:
        if D.dying(): raise Crashed()
        raise

def info():
    ok, t = cmd("info t")
    return dict(l.split("=") for l in t.split("\n") if "=" in l)

# ---- oracle state
size = 0                # current logical size in mesoblocks (as we expect it)
nblocks = 0             # macroblocks (target)
model = {}              # idx -> gen (absent = 0)
durable = None          # (nblocks, size, model) at last durable point
window = {}             # idx -> set of gens written since durable point
gen = 0
stats = dict(writes=0, mesoblocks=0, grows=0, shrinks=0, restarts=0,
        crashes=0, crash_in_resize=0, verifies=0, pending_seen=0,
        max_pending=0, crash_while_pending=0, fsyncs=0, discards=0,
        discarded=0, max_tombstone_slots=0)

libc = ctypes.CDLL(None, use_errno=True)
FALLOC_KEEP_SIZE, FALLOC_PUNCH_HOLE, FALLOC_ZERO_RANGE = 1, 2, 0x10
def fallocate(fd, mode, off, ln):
    if libc.fallocate(fd, mode, ctypes.c_long(off), ctypes.c_long(ln)) != 0:
        e = ctypes.get_errno()
        raise OSError(e, os.strerror(e))

def mark_durable():
    global durable, window
    durable = (nblocks, size, dict(model))
    window = {}

def verify(allowed_fn, sz):
    """read the whole partition and check every mesoblock"""
    fd = fopen()
    try:
        bad = []
        for start in range(0, sz, 64):
            n = min(64, sz - start)
            data = os.pread(fd, n * MESO, start * MESO)
            if len(data) != n * MESO:
                raise RuntimeError("short read at %d" % start)
            for k in range(n):
                idx = start + k
                g = decode(idx, data[k*MESO:(k+1)*MESO])
                if g not in allowed_fn(idx):
                    bad.append((idx, g, sorted(allowed_fn(idx))[:4]))
                else: model_set(idx, g)
        if bad:
            raise RuntimeError("%d bad mesoblocks, first: %s" % (len(bad), bad[:5]))
    finally: os.close(fd)
    stats["verifies"] += 1

def model_set(idx, g):
    if g: model[idx] = g
    else: model.pop(idx, None)

PARANOIA = os.environ.get("PARANOIA")
def reopen(crash_after=None):
    D.start(crash_after)
    cmd("open-internal t CBC_ESSIV(AES256) " + KEY)
    if PARANOIA: cmd("paranoia " + PARANOIA)

def do_write():
    global gen
    fd = fopen()
    try:
        n = rng.choice([1, 8, 64, 200, 400, 700])
        seq = rng.random() < 0.5
        first = rng.randrange(size)
        idxs = [(first + k) % size for k in range(n)] if seq else \
               [rng.randrange(size) for _ in range(n)]
        for idx in idxs:
            gen += 1
            window.setdefault(idx, set()).add(gen)
            try:
                if os.pwrite(fd, pattern(idx, gen), idx * MESO) != MESO:
                    raise RuntimeError("short write")
            except OSError:
                if D.dying(): raise Crashed()
                raise
            model[idx] = gen
            stats["mesoblocks"] += 1
        if rng.random() < 0.35:
            # fsync: everything written so far must survive a crash
            try: os.fsync(fd)
            except OSError:
                if D.dying(): raise Crashed()
                raise
            mark_durable()
            stats["fsyncs"] += 1
    finally:
        try: os.close(fd)
        except OSError: pass
    stats["writes"] += 1

def do_discard():
    """punch holes (or zero ranges) of whole mesoblocks"""
    fd = fopen()
    try:
        n = rng.choice([1, 3, 50, 300, size])
        first = rng.randrange(size)
        n = min(n, size - first)
        mode = rng.choice([FALLOC_PUNCH_HOLE|FALLOC_KEEP_SIZE, FALLOC_ZERO_RANGE])
        for idx in range(first, first + n): window.setdefault(idx, set()).add(0)
        try: fallocate(fd, mode, first * MESO, n * MESO)
        except OSError:
            if D.dying(): raise Crashed()
            raise
        for idx in range(first, first + n): model.pop(idx, None)
        stats["discards"] += 1; stats["discarded"] += n
        if rng.random() < 0.35:
            try: os.fsync(fd)
            except OSError:
                if D.dying(): raise Crashed()
                raise
            mark_durable()
            stats["fsyncs"] += 1
    finally:
        try: os.close(fd)
        except OSError: pass

def tombstone_slots():
    t = int(info()["tombstone_slots"])
    stats["max_tombstone_slots"] = max(stats["max_tombstone_slots"], t)
    return t

def do_cycle():
    cmd("cycle t 1")
    mark_durable()

def do_resize(new):
    global nblocks, size
    old_size = size
    cmd("resize-internal t %d %d" % (new, reserved(new)))
    if usable(new) < old_size:
        for idx in [i for i in model if i >= usable(new)]: del model[idx]
        stats["shrinks"] += 1
    elif usable(new) > old_size: stats["grows"] += 1
    nblocks, size = new, usable(new)
    if fsize() != size: raise RuntimeError("size %d != %d" % (fsize(), size))
    # resize gets its own durability window
    do_cycle()

def check_pending():
    i = info()
    global last_pending
    p = int(i["pending_removals"])
    last_pending = p
    if p: stats["pending_seen"] += 1
    stats["max_pending"] = max(stats["max_pending"], p)
    return p

def recover_after_crash(resize_window):
    """the daemon died: restart, open and check against the oracle"""
    stats["crashes"] += 1
    if last_pending: stats["crash_while_pending"] += 1
    attempts = 0
    while True:
        D.p.wait()
        attempts += 1
        try: return check_after_crash(resize_window, attempts > 2)
        except Crashed: stats["crashes"] += 1   # crashed again while opening

def check_after_crash(resize_window, no_crash=False):
    global nblocks, size
    pre_nb, pre_size, pre_model = durable
    # after repeated crashes while opening, open once without crashing
    reopen(rng.randint(1, CRASH_MAX) if crash_mode and not no_crash else None)
    actual = fsize()
    if resize_window is not None:
        stats["crash_in_resize"] += 1
        post_nb, post_size, post_model = resize_window
        # the partition is either completely pre or post resize
        cand = [(post_nb, post_size, post_model), (pre_nb, pre_size, pre_model)]
        last_err = None
        actual_nb = int(info()["no_macroblocks"])
        for nb, sz, m in cand:
            if sz != actual or nb != actual_nb: continue
            try:
                model.clear(); model.update(m)
                verify(lambda idx: {m.get(idx, 0)}, sz)
                nblocks, size = nb, sz
                mark_durable()
                return
            except RuntimeError as e: last_err = e
        raise RuntimeError("after crash in resize window: size %d, pre %d, "
                "post %d: %s" % (actual, pre_size, post_size, last_err))
    if actual != pre_size:
        raise RuntimeError("size after crash %d != %d" % (actual, pre_size))
    model.clear(); model.update(pre_model)
    w = window
    if os.environ.get("TORTURE_SNAP"):       # debugging: keep the state
        subprocess.run(["cp", base, base + ".snap"])
    verify(lambda idx: {pre_model.get(idx, 0)} | w.get(idx, set()), pre_size)
    mark_durable()
    if os.environ.get("TORTURE_SNAP"):
        subprocess.run(["cp", base + ".snap", base + ".good"])
        import json
        json.dump({"model": model, "durable_size": size}, open(base + ".good.json", "w"))

def clean_restart():
    cmd("close t")
    mark_durable()          # everything is written at close
    D.stop()
    reopen(rng.randint(1, CRASH_MAX) if crash_mode else None)
    if fsize() != size:
        raise RuntimeError("size after restart %d != %d" % (fsize(), size))
    verify(lambda idx: {model.get(idx, 0)}, size)
    stats["restarts"] += 1

# ---- setup
subprocess.run(["fusermount", "-u", "-z", MNT], stderr=subprocess.DEVNULL)
os.makedirs(MNT, exist_ok=True)
with open(base, "wb") as f: f.write(os.urandom(TOTAL << 22))
D.start()
cmd("create-internal t CBC_ESSIV(AES256) " + KEY)
if PARANOIA: cmd("paranoia " + PARANOIA)
nblocks = 12
cmd("resize-internal t %d %d" % (nblocks, reserved(nblocks)))
size = usable(nblocks)
cmd("cycle t 1")
mark_durable()
if crash_mode:          # from now on, the daemon may crash
    cmd("close t"); D.stop(); reopen(rng.randint(3, 60))

step = 0
try:
    while step < steps:
        step += 1
        r = rng.random()
        resize_window = None
        try:
            if r < 0.45 and size > 0:
                do_write()
                if rng.random() < 0.5: do_cycle()
            elif r < 0.55 and size > 0:
                do_discard()
                if rng.random() < 0.5: do_cycle()
                tombstone_slots()
            elif r < 0.75:
                new = rng.choice([rng.randint(2, 6), rng.randint(2, 30)])  # 0: gone, 1: refused
                if new == nblocks: continue
                do_cycle()      # the resize gets a window of its own
                post_model = {i: g for i, g in model.items() if i < usable(new)}
                resize_window = (new, usable(new), post_model)
                do_resize(new)
                resize_window = None
                check_pending()
            elif r < 0.85:
                if size > 0 and rng.random() < 0.7: do_write()
                clean_restart()
            else:
                do_cycle()
                check_pending()
        except Crashed:
            recover_after_crash(resize_window)
        if step % 25 == 0:
            print("step %d: nblocks=%d size=%d stats=%s" % (step, nblocks, size,
                stats), flush=True)
    # final: let pending removals finish, then a clean restart and check
    if crash_mode:
        # the daemon may crash while closing: then everything since the
        # last durable point may or may not be there
        try:
            cmd("close t")
            mark_durable()
        except Crashed: pass
        D.stop(); reopen(None)
        pre_model = durable[2]
        verify(lambda idx: {pre_model.get(idx, 0)} | window.get(idx, set()), size)
        mark_durable()
    for _ in range(200):
        if not check_pending(): break
        do_cycle()
    pend = check_pending()
    # tombstones die out when no block has an old version anymore (the
    # daemon does not crash here, see above)
    # (not in paranoia level 0: a block with a lot of live data is not
    # rewritten as long as there are emptier blocks, so an old version in it
    # may stay on disk, and its tombstone with it, see the README)
    for cycles in range(3000):
        if not tombstone_slots(): break
        do_cycle()
    else:
        if PARANOIA != "0":
            raise RuntimeError("tombstones do not die out: %d slots" %
                    tombstone_slots())
        stats["tombstones_left"] = tombstone_slots()
    stats["cycles_until_no_tombstones"] = cycles
    clean_restart()
    lg = open(os.path.join(B, "torture.log")).read()
    stats["discard_writes"] = lg.count("discarding block")
    stats["torn_blocks_ignored"] = lg.count("is incomplete (interrupted write)")
    print("FINAL OK: steps=%d nblocks=%d size=%d pending_after=%d %s" %
            (steps, nblocks, size, pend, stats), flush=True)
finally:
    D.stop()
