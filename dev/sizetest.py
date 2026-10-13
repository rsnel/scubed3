#!/usr/bin/env python3
# sizetest.py - non-default mesoblock and macroblock sizes (-m/-M): ext4 with
# files, fstrim, restart, e2fsck, contents
#
# usage: sizetest.py DAEMON
import os, sys, time, socket, hashlib, subprocess, random
import shutil
FUSERMOUNT = shutil.which("fusermount3") or "fusermount"   # libfuse 3 or 2

B = os.environ.get("S3BUILD", os.getcwd())
MNT, FS, SOCK = "/mnt/s3", "/mnt/sz", "/run/scubed3/control"
KEY = hashlib.pbkdf2_hmac("sha256", b"spw", b"scubed3_prod", 1000, 32).hex()
base = B + "/size.base"
rng = random.Random(1)

def cmd(line):
    s = socket.socket(socket.AF_UNIX); s.connect(SOCK)
    s.sendall((line + "\nexit\n").encode()); buf = b""
    while True:
        d = s.recv(65536)
        if not d: break
        buf += d
    if not buf.startswith(b"OK"): raise RuntimeError("%s: %r" % (line, buf))
    return buf.decode()

def run(*a):
    r = subprocess.run(a, capture_output=True, text=True)
    if r.returncode: raise RuntimeError("%s: %s %s" % (a, r.stdout, r.stderr))

def start(opts):
    subprocess.run([FUSERMOUNT, "-u", "-z", MNT], capture_output=True)
    if os.path.exists(SOCK): os.unlink(SOCK)
    p = subprocess.Popen([B + "/" + sys.argv[1], "-b", base, *opts, "-f", MNT],
            stdout=open(B + "/size.log", "a"), stderr=subprocess.STDOUT)
    while not os.path.exists(SOCK):
        if p.poll() is not None: raise RuntimeError("daemon exited")
        time.sleep(0.05)
    time.sleep(0.1)
    return p

def stop(p): run(FUSERMOUNT, "-u", MNT); p.wait(timeout=60)

os.makedirs(FS, exist_ok=True)
open(B + "/size.log", "w").close()
for m, M, nblocks in ((12, 20, 160), (13, 21, 80), (14, 22, 40), (15, 21, 80), (16, 24, 12)):
    with open(base, "wb") as f: f.write(os.urandom(nblocks << M))
    opts = ["-m", str(m), "-M", str(M)]
    p = start(opts)
    cmd("create-internal t CBC_ESSIV(AES256) " + KEY)
    n = nblocks * 3 // 4
    cmd("resize-internal t %d %d" % (n, (n - 1) // 4 + 1))
    run("mkfs.ext4", "-q", "-F", MNT + "/t")
    run("mount", "-o", "loop", MNT + "/t", FS)
    files = {}
    for i in range(12):
        d = os.urandom(rng.choice([10_000, 500_000, 3_000_000]))
        open(FS + "/f%d" % i, "wb").write(d); files["f%d" % i] = hashlib.sha256(d).hexdigest()
    for i in range(0, 12, 2): os.unlink(FS + "/f%d" % i); del files["f%d" % i]
    os.sync(); run("fstrim", FS); run("umount", FS); time.sleep(0.3)
    cmd("close t"); stop(p)
    p = start(opts)
    cmd("open-internal t CBC_ESSIV(AES256) " + KEY)
    run("e2fsck", "-fn", MNT + "/t")
    run("mount", "-o", "loop", MNT + "/t", FS)
    for name, h in files.items():
        if hashlib.sha256(open(FS + "/" + name, "rb").read()).hexdigest() != h:
            raise RuntimeError("-m %d -M %d: %s corrupt" % (m, M, name))
    run("umount", FS); time.sleep(0.3)
    info = dict(l.split("=") for l in cmd("info t").split("\n") if "=" in l)
    print("-m %d -M %d (%d KiB / %d KiB, %d blocks): ext4, files, fstrim, restart, "
            "e2fsck OK, mapped mesoblocks %s" % (m, M, 1 << (m - 10), 1 << (M - 10),
            n, info["mapped_mesoblocks"]), flush=True)
    cmd("close t"); stop(p)
os.unlink(base)
print("SIZETEST OK")
