#!/usr/bin/env python3
# truncatetest.py - truncate behaves as on a block device: dd without
# conv=notrunc, "> partition", truncate(1) and ftruncate() succeed, but
# neither the size nor the contents change
#
# usage: truncatetest.py DAEMON
import os, sys, time, socket, hashlib, subprocess

B = os.environ.get("S3BUILD", os.getcwd())
MNT, SOCK = "/mnt/s3", "/run/scubed3/control"
KEY = hashlib.pbkdf2_hmac("sha256", b"tpw", b"scubed3_prod", 1000, 32).hex()
base, T = B + "/trunc.base", MNT + "/t"
log = open(B + "/trunc.log", "w")

def ok(line):
    s = socket.socket(socket.AF_UNIX); s.connect(SOCK)
    s.sendall((line + "\nexit\n").encode()); buf = b""
    while True:
        d = s.recv(65536)
        if not d: break
        buf += d
    if not buf.startswith(b"OK"): raise RuntimeError("%s: %r" % (line, buf))

def run(*a, shell=False):
    r = subprocess.run(a[0] if shell else a, capture_output=True, text=True, shell=shell)
    if r.returncode: raise RuntimeError("%s: %s %s" % (a, r.stdout, r.stderr))

def check(what):
    assert os.stat(T).st_size == S, (what, os.stat(T).st_size)
    assert open(T, "rb").read() == bytes(img), what
    print("%s: OK" % what, flush=True)

with open(base, "wb") as f: f.write(os.urandom(12 << 22))
subprocess.run(["fusermount", "-u", "-z", MNT], capture_output=True)
if os.path.exists(SOCK): os.unlink(SOCK)
P = subprocess.Popen([B + "/" + sys.argv[1], "-b", base, "-f", MNT], stdout=log, stderr=log)
while not os.path.exists(SOCK): time.sleep(0.05)
time.sleep(0.1)
ok("create-internal t CBC_ESSIV(AES256) " + KEY); ok("resize-internal t 8 2")
S = os.stat(T).st_size
img = bytearray(os.urandom(S)); open(B + "/trunc.img", "wb").write(img)

run("dd", "if=" + B + "/trunc.img", "of=" + T, "bs=1M")
check("dd without conv=notrunc")
patch = os.urandom(65536); img[10 * 65536:11 * 65536] = patch
open(B + "/trunc.patch", "wb").write(patch)
run("dd", "if=" + B + "/trunc.patch", "of=" + T, "bs=65536", "seek=10")
check("dd seek=10 without conv=notrunc")
run(": > " + T, shell=True); check("> partition")
run("truncate", "-s", "0", T); run("truncate", "-s", "+1M", T); check("truncate -s 0 / +1M")
with open(T, "r+b") as f: f.truncate(12345)
check("ftruncate")
ok("close t"); run("fusermount", "-u", MNT); P.wait(timeout=30)
os.unlink(B + "/trunc.img"); os.unlink(B + "/trunc.patch")
print("TRUNCATETEST OK", flush=True)
