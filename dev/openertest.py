#!/usr/bin/env python3
# openertest.py - several openers of a partition, and no EBUSY races:
# two file descriptors at once, mkfs/resize2fs/e2fsck directly on the
# partition (resize2fs opens it twice), close/resize right after umount or
# close() without retrying, close of a partition that is really in use,
# close-on-release with two openers, scubed3ctl refuses to mount twice
#
# usage: openertest.py DAEMON SCUBED3CTL ROUNDS
import os, sys, time, socket, hashlib, subprocess, errno
import shutil
FUSERMOUNT = shutil.which("fusermount3") or "fusermount"   # libfuse 3 or 2

B = os.environ.get("S3BUILD", os.getcwd())
MNT, FS, FS2, SOCK, MESO = "/mnt/s3", "/mnt/ot", "/mnt/ot2", "/run/scubed3/control", 1 << 14
KEY = hashlib.pbkdf2_hmac("sha256", b"opw", b"scubed3_prod", 1000, 32).hex()
daemon, ctl, rounds = sys.argv[1], sys.argv[2], int(sys.argv[3])
base = B + "/opener.base"
log = open(B + "/opener.log", "w")

def cmd(line):
    """one try, no retry on busy"""
    s = socket.socket(socket.AF_UNIX); s.connect(SOCK)
    s.sendall((line + "\nexit\n").encode())
    buf = b""
    while True:
        d = s.recv(65536)
        if not d: break
        buf += d
    return buf.decode()

def ok(line):
    t = cmd(line)
    if not t.startswith("OK"): raise RuntimeError("%s: %r" % (line, t))
    return t

def run(*a):
    r = subprocess.run(a, capture_output=True, text=True)
    if r.returncode: raise RuntimeError("%s: %s %s" % (a, r.stdout, r.stderr))
    return r.stdout

def size(): return os.stat(MNT + "/t").st_size

for d in (FS, FS2): os.makedirs(d, exist_ok=True)
with open(base, "wb") as f: f.write(os.urandom(40 << 22))
subprocess.run([FUSERMOUNT, "-u", "-z", MNT], capture_output=True)
if os.path.exists(SOCK): os.unlink(SOCK)
P = subprocess.Popen([B + "/" + daemon, "-b", base, "-f", MNT], stdout=log, stderr=log)
while not os.path.exists(SOCK): time.sleep(0.05)
time.sleep(0.1)
ok("create-internal t CBC_ESSIV(AES256) " + KEY)
ok("resize-internal t 10 3")

# two openers at the same time
a = os.open(MNT + "/t", os.O_RDWR); b = os.open(MNT + "/t", os.O_RDWR)
d1, d2 = os.urandom(MESO), os.urandom(MESO)
os.pwrite(a, d1, 0); os.pwrite(b, d2, MESO)
assert os.pread(b, MESO, 0) == d1 and os.pread(a, MESO, MESO) == d2
os.close(a); os.close(b)
print("two openers OK", flush=True)

# e2fsprogs directly on the partition, no loop device
run("mkfs.ext4", "-q", "-F", MNT + "/t")
ok("resize-internal t 14 4")
run("resize2fs", "-f", MNT + "/t")
run("e2fsck", "-fn", MNT + "/t")
print("mkfs/resize2fs/e2fsck directly on the partition OK", flush=True)

# close and resize right after umount or close(), no retries
waits = []
for r in range(rounds):
    run("mount", "-o", "loop", MNT + "/t", FS)
    data = os.urandom(300_000)
    open(FS + "/f%d" % r, "wb").write(data)
    run("umount", FS)
    t0 = time.time()
    ok("close t")          # the release of the loop device is async
    waits.append(time.time() - t0)
    ok("open-internal t CBC_ESSIV(AES256) " + KEY)
    fd = os.open(MNT + "/t", os.O_RDONLY); os.pread(fd, 4096, 0); os.close(fd)
    t0 = time.time()
    ok("resize-internal t %d 4" % (15 - r % 2))    # right after close()
    waits.append(time.time() - t0)
if size() != 10 * 255 * MESO: ok("resize-internal t 14 4")
run("e2fsck", "-fn", MNT + "/t")
print("close/resize right after umount/close: %d rounds without EBUSY, longest wait %.3fs"
        % (rounds, max(waits)), flush=True)

# close of a partition that is in use: busy (after waiting)
fd = os.open(MNT + "/t", os.O_RDONLY)
t0 = time.time(); t = cmd("close t")
assert t.startswith("ERR") and "busy" in t, t
print("close while open: %r after %.1fs" % (t.split("\n")[1], time.time() - t0), flush=True)
os.close(fd)

# close on release with two openers: closed when the last one is gone
# (open first: the release of the descriptor above may still be on its way)
a = os.open(MNT + "/t", os.O_RDONLY); b = os.open(MNT + "/t", os.O_RDONLY)
ok("set-close-on-release t 1")
os.close(a); time.sleep(0.3)
assert cmd("info t").startswith("OK"), "closed while still open"
os.close(b)
for _ in range(60):
    if not cmd("info t").startswith("OK"): break
    time.sleep(0.05)
else: raise RuntimeError("not closed on release")
print("close on release with two openers OK", flush=True)

# scubed3ctl: mount, a second mount is refused, umount
ok("open-internal t CBC_ESSIV(AES256) " + KEY)
# (in level 1 scubed3ctl warns, below the recommended level 2: yes)
out1 = subprocess.run([ctl, "-c", "mount t " + FS], input="yes\n", capture_output=True, text=True)
out2 = subprocess.run([ctl, "-c", "mount t " + FS2], capture_output=True, text=True)
assert "already mounted on " + FS in out2.stdout, (out1.stdout, out1.stderr, out2.stdout, out2.stderr)
assert os.path.ismount(FS) and not os.path.ismount(FS2)
for r in range(rounds): assert os.path.exists(FS + "/f%d" % r)
subprocess.run([ctl, "-c", "umount t"], capture_output=True, text=True)
ok("close t")
print("scubed3ctl: %s" % out2.stdout.strip(), flush=True)

run(FUSERMOUNT, "-u", MNT); P.wait(timeout=30)
print("OPENERTEST OK", flush=True)
