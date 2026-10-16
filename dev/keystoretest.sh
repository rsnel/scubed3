#!/bin/sh
# keystoretest.sh - the key store of scubed3ctl (see Key store in the README)
#
# a key store with two passphrases (A and B) and their views: root (A), hid
# (B), shared (A and B); the partitions are created and opened with the
# keys from the store (no passphrases); every write changes the whole file
# (no 16 byte block stays the same), its size is always the same; A alone
# shows and opens only root and shared; a wrong passphrase has no view; an
# existing partition (with its own passphrase) is imported; LUKS: luksformat
# and luksopen with the key from the store; a passphrase is changed; a write
# with only A unlocked loses the view of B (as the warning says); keys-setup
# (guided) makes a key store with two passphrases, a new, an imported and a
# shared partition; two partitions with the same name (root) in two views:
# the import and unlock-all warn, open and open-all refuse until one of the
# views is locked (lock VIEW), then root opens the partition of the other;
# keys-copy (the same partition under another name), unlock-until (for a
# boot: passphrases from a pipe, one answer each), luksopen-all, a new
# passphrase is never generated (an empty one is no passphrase, two
# different ones do not match), the slot of a new view is random, -k with
# the key of keys-derive, two scubed3ctl at the same time (a write that
# would lose the change of the other is refused)
#
# usage: keystoretest.sh DAEMON CTL (as root; CRYPTSETUP=path: the
# cryptsetup to use, default the stand-in dev/fake-cryptsetup, it needs no
# device mapper)
D=$(realpath $1); C=$(realpath $2); T=$(mktemp -d); MNT=/mnt/s3
DEV=$(cd "$(dirname "$0")" && pwd)
FUSERMOUNT=$(command -v fusermount3 || echo fusermount)
mkdir -p $T/bin
ln -s ${CRYPTSETUP:-$DEV/fake-cryptsetup} $T/bin/cryptsetup
export PATH=$T/bin:$PATH
K=$T/keys
cleanup() {
	cryptsetup close scubed3-root 2> /dev/null
	$FUSERMOUNT -u $T/mnt2 2> /dev/null
	$FUSERMOUNT -u $MNT 2> /dev/null; wait; rm -rf $T
}
fail() { echo "KEYSTORETEST FAILED: $1"; [ -f $T/out ] && cat $T/out; cleanup; exit 1; }
ok() { echo "   ok: $1"; }

printf 'iterations=1000\nsalt=keystoretest\n' > $T/s3.conf
head -c $((400<<16)) /dev/urandom > $T/base
$FUSERMOUNT -u -z $MNT 2> /dev/null; rm -f /run/scubed3/control
$D -b $T/base -m 12 -M 16 -f $MNT > $T/log 2>&1 & sleep 0.5

# scubed3ctl with the commands -c, on a terminal, the answers are typed one
# per second
run() {
	cmds=$1; shift
	(for l in "$@"; do sleep 1; printf '%s\n' "$l"; done; sleep 2) |
		script -qc "$C -C $T/s3.conf -K $K $cmds" /dev/null | tr -d '\r' > $T/out
}
ctl() { $C -q -YYY -C $T/s3.conf -K $K "$@"; }
SIZE=$((64*(12 + 64*128 + 16)))
size() { [ "$(stat -c %s $K)" = $SIZE ] || fail "the key store has size $(stat -c %s $K)"; }
# the number of 16 byte blocks that are the same in two versions
same() { python3 -c "
import sys; a, b = open(sys.argv[1], 'rb').read(), open(sys.argv[2], 'rb').read()
print(sum(a[i:i + 16] == b[i:i + 16] for i in range(0, len(a), 16)))" $1 $2; }

echo "1. a key store with two passphrases"
run "-c keys-create"
size
cp $K $T/k0
run "-c unlock-all -c keys-passphrase-add" "" A A yes
grep -q "view 1 is added" $T/out || fail "passphrase A"
size; [ "$(same $T/k0 $K)" = 0 ] || fail "the key store did not change completely"
cp $K $T/k1
run "-c unlock-all -c keys-passphrase-add" A "" B B yes
grep -q "view 2 is added" $T/out || fail "passphrase B"
size; [ "$(same $T/k1 $K)" = 0 ] || fail "the key store did not change completely"
ok "the file changes completely with every write, it has always $SIZE bytes"

echo "2. partitions with keys from the store"
run "-c unlock-all -c 'keys-add root 1' -c 'keys-add hid 2' -c 'keys-add shared 1' -c 'keys-add shared 2'" \
	A B "" yes yes yes yes
grep -q "copied from view 1 to view 2" $T/out || fail "shared is not copied"
run "-c unlock-all -c 'create root' -c 'create hid' -c 'create shared'" A B ""
grep -qi "passphrase:" $T/out && grep -q "Enter passphrase" $T/out && fail "a passphrase was asked"
for n in root:60 hid:40 shared:30; do ctl -c "resize ${n%:*} ${n#*:}" > /dev/null; done
head -c 100000 /dev/urandom > $T/data
for n in root hid shared; do dd if=$T/data of=$MNT/$n conv=notrunc 2> /dev/null; done
sync; for n in root hid shared; do ctl -c "close $n" > /dev/null; done
ok "created with the keys from the store"

echo "3. one passphrase"
run "-c unlock -c keys" A
sed -n '/^view 1/,$p' $T/out | sed 's/^/      /'
grep -q "^  root \[LUKS\]" $T/out && grep -q "^  shared" $T/out || fail "view of A: $(cat $T/out)"
grep -q "hid" $T/out && fail "A shows hid"
run "-c unlock -c open-all" A
grep -q "2 partitions are opened" $T/out || fail "open-all"
for n in root shared; do cmp -s -n 100000 $T/data $MNT/$n || fail "$n is wrong"; done
[ -e $MNT/hid ] && fail "hid is open"
ok "A shows and opens root and shared, not hid, the data is right"
run "-c unlock" wrong
grep -q "no view for this passphrase" $T/out || fail "a wrong passphrase"
ok "a wrong passphrase has no view"
run "-c unlock-all -c open-all" B ""
cmp -s -n 100000 $T/data $MNT/hid || fail "hid is wrong"
ok "B opens hid"
for n in root hid shared; do ctl -c "close $n" > /dev/null; done

echo "4. import a partition with its own passphrase"
run "-c 'create old'" opw opw
ctl -c "resize old 30" > /dev/null
dd if=$T/data of=$MNT/old conv=notrunc 2> /dev/null; sync; ctl -c "close old" > /dev/null
run "-c unlock-all -c 'keys-import old 1'" A B "" opw yes
run "-c unlock -c 'open old'" A
grep -q "Enter passphrase" $T/out && fail "a passphrase was asked for old"
cmp -s -n 100000 $T/data $MNT/old || fail "old is wrong"
ctl -c "close old" > /dev/null
ok "old is imported, it opens with the key from the store"

echo "5. LUKS with the key from the store"
run "-c unlock -c 'open root' -c 'luksformat root'" A yes yes
cryptsetup isLuks $MNT/root || fail "luksformat: $(cat $T/out)"
run "-c unlock -c 'luksopen root'" A
[ -b /dev/mapper/scubed3-root ] || fail "luksopen: $(cat $T/out)"
cryptsetup close scubed3-root
printf 'not the key' | cryptsetup open --key-file=- $MNT/root scubed3-root 2> /dev/null &&
	fail "another key opens the LUKS volume"
ok "luksformat and luksopen with the key from the store (another key does not open it)"
ctl -c "close root" > /dev/null

echo "6. a passphrase is changed"
run "-c unlock-all -c 'keys-passphrase-change 1'" A B "" A2 A2 yes
run "-c unlock" A
grep -q "no view" $T/out || fail "the old passphrase still has a view"
run "-c unlock -c keys" A2
grep -q "^  root" $T/out || fail "the new passphrase: $(cat $T/out)"
ok "the new passphrase has the view, the old one not"

echo "7. a write without B unlocked"
run "-c unlock -c 'keys-add x 1'" A2 yes
grep -q "views of passphrases that are NOT UNLOCKED NOW ARE LOST" $T/out ||
	fail "no warning: $(cat $T/out)"
run "-c unlock" B
grep -q "no view" $T/out || fail "the view of B survived"
ok "the warning is shown, the view of B is lost (as it says)"

echo "8. keys-setup (guided)"
K=$T/keys2
run "-c keys-setup" y P P y Q Q n s1 "" old y opw "" s1 s2 "" "" yes
grep -q "the key store is written" $T/out || fail "keys-setup: $(cat $T/out)"
size
run "-c unlock -c keys" P
grep -q "^  s1" $T/out && grep -q "^  old" $T/out || fail "view of P: $(cat $T/out)"
grep -q "s2" $T/out && fail "P shows s2"
run "-c unlock -c keys" Q
grep -q "^  s1" $T/out && grep -q "^  s2" $T/out || fail "view of Q: $(cat $T/out)"
run "-c unlock -c 'open old'" P
cmp -s -n 100000 $T/data $MNT/old || fail "old (imported by keys-setup) is wrong"
ctl -K $K -c "close old" > /dev/null
ok "keys-setup: two passphrases, a new, an imported and a shared partition, one write"

echo "9. two partitions with the same name in two views"
K=$T/keys3
for n in r1 r2; do
	run "-c 'create $n'" pw-$n pw-$n
	ctl -c "resize $n 20" > /dev/null
done
head -c 100000 /dev/urandom > $T/data2
dd if=$T/data of=$MNT/r1 conv=notrunc 2> /dev/null
dd if=$T/data2 of=$MNT/r2 conv=notrunc 2> /dev/null
sync; ctl -c "close r1" > /dev/null; ctl -c "close r2" > /dev/null
run "-c keys-create"
run "-c unlock-all -c keys-passphrase-add" "" C C yes
run "-c unlock-all -c keys-passphrase-add" C "" D D yes
run "-c unlock-all -c 'keys-import root 1' -c 'keys-import root 2'" C D "" pw-r1 yes pw-r2 yes
grep -q "NOTE: view 1 has another partition \"root\"" $T/out || fail "no note at the import: $(cat $T/out)"
run "-c unlock-all -c keys" C D ""
grep -q "WARNING: \"root\" is in view 2 and in view 1 with other keys" $T/out &&
	grep -q "^  root (other keys in view 2" $T/out || fail "no warning: $(cat $T/out)"
ok "the import and unlock-all warn, keys marks root in both views"
run "-c unlock-all -c open-all" C D ""
grep -q "nothing is opened" $T/out || fail "open-all: $(cat $T/out)"
run "-c unlock-all -c 'open root'" C D ""
grep -q "lock one of the views first" $T/out || fail "open root: $(cat $T/out)"
grep -q "Enter passphrase" $T/out && fail "a passphrase was asked for the ambiguous root"
[ -e $MNT/root ] && fail "root is opened"
ok "open-all and open root refuse while both views are unlocked, nothing is opened"
run "-c unlock-all -c 'lock 2' -c open-all" C D ""
cmp -s -n 100000 $T/data $MNT/root || fail "view 1: root is not r1"
ctl -c "close root" > /dev/null
run "-c unlock-all -c 'lock 1' -c 'open root'" C D ""
cmp -s -n 100000 $T/data2 $MNT/root || fail "view 2: root is not r2"
ctl -c "close root" > /dev/null
ok "after lock 2 / lock 1, root opens the partition of the view that is left"

echo "10. keys-copy, unlock-until, luksopen-all"
K=$T/keys
run "-c unlock -c 'keys-copy old r00t'" A2 yes
grep -q '"r00t" has the keys of "old" in view 1' $T/out || fail "keys-copy: $(cat $T/out)"
run "-c unlock -c 'keys-copy old r00t'" A2
grep -q '"r00t" is in view 1 already' $T/out || fail "keys-copy to an existing name: $(cat $T/out)"
run "-c unlock -c 'open r00t'" A2
cmp -s -n 100000 $T/data $MNT/r00t || fail "r00t is not the partition old"
ctl -c "close r00t" > /dev/null
ok "keys-copy old r00t: r00t opens the partition old, a second copy is refused"
K=$T/keys2
printf 'wrong\nQ\nP\n' | ctl -c "unlock-until old" -c keys > $T/out 2> /dev/null
printf 'no view for this passphrase\nview 1: 2 entries, without "old"\n"old" is in view 2\n' > $T/expect
head -n 3 $T/out | cmp -s - $T/expect || fail "unlock-until: $(cat $T/out)"
grep -q "^view 1:" $T/out && grep -q "^view 2:" $T/out || fail "the views after unlock-until: $(cat $T/out)"
printf 'Q\n' | ctl -c "unlock-until old" -c keys > $T/out 2> /dev/null && fail "unlock-until without old succeeds"
grep -q '^no view with "old" is unlocked' $T/out && ! grep -q "^view 1: \[slot" $T/out ||
	fail "unlock-until at the end of the input: $(cat $T/out)"
ok "unlock-until: one answer per passphrase, the views stay unlocked; at the end of the input the next command is not done"
K=$T/keys
run "-c unlock -c 'open root' -c 'luksopen-all root'" A2
grep -q "1 LUKS volumes are opened" $T/out && [ -b /dev/mapper/scubed3-root ] ||
	fail "luksopen-all: $(cat $T/out)"
cryptsetup close scubed3-root
ctl -c "close root" > /dev/null
ok "luksopen-all root: the LUKS volume of root with its key from the store"
run "-c 'create lk'" lpw lpw
ctl -c "resize lk 20" > /dev/null
printf 'lukspw' | cryptsetup -q luksFormat --key-file=- $MNT/lk || fail "luksFormat lk"
run "-c unlock -c 'keys-import lk 1'" A2 lpw lukspw yes
grep -q 'the LUKS volume on "lk" has a new key' $T/out || fail "keys-import with LUKS: $(cat $T/out)"
ctl -c "close lk" > /dev/null
run "-c unlock -c 'open lk' -c 'luksopen lk'" A2
[ -b /dev/mapper/scubed3-lk ] || fail "luksopen lk with the imported key: $(cat $T/out)"
cryptsetup close scubed3-lk
ctl -c "close lk" > /dev/null
ok "keys-import of an open partition with LUKS: the LUKS passphrase is asked, the new LUKS key opens it"
cp $K $T/keys.before
run "-c unlock-all -c keys-passphrase-add" A2 "" "" ""
grep -q "^no passphrase" $T/out || fail "an empty new passphrase: $(cat $T/out)"
grep -q -i "autogenerat" $T/out && fail "an autogenerated passphrase: $(cat $T/out)"
run "-c unlock-all -c keys-passphrase-add" A2 "" newpass1 newpass2
grep -q "^passphrases do not match" $T/out || fail "two different passphrases: $(cat $T/out)"
cmp -s $K $T/keys.before || fail "the key store changed"
run "-c unlock -c keys" newpass1
grep -q "no view for this passphrase" $T/out || fail "newpass1 has a view: $(cat $T/out)"
ok "a new passphrase is never generated: an empty one is no passphrase, two different ones do not match (the store does not change)"


echo "11. the slots of the views, -k and keys-derive"
# a new view gets a slot at random (not the first free one): 300 new key
# stores with one view, 64 slots, about 4.7 per slot
: > $T/slots
i=0
while [ $i -lt 300 ]; do
	rm -f $T/k1
	ctl -K $T/k1 -c keys-create > /dev/null
	printf '\nS\nS\n' | ctl -K $T/k1 -c unlock-all -c keys-passphrase-add |
		sed -n 's/.*view 1 is added (empty, in slot \([0-9]*\) of 64, chosen at random)$/\1/p' >> $T/slots
	i=$((i + 1))
done
[ "$(wc -l < $T/slots)" = 300 ] || fail "the slot of a new view is not shown"
n=$(sort -u $T/slots | wc -l); max=$(sort $T/slots | uniq -c | sort -n | tail -n 1 | awk '{ print $1 }')
[ $n -ge 50 ] && [ $max -le 18 ] || fail "the slots are not random: $n slots used, one $max times"
printf 'S\n' | ctl -K $T/k1 -c unlock -c keys > $T/out
grep -q "^view 1: (empty) \[slot $(tail -n 1 $T/slots) of 64\]$" $T/out || fail "keys shows no slot: $(cat $T/out)"
ok "300 new views are in $n of the 64 slots (at most $max in one), keys shows the slot"
key=$(printf 'S\n' | ctl -K $T/k1 -c keys-derive | sed -n 's/.*key: \([0-9a-f]\{64\}\)$/\1/p')
[ ${#key} = 64 ] || fail "keys-derive: $(printf 'S\n' | ctl -K $T/k1 -c keys-derive)"
printf '%s\n' "$key" | ctl -k -K $T/k1 -c unlock -c keys > $T/out
grep -q "view 1: 0 entries" $T/out || fail "-k unlock with the key: $(cat $T/out)"
printf '%s\n' "$key" | tr a-f A-F | ctl -k -K $T/k1 -c unlock > $T/out
grep -q "view 1: 0 entries" $T/out || fail "-k unlock with the key in capitals: $(cat $T/out)"
printf 'S\n' | ctl -k -K $T/k1 -c unlock > $T/out && fail "-k takes a passphrase"
grep -q "not a key" $T/out || fail "-k with a passphrase: $(cat $T/out)"
printf '%s\n' "${key%?}" | ctl -k -K $T/k1 -c unlock > $T/out && fail "-k takes 63 digits"
# a view added with -k (its key from keys-derive) opens with the passphrase
key2=$(printf 'T\n' | ctl -K $T/k1 -c keys-derive | sed -n 's/.*key: \([0-9a-f]\{64\}\)$/\1/p')
printf '%s\n\n%s\n%s\n' "$key" "$key2" "$key2" | ctl -k -K $T/k1 -c unlock-all -c keys-passphrase-add > $T/out
grep -q "view 2 is added" $T/out || fail "-k keys-passphrase-add: $(cat $T/out)"
printf 'T\n' | ctl -K $T/k1 -c unlock > $T/out
grep -q "view 1: 0 entries" $T/out || fail "the passphrase of a view added with -k: $(cat $T/out)"
ok "keys-derive gives the key of a passphrase, -k unlocks and adds views with it (not with a passphrase or a wrong key)"

echo "12. two scubed3ctl at the same time"
# a daemon takes one scubed3ctl at a time (B waits until A is done); with
# two daemons (two base devices, one key store) they run at the same time:
# A reads the key store (unlock-all), waits for its new passphrase; B adds
# a view meanwhile: the write of A would lose it, it is refused
head -c $((40<<16)) /dev/urandom > $T/base2
mkdir -p $T/mnt2
$D -b $T/base2 -m 12 -M 16 -a $T/control2 -f $T/mnt2 > $T/log2 2>&1 &
until [ -S $T/control2 ]; do sleep 0.2; done
{ printf 'S\n\n'; sleep 4; printf 'U\nU\n'; } | ctl -K $T/k1 -c unlock-all -c keys-passphrase-add > $T/outA 2>&1 &
pa=$!
sleep 2
printf 'S\n\nV\nV\n' | ctl -a $T/control2 -K $T/k1 -c unlock-all -c keys-passphrase-add > $T/outB 2>&1
wait $pa
$FUSERMOUNT -u $T/mnt2
grep -q "the key store is written" $T/outB || fail "B: $(cat $T/outB)"
grep -q "is NOT written: another scubed3ctl changed it" $T/outA || fail "A: $(cat $T/outA)"
printf 'V\n' | ctl -K $T/k1 -c unlock > $T/out
grep -q "view 1: 0 entries" $T/out || fail "the view of B is lost: $(cat $T/out)"
printf 'U\n' | ctl -K $T/k1 -c unlock > $T/out
grep -q "no view for this passphrase" $T/out || fail "the view of A is written: $(cat $T/out)"
ls $T | grep -q '\.new' && fail "a temporary file is left: $(ls $T)"
ok "a write after another scubed3ctl wrote the key store is refused (it would lose that change)"
ctl -K $T/k1 -c keys-create > $T/out
grep -q "unable to create the key store" $T/out || fail "keys-create over a key store: $(cat $T/out)"
printf 'V\n' | ctl -K $T/k1 -c unlock | grep -q "view 1: 0 entries" || fail "keys-create changed the key store"
ok "keys-create does not replace an existing key store"

cleanup
echo "KEYSTORETEST OK"
