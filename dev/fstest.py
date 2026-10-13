#!/usr/bin/env python3
# fstest.py - ext2 on a scubed3 partition through grow/shrink cycles
#
# usage: fstest.py DAEMON CYCLES SEED
#
# every cycle: shrink the filesystem (resize2fs), shrink the partition,
# restart the daemon while the shrink is unfinished, write files, grow the
# partition, grow the filesystem; after every step e2fsck -fn and checksums
import os, sys, time, socket, random, hashlib, subprocess

B = os.environ.get("S3BUILD", os.getcwd())
MNT, FS, SOCK = "/mnt/s3", "/mnt/fst", "/run/scubed3/control"
KEY = hashlib.pbkdf2_hmac("sha256", b"fpw", b"scubed3_prod", 1000, 32).hex()
daemon, cycles, seed = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
rng = random.Random(seed)
base = B + "/fstest.base"
log = open(B + "/fstest.log", "w")
P = None
files = {}      # name -> sha256

def sh(*a, ok=(0,)):
    r = subprocess.run(a, capture_output=True, text=True)
    if r.returncode not in ok:
        raise RuntimeError("%s: rc=%d %s %s" % (a, r.returncode, r.stdout, r.stderr))
    return r.stdout

def start():
    global P
    sh("fusermount", "-u", "-z", MNT, ok=(0, 1))
    if os.path.exists(SOCK): os.unlink(SOCK)
    P = subprocess.Popen([B + "/" + daemon, "-b", base, "-f", MNT],
            stdout=log, stderr=log)
    while not os.path.exists(SOCK): time.sleep(0.05)
    time.sleep(0.1)

def stop():
    sh("fusermount", "-u", MNT)
    P.wait(timeout=30)

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

def info():
    return dict(l.split("=") for l in cmd("info t").split("\n") if "=" in l)

def reserved(n): return (n - 1) // 4 + 1

def resize(n): cmd("resize-internal t %d %d" % (n, reserved(n)))

def kib(): return os.stat(MNT + "/t").st_size // 1024

def mount():
    for _ in range(50):
        r = subprocess.run(["mount", "-o", "loop", MNT + "/t", FS], capture_output=True)
        if r.returncode == 0: return
        time.sleep(0.2)
    raise RuntimeError("mount failed: %s" % r.stderr)

def umount():
    sh("umount", FS)
    time.sleep(0.3)

def on_loop(argv):
    """run an e2fsprogs tool on the partition through a fresh loop device,
    scubed3 allows one opener only and resize2fs opens the file twice"""
    for _ in range(50):
        r = subprocess.run(["losetup", "-f", "--show", MNT + "/t"],
                capture_output=True, text=True)
        if r.returncode == 0: break
        time.sleep(0.2)
    else: raise RuntimeError("losetup: " + r.stderr)
    dev = r.stdout.strip()
    try:
        opts = [a for a in argv[1:] if a.startswith("-")]
        rest = [a for a in argv[1:] if not a.startswith("-")]
        r = subprocess.run(argv[:1] + opts + [dev] + rest, capture_output=True, text=True)
        if r.returncode != 0:
            raise RuntimeError("%s: %s %s" % (argv[0], r.stdout, r.stderr))
    finally:
        subprocess.run(["losetup", "-d", dev])
        time.sleep(0.3)

def fsck(): on_loop(["e2fsck", "-fn"])

def resize2fs(*a): on_loop(["resize2fs", "-f"] + list(a))

def check_files():
    mount()
    for name, h in files.items():
        if hashlib.sha256(open(FS + "/" + name, "rb").read()).hexdigest() != h:
            raise RuntimeError("file %s corrupt" % name)
    have = set(os.listdir(FS)) - {"lost+found"}
    if have != set(files): raise RuntimeError("files differ: %s" % (have ^ set(files)))
    umount()

def add_files(n, maxsize):
    mount()
    st = os.statvfs(FS)
    free = st.f_bavail * st.f_frsize
    for _ in range(n):
        size = min(rng.randint(1, maxsize), int(free * 0.4))
        if size < 4096: break
        data = os.urandom(size)
        name = "f%06d" % rng.randrange(10**6)
        open(FS + "/" + name, "wb").write(data)
        files[name] = hashlib.sha256(data).hexdigest()
        free -= size
    # also delete some
    for name in rng.sample(sorted(files), len(files) // 4):
        os.unlink(FS + "/" + name); del files[name]
    os.sync()
    umount()

def used_kib():
    mount()
    st = os.statvfs(FS)
    u = (st.f_blocks - st.f_bfree) * st.f_frsize // 1024
    umount()
    return u

# setup
os.makedirs(FS, exist_ok=True)
with open(base, "wb") as f: f.write(os.urandom(48 << 22))
start()
cmd("create-internal t CBC_ESSIV(AES256) " + KEY)
n = 20
resize(n)
on_loop(["mke2fs", "-q", "-F", "-b1024"])
add_files(30, 2_000_000)
fsck(); check_files()
print("setup: %d macroblocks, %d KiB, %d files" % (n, kib(), len(files)), flush=True)

for c in range(cycles):
    # shrink: filesystem first, then the partition
    used = used_kib()
    smallest = 1
    while (smallest - reserved(smallest)) * 255 * 16 < used * 1.6 + 2048: smallest += 1
    new = rng.randint(smallest, max(smallest, n - 1))
    newkib = (new - reserved(new)) * 255 * 16
    if new < n:
        fsck()
        resize2fs("%dK" % newkib)
        resize(new)
        p = int(info()["pending_removals"])
        fsck(); check_files()
        # interrupt the unfinished shrink: restart the daemon
        cmd("close t"); stop(); start()
        cmd("open-internal t CBC_ESSIV(AES256) " + KEY)
        p2 = int(info()["pending_removals"])
        if kib() != newkib: raise RuntimeError("size after restart %d != %d" % (kib(), newkib))
        fsck(); check_files()
        print("cycle %d: shrunk %d -> %d macroblocks (fs used %d KiB), pending %d, "
                "after restart pending %d" % (c, n, new, used, p, p2), flush=True)
        n = new
    # write while the removal may still be in progress
    add_files(rng.randint(3, 10), 1_000_000)
    fsck(); check_files()
    # grow the partition, then the filesystem
    new = rng.randint(n + 1, 30)
    resize(new)
    resize2fs()
    fsck(); check_files()
    add_files(rng.randint(5, 15), 3_000_000)
    fsck(); check_files()
    print("cycle %d: grown %d -> %d macroblocks, %d files, pending %s" %
            (c, n, new, len(files), info()["pending_removals"]), flush=True)
    n = new
    if rng.random() < 0.5:
        cmd("close t"); stop(); start()
        cmd("open-internal t CBC_ESSIV(AES256) " + KEY)
        fsck(); check_files()

cmd("close t"); stop(); start()
cmd("open-internal t CBC_ESSIV(AES256) " + KEY)
fsck(); check_files()
print("FSTEST OK: %d cycles, %d files, discard writes: %d" % (cycles, len(files),
        open(B + "/fstest.log").read().count("discarding block")), flush=True)
cmd("close t"); stop()
