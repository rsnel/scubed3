#!/usr/bin/env python3
# ioerrtest.py - return values of reads and writes: end of the partition
# (short reads/writes, 0 at the end, ENOSPC for writes at the end), and
# read errors of the base device (SCUBED3_BAD_READ, needs a daemon built with
# -DSCUBED3_CRASH_TEST): the daemon keeps running, the mesoblock gives EIO,
# check-data-integrity reports it, rewriting repairs it; an unreadable index
# of an unallocated block is harmless, of a block of the partition: the
# partition is not opened (the daemon keeps running)
#
# usage: ioerrtest.py DAEMON (the -crash variant)
import os, sys, time, socket, hashlib, subprocess, errno
import shutil
FUSERMOUNT = shutil.which("fusermount3") or "fusermount"   # libfuse 3 or 2
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import s3read

B = os.environ.get("S3BUILD", os.getcwd())
MNT, SOCK, MESO, MACRO = "/mnt/s3", "/run/scubed3/control", 1 << 14, 1 << 22
PW = b"iopw"
KEY = hashlib.pbkdf2_hmac("sha256", PW, b"scubed3_prod", 1000, 32).hex()
daemon = sys.argv[1]
base = B + "/ioerr.base"
log = open(B + "/ioerr.log", "w")
P = None

def start(env={}, opts=()):
    global P
    subprocess.run([FUSERMOUNT, "-u", "-z", MNT], capture_output=True)
    if os.path.exists(SOCK): os.unlink(SOCK)
    P = subprocess.Popen([B + "/" + daemon, "-b", base, "-f", *opts, MNT], stdout=log,
            stderr=log, env=dict(os.environ, **env))
    while not os.path.exists(SOCK): time.sleep(0.05)
    time.sleep(0.1)

def stop():
    subprocess.run([FUSERMOUNT, "-u", MNT], check=True); P.wait(timeout=30)

def cmd(line, ok=True):
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
        if ok and not t.startswith("OK"): raise RuntimeError("%s: %r" % (line, t))
        return t
    raise RuntimeError("busy")

def opent(flags):
    for _ in range(100):
        try: return os.open(MNT + "/t", flags)
        except OSError as e:
            if e.errno != errno.EBUSY: raise
            time.sleep(0.05)
    raise RuntimeError("busy")

def pread(off, n):
    fd = opent(os.O_RDONLY)
    try: return os.pread(fd, n, off)
    finally: os.close(fd)

def pwrite(off, data):
    fd = opent(os.O_WRONLY)
    try: return os.pwrite(fd, data, off)
    finally: os.close(fd)

def expect_err(e, f, *a):
    try: f(*a)
    except OSError as ex:
        if ex.errno == e: return
        raise
    raise RuntimeError("%s%s did not fail with %s" % (f.__name__, a, errno.errorcode[e]))

def opn(): cmd("open-internal t CBC_ESSIV(AES256) " + KEY)

with open(base, "wb") as f: f.write(os.urandom(16 * MACRO))
data = {}

# end of the partition, with and without the page cache in between
for opts in ((), ("-o", "direct_io")):
    start(opts=opts)
    if not data:
        cmd("create-internal t CBC_ESSIV(AES256) " + KEY); cmd("resize-internal t 8 2")
    else: opn()
    S = os.stat(MNT + "/t").st_size
    assert S == 6 * 255 * MESO, S
    if not data:
        for m in range(S // MESO):
            d = os.urandom(MESO); pwrite(m * MESO, d); data[m] = d
    assert pread(S, 4096) == b"", "read at the end"
    assert pread(S + (1 << 40), 4096) == b"", "read far past the end"
    r = pread(S - 100, 4096)
    assert r == data[S // MESO - 1][-100:], "short read at the end: %d" % len(r)
    expect_err(errno.ENOSPC, pwrite, S, b"x" * 4096)
    expect_err(errno.ENOSPC, pwrite, S + 12345, b"x" * 10)
    d = os.urandom(4096)
    n = pwrite(S - 1000, d)
    assert n == 1000, "short write at the end: %d" % n
    last = data[S // MESO - 1]; data[S // MESO - 1] = last[:-1000] + d[:1000]
    assert pread(S - MESO, MESO) == data[S // MESO - 1]
    assert os.stat(MNT + "/t").st_size == S
    r = subprocess.run(["dd", "if=/dev/zero", "of=" + MNT + "/t", "bs=4096", "count=2",
            "seek=%d" % (S // 4096), "conv=notrunc"], capture_output=True, text=True)
    assert r.returncode != 0 and "No space left" in r.stderr, r.stderr
    cmd("close t"); stop()
    print("end of partition OK%s" % (" (direct_io)" if opts else ""), flush=True)

# read errors of the base device
st = s3read.Store(base, PW, 1000)
latest = st.blocks[-1][1]
victims = sorted(m for m, (blk, k, s, h) in st.loc.items() if blk != latest)[::997][:3]
bad = [st.loc[m][0] * MACRO + (st.loc[m][1] + 1) * MESO + 5000 for m in victims]
start({"SCUBED3_BAD_READ": ",".join(map(str, bad))}); opn()
for m in range(len(data)):
    if m in victims:
        expect_err(errno.EIO, pread, m * MESO, MESO)
    else: assert pread(m * MESO, MESO) == data[m], m
t = cmd("check-data-integrity t")
assert "corrupt_mesoblocks=%d" % len(victims) in t, t
for m in victims:   # repair by rewriting
    d = os.urandom(MESO); pwrite(m * MESO, d); data[m] = d
cmd("cycle t 30")   # the GC also reads around the bad sectors
t = cmd("check-data-integrity t")
assert "corrupt_mesoblocks=0" in t, t
for m in range(len(data)): assert pread(m * MESO, MESO) == data[m], m
assert P.poll() is None, "daemon died"
cmd("close t"); stop()
print("read errors in data OK (%d warnings)" % open(B + "/ioerr.log").read().count("error reading"),
        flush=True)

# unreadable index blocks: of an unallocated block (harmless) and of a block
# of the partition (opening fails)
st = s3read.Store(base, PW, 1000)
ours = {m for s, m, i in st.live}
free = [b for b in range(16) if b not in ours]
start({"SCUBED3_BAD_READ": str(free[0] * MACRO + 7)}); opn()
for m in range(len(data)): assert pread(m * MESO, MESO) == data[m], m
cmd("close t"); stop()
print("unreadable index of an unallocated block OK", flush=True)
start({"SCUBED3_BAD_READ": str(sorted(ours)[0] * MACRO + 7)})
t = cmd("open-internal t CBC_ESSIV(AES256) " + KEY, ok=False)
assert t.startswith("ERR") and "cannot be read" in t, t
assert P.poll() is None, "daemon died"
print("unreadable index of a block of the partition: %s" % t.split("\n")[1], flush=True)
stop()
start(); opn()
for m in range(len(data)): assert pread(m * MESO, MESO) == data[m], m
cmd("close t"); stop()
print("IOERRTEST OK", flush=True)
