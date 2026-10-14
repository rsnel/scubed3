# s3read.py - independent offline reader for scubed3 base files (format SSS3v0.5,
# and blocks of SSS3v0.4, without a nonce, as in README/blockio.c)
# usage: python3 s3read.py [-s SALT] BASE PASSPHRASE KDF_ITERATIONS [OUT_IMAGE]
# lists the macroblocks of the partition and optionally writes the reconstructed partition image
# (corrupt 4KiB parts are written as zeroes and reported)
# (default salt "scubed3_prod", PBKDF2-SHA256, CBC_ESSIV(AES256), 4 MiB macroblocks, 16 KiB mesoblocks)
import sys, hashlib, struct
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
MACRO, MESO = 1 << 22, 1 << 14
MMPM = MACRO // MESO - 1
HASHES = 0x100 + 4 * (MMPM + 1)
BITMAP = HASHES + 8 * (MMPM + 1)
TOMBSTONES = 0xFFFFFFFE
PARTS = 4       # the low 4 bits of a stored hash mark corrupt 4KiB parts

def mesohash(ct):
    return struct.unpack('>Q', hashlib.sha256(ct).digest()[:8])[0] & ~0xF

class Store:
    def __init__(self, base, pw, it, salt=b'scubed3_prod'):
        key = hashlib.pbkdf2_hmac('sha256', pw, salt, it, 32)
        self.key = key
        self.ess = Cipher(algorithms.AES(hashlib.sha256(key).digest()), modes.ECB()).encryptor()
        self.f = open(base, 'rb'); self.f.seek(0, 2); nmacro = self.f.tell() // MACRO
        blocks = []
        self.iv0 = {}   # macroblock -> iv0 of its data mesoblocks: seqno^nonce
        for mid in range(nmacro):
            self.f.seek(mid * MACRO); idx = self.dec(self.f.read(MESO), 0, 0, mid)
            if idx[0x70:0x78] not in (b'SSS3v0.5', b'SSS3v0.4'): continue
            if hashlib.sha256(idx[32:]).digest() != idx[:32]: print("bad index hash", mid); continue
            seq = struct.unpack('>Q', idx[0x60:0x68])[0]
            nonce = struct.unpack('>Q', idx[0x20:0x28])[0] if idx[0x70:0x78] == b'SSS3v0.5' else 0
            self.iv0[mid] = seq ^ nonce
            blocks.append((seq, mid, idx))
        blocks.sort()
        # only the most recent block can be torn (interrupted write)
        while blocks:
            seq, mid, idx = blocks[-1]
            if not [k for k in range(MMPM) if
                    mesohash(self.ct(mid, k)) != self.hash(idx, k) & ~0xF]: break
            print(f"macroblock {mid} seqno={seq} is torn, ignored"); blocks.pop()
        seq, mid, last = blocks[-1]
        self.floor = struct.unpack('>Q', last[0x40:0x48])[0]
        self.nmb, self.res, self.pending, ntrims = struct.unpack('>IIII', last[0x78:0x88])
        self.trims = [struct.unpack('>QI', last[0x88 + 12 * i:0x94 + 12 * i]) for i in range(ntrims)]
        self.size = (self.nmb - self.pending - self.res) * MMPM
        self.blocks = blocks
        self.live = [(s, m, i) for s, m, i in blocks if self.used(last, m) and s >= self.floor]
        self.loc = {}   # logical mesoblock -> (macroblock, slot, seqno, stored hash)
        self.tombs = 0
        trimmed = lambda s, L: any(s < ts and L >= tsz for ts, tsz in self.trims)
        for s, m, i in self.live:          # replay in seqno order, later wins
            n = struct.unpack('>I', i[0x100:0x104])[0]
            ind = struct.unpack('>%dI' % n, i[0x104:0x104 + 4 * n])
            # tombstones first: they discard versions in older blocks
            for k, logical in enumerate(ind):
                if logical != TOMBSTONES: continue
                ct = self.ct(m, k)
                if mesohash(ct) != self.hash(i, k) & ~0xF:
                    print(f"corrupt tombstones in block {m} slot {k}"); continue
                pt = self.dec(ct, self.iv0[m], k + 1, m)
                cnt = struct.unpack('>I', pt[:4])[0]
                for L in struct.unpack('>%dI' % cnt, pt[4:4 + 4 * cnt]):
                    if L < self.size and not trimmed(s, L): self.loc.pop(L, None)
                self.tombs += cnt
            for k, logical in enumerate(ind):
                if logical >= self.size or trimmed(s, logical): continue
                self.loc[logical] = (m, k, s, self.hash(i, k))

    def dec(self, buf, iv0, iv1, iv2):
        iv = self.ess.update(struct.pack('>QII', iv0, iv1, iv2))
        return Cipher(algorithms.AES(self.key), modes.CBC(iv)).decryptor().update(buf)
    @staticmethod
    def used(idx, n):  # bit n of the bitmap: 32 bit big endian words, lsb first
        w = struct.unpack('>I', idx[BITMAP + 4 * (n // 32):BITMAP + 4 * (n // 32) + 4])[0]
        return w >> (n % 32) & 1
    @staticmethod
    def hash(idx, k):
        return struct.unpack('>Q', idx[HASHES + 8 * k:HASHES + 8 * k + 8])[0]
    def ct(self, m, k):
        self.f.seek(m * MACRO + (k + 1) * MESO); return self.f.read(MESO)
    def read(self, logical):
        """plaintext of a logical mesoblock and the mask of its corrupt parts"""
        if logical not in self.loc: return b'\0' * MESO, 0
        m, k, s, h = self.loc[logical]
        ct = self.ct(m, k)
        return self.dec(ct, self.iv0[m], k + 1, m), 0xF if mesohash(ct) != h & ~0xF else h & 0xF

if __name__ == "__main__":
    args, salt = sys.argv[1:], b'scubed3_prod'
    if args[:1] == ['-s']: salt, args = args[1].encode(), args[2:]
    st = Store(args[0], args[1].encode(), int(args[2]), salt)
    for s, m, i in st.blocks:
        nb = struct.unpack('>I', i[0x68:0x6C])[0]
        print(f"macroblock {m:3d} seqno={s:4d} next_block={'-' if nb == 0xFFFFFFFF else nb:>4} "
              f"{'' if (s, m, i) in st.live else 'stale'}")
    print(f"no_macroblocks={st.nmb} reserved={st.res} pending_removals={st.pending} "
          f"trims={st.trims} trim_floor={st.floor} logical mesoblocks={st.size}, mapped={len(st.loc)}, "
          f"tombstones={st.tombs}")
    if len(args) > 3:
        out = open(args[3], 'wb'); bad = []
        P = MESO // PARTS
        for logical in range(st.size):
            d, mask = st.read(logical)
            for p in range(PARTS):
                if mask >> p & 1:
                    bad.append(logical * MESO + p * P); d = d[:p * P] + b'\0' * P + d[(p + 1) * P:]
            out.write(d)
        print(f"corrupt parts (zeroed in the image): {len(bad)}" + "".join(f"\ncorrupt={b}+{MESO // PARTS}" for b in bad))
