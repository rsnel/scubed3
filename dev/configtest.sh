#!/bin/sh
# configtest.sh - KDF parameters from the configuration file of scubed3ctl:
# a partition created with the salt from the file is found with the key
# PBKDF2-SHA256(passphrase, salt, iterations), -s overrides the file,
# /etc/scubed3.conf is the default, errors in the file are fatal
#
# usage: configtest.sh DAEMON SCUBED3CTL (run as root, in the build directory)
set -e
D=$1; C=$2; T=$(mktemp -d); MNT=/mnt/s3
fail() { echo "CONFIGTEST FAILED: $*"; exit 1; }
raw() { printf '%s\nexit\n' "$1" | python3 -c "
import socket,sys; s=socket.socket(socket.AF_UNIX); s.connect('/run/scubed3/control'); s.sendall(sys.stdin.read().encode()); b=b''
while True:
    d=s.recv(9999)
    if not d: break
    b+=d
print(b.decode().split(chr(10))[0])"; }
# scubed3ctl reads the passphrase from a terminal
ctl() { (sleep 1; printf 'pw\n'; sleep 1; printf 'pw\n'; sleep 2) | script -qc "$C -q -YYY $1 -c '$2'" /dev/null | tr -d '\r' | grep -v "passphrase:" || true; }
[ -e /etc/scubed3.conf ] && fail "/etc/scubed3.conf exists, move it away first"
printf '# test\n\nkdf = PBKDF2-SHA256\nsalt=  my salt 123  \niterations=1000\n' > $T/s3.conf
head -c $((8<<22)) /dev/urandom > $T/base
fusermount -u -z $MNT 2>/dev/null || true; rm -f /run/scubed3/control
$D -b $T/base -f $MNT > $T/log 2>&1 & sleep 0.5
ctl "-C $T/s3.conf" "create t"; $C -q -YYY -C $T/s3.conf -c "resize t 4"
head -c 1048576 /dev/urandom > $T/data; dd if=$T/data of=$MNT/t bs=1M conv=notrunc 2>/dev/null
raw "close t" > /dev/null
for salt in "my salt 123" "  my salt 123  " "scubed3_prod"; do
	key=$(python3 -c "import hashlib,sys; print(hashlib.pbkdf2_hmac('sha256', b'pw', sys.argv[1].encode(), 1000, 32).hex())" "$salt")
	r=$(raw "open-internal t CBC_ESSIV(AES256) $key")
	if [ "$r" = OK ] && cmp -s -n 1048576 $T/data $MNT/t; then found=yes; else found=no; fi
	raw "close t" > /dev/null
	echo "salt '$salt': data found: $found"
	case "$salt:$found" in "my salt 123:yes"|"  my salt 123  :no"|"scubed3_prod:no") ;; *) fail "salt '$salt'";; esac
done
check_open() { ctl "$1" "open t"; if [ -e $MNT/t ]; then r=yes; raw "close t" > /dev/null; else r=no; fi
	[ $r = $2 ] || fail "$1: found=$r"; echo "scubed3ctl $1: found=$r"; }
check_open "-C $T/s3.conf" yes
check_open "-C $T/s3.conf -s scubed3_prod -i 1000" no
check_open "-s 'my salt 123' -i 1000" yes
cp $T/s3.conf /etc/scubed3.conf; check_open "" yes; rm /etc/scubed3.conf
check_open "" no
err() { printf "$1" > $T/e.conf; out=$($C -C $T/e.conf -c status 2>&1 || true)
	echo "$out" | grep -q "$2" || fail "config '$1': $out"; echo "error: $(echo "$out" | tail -1)"; }
err 'salt=a\nsalt=b\n' "specified twice"
err 'kdf=argon2\n' "unsupported kdf"
err 'iterations=12x\n' "invalid number"
err '# ok\nsalz=x\n' "unknown key"
err 'salt\n' "expected KEY=VALUE"
err 'salt=\n' "empty value"
out=$($C -C $T/none.conf -c status 2>&1 || true); echo "$out" | grep -q "unable to open" || fail "missing -C file"
fusermount -u $MNT; sleep 1; rm -rf $T
echo "CONFIGTEST OK"
