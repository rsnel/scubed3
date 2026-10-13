#!/usr/bin/env python3
# killtest.py - ext4 on scubed3: write files, sync, write more, kill -9 the
# daemon; after restart all files written before the sync must be intact and
# e2fsck must find the filesystem clean
#
# usage: killtest.py DAEMON ROUNDS SEED
# (DISCARD=1: mount with -o discard and run fstrim before the kill)
import os, sys, time, socket, random, hashlib, subprocess, signal

B = os.environ.get("S3BUILD", os.getcwd())
MNT, FS, SOCK = "/mnt/s3", "/mnt/kt", "/run/scubed3/control"
KEY = hashlib.pbkdf2_hmac("sha256", b"kpw", b"scubed3_prod", 1000, 32).hex()
daemon, rounds, seed = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
rng = random.Random(seed)
base = B + "/killtest.base"
log = open(B + "/killtest.log", "w")
P = None
synced = {}     # name -> sha256 of files that were synced

def run(*a, ok=(0,)):
    r = subprocess.run(a, capture_output=True, text=True)
    if r.returncode not in ok:
        raise RuntimeError("%s: rc=%d %s %s" % (a, r.returncode, r.stdout, r.stderr))
    return r

def start():
    global P
    run("fusermount", "-u", "-z", MNT, ok=(0, 1))
    if os.path.exists(SOCK): os.unlink(SOCK)
    P = subprocess.Popen([B + "/" + daemon, "-b", base, "-f", MNT], stdout=log, stderr=log)
    while not os.path.exists(SOCK): time.sleep(0.05)
    time.sleep(0.1)

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

def mount():
    for _ in range(50):
        opts = "loop,discard" if os.environ.get("DISCARD") else "loop"
        r = subprocess.run(["mount", "-o", opts, MNT + "/t", FS], capture_output=True, text=True)
        if r.returncode == 0: return
        if "busy" not in r.stderr.lower() and "loop" not in r.stderr.lower():
            raise RuntimeError("mount: " + r.stderr)
        time.sleep(0.2)
    raise RuntimeError("mount: busy")

def fsck():
    for _ in range(50):
        r = subprocess.run(["losetup", "-f", "--show", MNT + "/t"], capture_output=True, text=True)
        if r.returncode == 0: break
        time.sleep(0.2)
    dev = r.stdout.strip()
    try:
        r = subprocess.run(["e2fsck", "-fn", dev], capture_output=True, text=True)
        if r.returncode != 0: raise RuntimeError("e2fsck: " + r.stdout + r.stderr)
    finally:
        subprocess.run(["losetup", "-d", dev])
        time.sleep(0.3)

def write_files(n):
    out = {}
    for _ in range(n):
        data = os.urandom(rng.choice([4096, 50_000, 300_000, 1_500_000]))
        name = "f%06d" % rng.randrange(10**6)
        with open(FS + "/" + name, "wb") as f: f.write(data)
        out[name] = hashlib.sha256(data).hexdigest()
    return out

os.makedirs(FS, exist_ok=True)
with open(base, "wb") as f: f.write(os.urandom(40 << 22))
start()
cmd("create-internal t CBC_ESSIV(AES256) " + KEY)
cmd("resize-internal t 24 6")
for _ in range(50):
    r = subprocess.run(["mkfs.ext4", "-q", "-F", MNT + "/t"], capture_output=True, text=True)
    if r.returncode == 0: break
    time.sleep(0.2)
else: raise RuntimeError("mkfs: " + r.stderr)
cmd("close t")

lost = ok_rounds = 0
for rnd in range(rounds):
    cmd("open-internal t CBC_ESSIV(AES256) " + KEY)
    mount()
    # verify everything that was synced before
    for name, h in synced.items():
        p = FS + "/" + name
        if not os.path.exists(p):
            lost += 1; print("round %d: synced file %s LOST" % (rnd, name), flush=True); continue
        if hashlib.sha256(open(p, "rb").read()).hexdigest() != h:
            lost += 1; print("round %d: synced file %s CORRUPT" % (rnd, name), flush=True)
    # remove files that were not synced (they may or may not exist)
    for name in os.listdir(FS):
        if name not in synced and name != "lost+found": os.unlink(FS + "/" + name)
    # keep the filesystem from filling up
    for name in rng.sample(sorted(synced), max(0, len(synced) - 25)):
        os.unlink(FS + "/" + name); del synced[name]
    new = write_files(rng.randint(2, 8))
    if os.environ.get("DISCARD"): subprocess.run(["fstrim", FS])
    os.sync()
    synced.update(new)
    write_files(rng.randint(0, 4))      # not synced
    time.sleep(rng.random() * 0.5)
    P.send_signal(signal.SIGKILL); P.wait()
    subprocess.run(["umount", "-l", FS])
    subprocess.run(["losetup", "-D"])
    start()
    if lost == 0: ok_rounds += 1
    # check the filesystem without mounting first (journal not replayed yet:
    # e2fsck -n would complain about the journal), so mount, umount, fsck
    cmd("open-internal t CBC_ESSIV(AES256) " + KEY)
    mount(); subprocess.run(["umount", FS]); time.sleep(0.3)
    fsck()
    cmd("close t")
    print("round %d: %d synced files, kill -9 after sync, fsck clean, lost so far: %d"
            % (rnd, len(synced), lost), flush=True)

print("KILLTEST %s: %d rounds, %d synced files lost or corrupt" %
        ("OK" if not lost else "FAILED", rounds, lost), flush=True)
cmd("open-internal t CBC_ESSIV(AES256) " + KEY); cmd("close t")
run("fusermount", "-u", MNT)
