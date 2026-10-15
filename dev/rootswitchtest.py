#!/usr/bin/env python3
# rootswitchtest.py - scubed3 keeps working when its root directory is
# emptied after the start, as with root on scubed3: the daemon is started in
# the initramfs, run-init deletes the files of the initramfs and switches to
# the real root, the daemon keeps running (see contrib/initramfs-tools)
#
# the daemon (with its libraries, /dev/fuse and the base file) is started in
# a chroot, then everything in the chroot is deleted; partitions are
# created, written, closed, opened again and read, also in paranoia level 3;
# afterwards the base file (a hard link outside the chroot) is opened by a
# normal daemon, the data must be there
#
# usage: rootswitchtest.py DAEMON SEED
# (small blocks: -m 12 -M 16, 64 KiB macroblocks)
import os, sys, time, socket, random, hashlib, subprocess, errno, shutil, atexit, tempfile
FUSERMOUNT = shutil.which("fusermount3") or "fusermount"   # libfuse 3 or 2

B = os.environ.get("S3BUILD", os.getcwd())
M_LOG, MM_LOG = 12, 16
MESO, MACRO = 1 << M_LOG, 1 << MM_LOG
MMPM = MACRO // MESO - 1
TOTAL = 100
daemon, seed = sys.argv[1], int(sys.argv[2])
rng = random.Random(seed)
log = open(B + "/rootswitch.log", "w")
T = tempfile.mkdtemp(prefix="s3root")
MNT, SOCK = T + "/mnt", T + "/run/scubed3/control"
keep = B + "/rootswitch.base"
P = None

def cleanup():
    if P and P.poll() is None: P.kill(); P.wait()
    subprocess.run([FUSERMOUNT, "-u", "-z", MNT], capture_output=True)
    subprocess.run([FUSERMOUNT, "-u", "-z", "/mnt/s3"], capture_output=True)
    shutil.rmtree(T, ignore_errors=True)
atexit.register(cleanup)

def key(name): return hashlib.pbkdf2_hmac("sha256", name.encode(), b"scubed3_prod", 1000, 32).hex()
def reserved(n): return max(1, n // 4)
def usable(n): return (n - reserved(n)) * MMPM

def cmd(line, ok=True, sock=SOCK):
    s = socket.socket(socket.AF_UNIX); s.connect(sock)
    s.sendall((line + "\nexit\n").encode()); buf = b""
    while True:
        d = s.recv(65536)
        if not d: break
        buf += d
    t = buf.decode()
    if ok and not t.startswith("OK"): raise RuntimeError("%s: %r" % (line, t))
    return t

def opent(mnt, name, flags):
    for _ in range(400):
        try: return os.open(mnt + "/" + name, flags)
        except OSError as e:
            if e.errno != errno.EBUSY: raise
            time.sleep(0.05)
    raise RuntimeError("busy")

errors = []
def check(cond, what):
    print(("   ok: " if cond else "   FAILED: ") + what, flush=True)
    if not cond: errors.append(what)

# ------------------------------------------------------------- the chroot
binary = os.path.realpath(B + "/" + daemon)
libs = [l.split("=>")[-1].split()[0] for l in subprocess.run(["ldd", binary],
        capture_output=True, text=True).stdout.splitlines() if "/" in l]
for f in [binary] + libs:
    d = T + os.path.dirname(f) if f != binary else T
    os.makedirs(d, exist_ok=True)
    shutil.copy2(f, d + "/" + (os.path.basename(f) if f != binary else "scubed3"))
os.makedirs(T + "/dev"); os.makedirs(MNT); os.makedirs(T + "/run")
os.mknod(T + "/dev/fuse", 0o20666, os.makedev(10, 229))
os.mknod(T + "/dev/null", 0o20666, os.makedev(1, 3))
os.mknod(T + "/dev/urandom", 0o20666, os.makedev(1, 9))   # (an initramfs has it)
with open(T + "/base", "wb") as f: f.write(os.urandom(TOTAL * MACRO))
if os.path.exists(keep): os.unlink(keep)
os.link(T + "/base", keep)

subprocess.run([FUSERMOUNT, "-u", "-z", MNT], capture_output=True)
P = subprocess.Popen(["chroot", T, "/scubed3", "-b", "/base", "-m", str(M_LOG), "-M",
    str(MM_LOG), "-f", "-o", "direct_io", "/mnt"], stdout=log, stderr=log)
for _ in range(200):
    if os.path.exists(SOCK): break
    if P.poll() is not None: raise RuntimeError("daemon did not start")
    time.sleep(0.05)
time.sleep(0.2)

# run-init: everything is deleted (not the mount point and the directory
# of the socket)
for e in os.listdir(T):
    if e in ("mnt", "run"): continue
    p = T + "/" + e
    if os.path.isdir(p) and not os.path.islink(p): shutil.rmtree(p)
    else: os.unlink(p)
print("1. the root directory of the daemon is empty: %s" % sorted(os.listdir(T)), flush=True)

# ------------------------------------------------------------- partitions
model = {}
def fill(name, nb, n):
    fd = opent(MNT, name, os.O_RDWR); m = model.setdefault(name, {})
    for _ in range(n):
        i = rng.randrange(usable(nb)); d = os.urandom(MESO)
        os.pwrite(fd, d, i * MESO); m[i] = d
    os.fsync(fd); os.close(fd)

def verify(mnt, name, nb, when):
    fd = opent(mnt, name, os.O_RDONLY); bad = 0
    for i in range(usable(nb)):
        if os.pread(fd, MESO, i * MESO) != model[name].get(i, bytes(MESO)): bad += 1
    os.close(fd)
    check(not bad, "%s: %s is intact (%d bad mesoblocks)" % (when, name, bad))

try:
    print("2. partitions are created, written, closed and opened again", flush=True)
    for n, nb in (("a", 30), ("b", 30)):
        cmd("create-internal %s CBC_ESSIV(AES256) %s" % (n, key(n)))
        cmd("resize-internal %s %d %d" % (n, nb, reserved(nb)))
        fill(n, nb, 300)
    cmd("close a")
    cmd("open-internal a CBC_ESSIV(AES256) %s" % key("a"))
    verify(MNT, "a", 30, "opened again")
    print("3. paranoia level 3 (the clock thread writes too)", flush=True)
    cmd("paranoia 3 6400")
    fill("a", 30, 100); fill("b", 30, 100)
    cmd("paranoia 1")
    verify(MNT, "a", 30, "level 3"); verify(MNT, "b", 30, "level 3")
    t = cmd("p")
    check("blocks of the free space" in t and "paranoia level 1" in t, "p works")
    for n in ("a", "b"): cmd("close " + n)
except (RuntimeError, OSError) as e:
    check(False, "the daemon fails after its root was emptied: %s" % e)
alive = P.poll() is None
check(alive, "the daemon is still running")
subprocess.run([FUSERMOUNT, "-u", MNT], capture_output=True)
try: P.wait(timeout=60)
except subprocess.TimeoutExpired: P.kill(); P.wait()

# ------------------------------------------------------------- afterwards
if model and not errors:
    print("4. the base file is opened by a normal daemon", flush=True)
    S = "/run/scubed3/control"
    subprocess.run([FUSERMOUNT, "-u", "-z", "/mnt/s3"], capture_output=True)
    if os.path.exists(S): os.unlink(S)
    P = subprocess.Popen([B + "/" + daemon, "-b", keep, "-m", str(M_LOG), "-M", str(MM_LOG),
        "-f", "-o", "direct_io", "/mnt/s3"], stdout=log, stderr=log)
    while not os.path.exists(S): time.sleep(0.05)
    time.sleep(0.1)
    for n in ("a", "b"):
        cmd("open-internal %s CBC_ESSIV(AES256) %s" % (n, key(n)), sock=S)
        verify("/mnt/s3", n, 30, "after a restart")
        cmd("close " + n, sock=S)
    subprocess.run([FUSERMOUNT, "-u", "/mnt/s3"], capture_output=True); P.wait(timeout=60)
os.unlink(keep)
print("ROOTSWITCHTEST %s" % ("OK" if not errors else "FAILED: %s" % errors), flush=True)
