#!/usr/bin/env python3
# fsrepair.py - ext4 on scubed3: corrupt one 4KiB block of a file on disk, reading
# the file gives EIO, rewriting the file in place repairs it, e2fsck stays clean
import os, sys, time, socket, hashlib, subprocess, re
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__))); import s3read
B=os.environ.get("S3BUILD", os.getcwd()); MNT="/mnt/s3"; FS="/mnt/fr"; SOCK="/run/scubed3/control"
KEY=hashlib.pbkdf2_hmac("sha256",b"fr",b"scubed3_prod",1000,32).hex()
base=B+"/fr.base"; open(base,"wb").write(os.urandom(16<<22)); os.makedirs(FS,exist_ok=True)
log=open(B+"/fr.log","w")
def start():
    global P
    subprocess.run(["fusermount","-u","-z",MNT],capture_output=True)
    if os.path.exists(SOCK): os.unlink(SOCK)
    P=subprocess.Popen([B+"/scubed3","-b",base,"-f",MNT],stdout=log,stderr=log)
    while not os.path.exists(SOCK): time.sleep(.05)
    time.sleep(.1)
def stop(): subprocess.run(["fusermount","-u",MNT]); P.wait()
def cmd(l):
    for _ in range(100):
        s=socket.socket(socket.AF_UNIX); s.connect(SOCK); s.sendall((l+"\nexit\n").encode()); b=b""
        while True:
            d=s.recv(65536)
            if not d: break
            b+=d
        if b"busy" in b: time.sleep(.1); continue
        assert b.startswith(b"OK"),(l,b); return b.decode()
def sh(*a):
    for _ in range(50):
        r=subprocess.run(a,capture_output=True,text=True)
        if r.returncode==0: return r.stdout
        if "busy" not in (r.stderr+r.stdout).lower(): raise RuntimeError(r.stderr+r.stdout)
        time.sleep(.2)
    raise RuntimeError("busy")
start(); cmd("create-internal t CBC_ESSIV(AES256) "+KEY); cmd("resize-internal t 10 3")
sh("mkfs.ext4","-q","-F","-b","4096",MNT+"/t"); sh("mount","-o","loop",MNT+"/t",FS)
data=os.urandom(1<<20); open(FS+"/f","wb").write(data); os.sync()
fr=sh("filefrag","-v",FS+"/f"); print(fr.strip().split("\n")[3])
pblk=int(re.search(r"^\s*0:\s+\d+\.\.\s*\d+:\s+(\d+)",fr,re.M).group(1))
target=pblk+5  # 6th 4KiB block of the file
sh("umount",FS); time.sleep(.3); cmd("close t"); stop()
st=s3read.Store(base,b"fr",1000); m=target*4096//16384; part=target%4
blk,k,sq,h=st.loc[m]
with open(base,"r+b") as f:
    f.seek(blk*(1<<22)+(k+1)*16384+part*4096+100); b=f.read(1); f.seek(-1,1); f.write(bytes([b[0]^1]))
print("corrupted fs block",target,"= mesoblock",m)
start(); cmd("open-internal t CBC_ESSIV(AES256) "+KEY); print(cmd("check-data-integrity t").strip().replace("\n"," | "))
sh("mount","-o","loop",MNT+"/t",FS)
r=subprocess.run(["dd","if="+FS+"/f","of=/dev/null","bs=4096","iflag=direct"],capture_output=True,text=True); print("read file:",r.returncode,r.stderr.strip().split("\n")[0])
ok=[r.returncode!=0 and "Input/output error" in r.stderr]
# rewrite the file in place (same blocks)
with open(FS+"/f","r+b") as f: f.write(data); os.fsync(f.fileno())
subprocess.run(["sync"]); 
r=subprocess.run(["dd","if="+FS+"/f","of=/dev/null","bs=4096","iflag=direct"],capture_output=True,text=True); print("after rewrite read:",r.returncode)
ok.append(r.returncode==0)
ok.append(hashlib.sha256(open(FS+"/f","rb").read()).digest()==hashlib.sha256(data).digest()); print("content ok:",ok[-1])
t=cmd("check-data-integrity t"); print(t.strip().replace("\n"," | ")); ok.append("corrupt_mesoblocks=0" in t)
sh("umount",FS); time.sleep(.3)
for _ in range(50):
    r=subprocess.run(["losetup","-f","--show",MNT+"/t"],capture_output=True,text=True)
    if r.returncode==0: break
    time.sleep(.2)
dev=r.stdout.strip(); r=subprocess.run(["e2fsck","-fn",dev],capture_output=True,text=True); print("e2fsck rc",r.returncode); subprocess.run(["losetup","-d",dev]); time.sleep(.3)
ok.append(r.returncode==0)
cmd("close t"); stop()
print("FSREPAIR %s" % ("OK" if all(ok) else "FAILED %s" % ok))
