#!/bin/sh
# entropytest.sh - the random secrets of keystorectl and scubed3ctl have at
# least 256 bits: the keys in the key store (keystorectl add, with
# fake-cryptsetup): the volume key that scubed3ctl luksformat gives
# cryptsetup has 64 bytes (512 bits), the key of a partition 32 bytes (256
# bits), from getrandom(2), they differ between partitions (the passphrases
# of the key store are typed, never generated; a random keyslot key has 64
# bytes)
#
# usage: entropytest.sh DAEMON CTL (as root; keystorectl is next to CTL)
D=$(realpath $1); C=$(realpath $2); T=$(mktemp -d); MNT=/mnt/s3
KS=$(dirname $C)/keystorectl
DEV=$(cd "$(dirname "$0")" && pwd); SRC=$DEV/../src
FUSERMOUNT=$(command -v fusermount3 || echo fusermount)
cleanup() { $FUSERMOUNT -u $MNT 2> /dev/null; wait; rm -rf $T; }
fail() { echo "ENTROPYTEST FAILED: $1"; cleanup; exit 1; }
ok() { echo "   ok: $1"; }

echo "the keys in the key store"
mkdir -p $T/bin
# cryptsetup: the volume key that luksformat gives it (--volume-key-file)
cat > $T/bin/cryptsetup << EOF
#!/bin/sh
case " \$* " in
*" luksFormat "*)
	v=; p=
	for a in "\$@"; do [ "\$p" = --volume-key-file ] && v=\$a; p=\$a; done
	[ -n "\$v" ] && cat "\$v" > $T/lukskey.\$\$
	exec $DEV/fake-cryptsetup "\$@";;
*)	exec $DEV/fake-cryptsetup "\$@";;
esac
EOF
chmod +x $T/bin/cryptsetup
export PATH=$T/bin:$PATH
printf 'iterations=1000\n' > $T/s3.conf
head -c $((100<<16)) /dev/urandom > $T/base
$FUSERMOUNT -u -z $MNT 2> /dev/null
mkdir -p $MNT
$D -b $T/base -m 12 -M 16 -f $MNT > $T/log 2>&1 & sleep 0.5
ctl() { $C -q -YYY -C $T/s3.conf -K $T/keys "$@"; }
ks() { $KS -q -YYY -C $T/s3.conf -K $T/keys "$@"; }
ks -c create > /dev/null
printf '\nA\nA\n' | ks -c unlock-all -c passphrase-add -c save > /dev/null
printf 'A\n\n' | ks -c unlock-all -c "add p1 1" -c "add p2 1" -c save > $T/out 2>&1
printf 'A\n\n\n\n\n' | ctl -c unlock -c "create p1" -c "create p2" -c "resize p1 10" \
	-c "resize p2 10" -c "luksformat p1" -c "luksformat p2" >> $T/out 2>&1
set -- $T/lukskey.*
[ $# = 2 ] || fail "luksformat: $(cat $T/out)"
[ "$(wc -c < $1)" = 64 ] && [ "$(wc -c < $2)" = 64 ] || fail "a volume key has not 64 bytes"
cmp -s $1 $2 && fail "two LUKS volumes have the same volume key"
ok "the volume keys of the LUKS volumes (in the key store) have 64 bytes (512 bits), they differ"
grep -q 'define KS_KEY		32' $SRC/keystore.h && grep -q 'ks_random(e->key, KS_KEY);' $SRC/keystorectl.c &&
	grep -q 'ks_random(e->luks, KS_LUKS);' $SRC/keystorectl.c &&
	grep -q 'len = KS_LUKS;' $SRC/scubed3ctl.c && grep -q 'ks_random(pw, len);' $SRC/scubed3ctl.c ||
	fail "the keys are not from ks_random"
# two partitions with keys from the store open as two partitions (the keys
# differ)
ctl -c p | grep -q " blocks in p1" && ctl -c p | grep -q " blocks in p2" || fail "p1 and p2: $(ctl -c p)"
ok "the keys of the partitions have 32 bytes (256 bits, KS_KEY = KEY_LENGTH), from getrandom"
for n in p1 p2; do ctl -c "close $n" > /dev/null; done
cleanup
echo "ENTROPYTEST OK"
