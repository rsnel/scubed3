#!/usr/bin/env python3
# s3diff.py - an adversary with ONE passphrase and two snapshots of the base
# file: which macroblocks changed, and do they all belong to the partition of
# that passphrase? (in paranoia level 1, changed blocks that are not ours
# show that another partition was written; in level 2 the free space is
# overwritten too, so this tells nothing)
# usage: s3diff.py [-s SALT] BEFORE AFTER PASSPHRASE KDF_ITERATIONS
# (4 MiB macroblocks, 16 KiB mesoblocks, PBKDF2-SHA256, format SSS3v0.5 or v0.4)
import sys, hashlib, struct
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
MACRO, MESO = 1 << 22, 1 << 14
args, salt = sys.argv[1:], b'scubed3_prod'
if args[:1] == ['-s']: salt, args = args[1].encode(), args[2:]
a, b, pw, it = args[0], args[1], args[2].encode(), int(args[3])
key = hashlib.pbkdf2_hmac('sha256', pw, salt, it, 32)
ess = Cipher(algorithms.AES(hashlib.sha256(key).digest()), modes.ECB()).encryptor()
def owned(f, mid):
    f.seek(mid * MACRO); c = f.read(MESO)
    iv = ess.update(struct.pack('>QII', 0, 0, mid))
    p = Cipher(algorithms.AES(key), modes.CBC(iv)).decryptor().update(c)
    return p[0x70:0x78] in (b'SSS3v0.5', b'SSS3v0.4') and hashlib.sha256(p[32:]).digest() == p[:32]
fa, fb = open(a, 'rb'), open(b, 'rb'); fa.seek(0, 2); n = fa.tell() // MACRO
mine = other = 0
for mid in range(n):
    fa.seek(mid * MACRO); fb.seek(mid * MACRO)
    if fa.read(MACRO) != fb.read(MACRO):
        if owned(fa, mid) or owned(fb, mid): mine += 1
        else: other += 1; print(f"macroblock {mid} changed, but it is not ours (before or after)")
print(f"changed macroblocks: {mine} belong to the opened partition, {other} do NOT -> " +
      ("another partition exists" if other else "nothing to see"))
