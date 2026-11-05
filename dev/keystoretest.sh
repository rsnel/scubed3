#!/bin/sh
# keystoretest.sh - the key store, keystorectl and scubed3ctl (see Key store
# in the README)
#
# a key store with two passphrases (A and B) and their views: root (A), hid
# (B), shared (A and B); keystorectl changes the key store in memory, save
# writes it (without save: nothing is written, -c fails); scubed3ctl reads
# it, one view at a time (unlock locks the view that is unlocked), the
# partitions are created and opened with the keys from the store (no
# passphrases); every write changes the whole file (no 16 byte block stays
# the same), its size is always the same; A shows and opens only root and
# shared; a wrong passphrase has no view; an existing partition (with its
# own passphrase) is imported; LUKS: luksformat and luksopen with the key
# from the store; a passphrase is changed; a save with only A unlocked
# loses the view of B (as the warning says); setup (guided) makes a key
# store with two passphrases, a new, an imported and a shared partition;
# two partitions with the same name (root) in two views: keystorectl warns
# (lock VIEW), scubed3ctl opens the root of the view it unlocked; copy (the
# same partition under another name), unlock-until (for a boot:
# passphrases from a pipe, one answer each, the partitions of the other
# views are opened), luksopen-all, a new passphrase is never generated (an
# empty one is no passphrase, two different ones do not match), the slot of
# a new view is random, -k with the key of derive, two keystorectl at the
# same time (a save that would lose the change of the other is refused),
# scubed3ctl never writes the key store
#
# usage: keystoretest.sh DAEMON CTL (as root; keystorectl is next to CTL;
# CRYPTSETUP=path: the cryptsetup to use, default the stand-in
# dev/fake-cryptsetup, it needs no device mapper; with the real one, for
# example CRYPTSETUP=$(command -v cryptsetup), the device mapper and
# dm-crypt are used; the partitions with LUKS on them (LUKS macroblocks,
# 25 MB) are larger than a LUKS2 header with its default size, 16 MiB:
# cryptsetup refuses a smaller header on a small device)
D=$(realpath $1); C=$(realpath $2); T=$(mktemp -d); MNT=/mnt/s3
KS=$(dirname $C)/keystorectl
DEV=$(cd "$(dirname "$0")" && pwd)
FUSERMOUNT=$(command -v fusermount3 || echo fusermount)
mkdir -p $T/bin
ln -s ${CRYPTSETUP:-$DEV/fake-cryptsetup} $T/bin/cryptsetup
export PATH=$T/bin:$PATH
K=$T/keys
cleanup() {
	for n in scubed3-root scubed3-lk scubed3-p3 lp3; do
		cryptsetup close $n 2> /dev/null
	done
	$FUSERMOUNT -u $T/mnt2 2> /dev/null
	$FUSERMOUNT -u $MNT 2> /dev/null; wait; rm -rf $T
}
fail() { echo "KEYSTORETEST FAILED: $1"; [ -f $T/out ] && cat $T/out; cleanup; exit 1; }
ok() { echo "   ok: $1"; }

printf 'iterations=1000\nsalt=keystoretest\n' > $T/s3.conf
LUKS=550
head -c $((2200<<16)) /dev/urandom > $T/base
$FUSERMOUNT -u -z $MNT 2> /dev/null; rm -f /run/scubed3/control
$D -b $T/base -m 12 -M 16 -f $MNT > $T/log 2>&1 & sleep 0.5

# scubed3ctl (run) and keystorectl (krun) with the commands -c, on a
# terminal, the answers are typed one per second
term() {
	prog=$1; cmds=$2; shift 2
	(for l in "$@"; do sleep 1; printf '%s\n' "$l"; done; sleep 2) |
		script -qc "$prog -C $T/s3.conf -K $K $cmds" /dev/null | tr -d '\r' > $T/out
}
run() { term $C "$@"; }
krun() { term $KS "$@"; }
ctl() { $C -q -YYY -C $T/s3.conf -K $K "$@"; }
ks() { $KS -q -YYY -C $T/s3.conf -K $K "$@"; }
SIZE=$((64*(12 + 64*128 + 16)))
size() { [ "$(stat -c %s $K)" = $SIZE ] || fail "the key store has size $(stat -c %s $K)"; }
# the number of 16 byte blocks that are the same in two versions
same() { python3 -c "
import sys; a, b = open(sys.argv[1], 'rb').read(), open(sys.argv[2], 'rb').read()
print(sum(a[i:i + 16] == b[i:i + 16] for i in range(0, len(a), 16)))" $1 $2; }

echo "1. a key store with two passphrases"
krun "-c create"
size
cp $K $T/k0
krun "-c unlock-all -c passphrase-add -c save" "" A A yes
grep -q "view i[0-9][0-9] is added" $T/out || fail "passphrase A"
size; [ "$(same $T/k0 $K)" = 0 ] || fail "the key store did not change completely"
cp $K $T/k1
krun "-c unlock-all -c passphrase-add -c save" A "" B B yes
grep -q "view i[0-9][0-9] is added" $T/out || fail "passphrase B"
size; [ "$(same $T/k1 $K)" = 0 ] || fail "the key store did not change completely"
ok "the file changes completely with every write, it has always $SIZE bytes"

echo "2. partitions with keys from the store"
cp $K $T/k2
krun "-c unlock-all -c 'add root 1' -c 'add hid 2' -c 'add shared 1' -c 'add shared 2'" A B "" n
grep -q "the changes are NOT saved" $T/out && cmp -s $K $T/k2 || fail "written without save: $(cat $T/out)"
printf 'A\nB\n\n' | ks -c unlock-all -c "add root 1" > $T/out 2>&1 && fail "-c without save succeeds"
cmp -s $K $T/k2 || fail "written without save (-c)"
ok "without save the key store is not written (at the prompt: a question, with -c: an error)"
krun "-c unlock-all -c 'add root 1' -c 'add hid 2' -c 'add shared 1' -c 'add shared 2' -c save" \
	A B "" yes
grep -q "copied from view i[0-9][0-9] to view i[0-9][0-9]" $T/out || fail "shared is not copied"
grep -q "the key store is written" $T/out || fail "save: $(cat $T/out)"
run "-c unlock -c 'create root' -c 'create shared'" A
grep -q "Enter passphrase" $T/out && fail "a passphrase was asked"
run "-c unlock -c 'create hid'" B
grep -q "Enter passphrase" $T/out && fail "a passphrase was asked"
for n in root:$LUKS hid:40 shared:30; do ctl -c "resize ${n%:*} ${n#*:}" > /dev/null; done
head -c 100000 /dev/urandom > $T/data
for n in root hid shared; do dd if=$T/data of=$MNT/$n conv=notrunc 2> /dev/null; done
sync; for n in root hid shared; do ctl -c "close $n" > /dev/null; done
ok "created with the keys from the store"

echo "3. one passphrase"
run "-c unlock -c keys" A
sed -n '/^view /,$p' $T/out | sed 's/^/      /'
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
run "-c unlock -c open-all" B
cmp -s -n 100000 $T/data $MNT/hid || fail "hid is wrong"
ok "B opens hid"
for n in root hid shared; do ctl -c "close $n" > /dev/null; done
run "-c unlock -c unlock -c keys -c open-all" A B
grep -q "^view i[0-9][0-9] is locked$" $T/out && grep -q "^  hid" $T/out && ! grep -q "^  root" $T/out ||
	fail "unlock after unlock: $(cat $T/out)"
[ -e $MNT/hid ] && [ -e $MNT/shared ] && [ ! -e $MNT/root ] || fail "open-all of the second view"
for n in hid shared; do ctl -c "close $n" > /dev/null; done
ok "scubed3ctl has one view at a time: unlock locks the view of A, B opens hid and shared"

echo "4. import a partition with its own passphrase"
# the daemon only knows the blocks of OPEN partitions: create and resize
# take blocks that are not claimed, they can be blocks of a partition that
# is not open (in level 1 too; see the README: open ALL partitions first);
# root is used later, it is open when partitions are made (here, 9, 10)
printf 'A\n' | ctl -c unlock -c "open root" > /dev/null
run "-c 'create old'" opw opw
ctl -c "resize old 30" > /dev/null
dd if=$T/data of=$MNT/old conv=notrunc 2> /dev/null; sync; ctl -c "close old" > /dev/null
krun "-c unlock-all -c 'import old 1' -c save" A B "" opw yes
run "-c unlock -c 'open old'" A
grep -q "Enter passphrase" $T/out && fail "a passphrase was asked for old"
cmp -s -n 100000 $T/data $MNT/old || fail "old is wrong"
ctl -c "close old" > /dev/null
ok "old is imported, it opens with the key from the store"

echo "5. LUKS with the key from the store"
run "-c unlock -c 'open root' -c 'luksformat root'" A yes yes "" ""
cryptsetup isLuks $MNT/root || fail "luksformat: $(cat $T/out)"
run "-c unlock -c 'luksopen root'" A
[ -b /dev/mapper/scubed3-root ] || fail "luksopen: $(cat $T/out)"
cryptsetup close scubed3-root
printf 'not the key' | cryptsetup open --key-file=- $MNT/root scubed3-root 2> /dev/null &&
	fail "another key opens the LUKS volume"
ok "luksformat and luksopen with the key from the store (another key does not open it)"
ctl -c "close root" > /dev/null

echo "6. a passphrase is changed"
krun "-c unlock-all -c 'passphrase-change 1' -c save" A B "" A2 A2 yes
run "-c unlock" A
grep -q "no view" $T/out || fail "the old passphrase still has a view"
run "-c unlock -c keys" A2
grep -q "^  root" $T/out || fail "the new passphrase: $(cat $T/out)"
ok "the new passphrase has the view, the old one not"

echo "7. a save without B unlocked"
krun "-c unlock -c 'add x 1' -c save" A2 yes
grep -q "views of passphrases that are NOT UNLOCKED NOW ARE LOST" $T/out ||
	fail "no warning: $(cat $T/out)"
run "-c unlock" B
grep -q "no view" $T/out || fail "the view of B survived"
ok "the warning is shown, the view of B is lost (as it says)"

echo "8. setup (guided)"
K=$T/keys2
krun "-c setup" y P P y Q Q n s1 "" old y opw "" s1 s2 "" "" yes
cp $T/out $T/setup.out
grep -q "the key store is written" $T/out || fail "setup: $(cat $T/out)"
size
run "-c unlock -c keys" P
grep -q "^  s1" $T/out && grep -q "^  old" $T/out || fail "view of P: $(cat $T/out)"
grep -q "s2" $T/out && fail "P shows s2"
run "-c unlock -c keys" Q
grep -q "^  s1" $T/out && grep -q "^  s2" $T/out || fail "view of Q: $(cat $T/out)"
# old: what the daemon says, does it open with its own passphrase (the key
# in the key store, or the data?), what setup did
old_diag() {
	echo "--- open old with the key store:"; cat $T/out
	echo "--- info old:"; ctl -c "info old" 2>&1
	echo "--- p:"; ctl -c p 2>&1
	ctl -c "close old" > /dev/null 2>&1
	printf 'opw\n' | ctl -K $T/nokeys -c "open old" 2>&1
	if cmp -s -n 100000 $T/data $MNT/old; then
		echo "--- with its passphrase old opens, its data is right"
	else echo "--- with its passphrase old does not open, or its data is wrong"
	fi
	echo "--- setup:"; cat $T/setup.out
	echo "--- the daemon:"; tail -n 20 $T/log
	rm -f $T/out
}
printf 'P\n' | ctl -c unlock -c "open old" > $T/out 2>&1
cmp -s -n 100000 $T/data $MNT/old || fail "old (imported by setup) is wrong
$(old_diag)"
ctl -K $K -c "close old" > /dev/null
ok "setup: two passphrases, a new, an imported and a shared partition, one write"

echo "9. two partitions with the same name in two views"
K=$T/keys3
# root and old are used later: open while r1 and r2 are made (then closed:
# the partition r1 is opened as root below)
printf 'A2\n' | ctl -K $T/keys -c unlock -c "open root" > /dev/null
printf 'opw\n' | ctl -K $T/nokeys -c "open old" > /dev/null
for n in r1 r2; do
	run "-c 'create $n'" pw-$n pw-$n
	ctl -c "resize $n 20" > /dev/null
done
ctl -c "close root" > /dev/null; ctl -c "close old" > /dev/null
head -c 100000 /dev/urandom > $T/data2
dd if=$T/data of=$MNT/r1 conv=notrunc 2> /dev/null
dd if=$T/data2 of=$MNT/r2 conv=notrunc 2> /dev/null
sync; ctl -c "close r1" > /dev/null; ctl -c "close r2" > /dev/null
krun "-c create"
krun "-c unlock-all -c passphrase-add -c save" "" C C yes
krun "-c unlock-all -c passphrase-add -c save" C "" D D yes
krun "-c unlock-all -c 'import root 1' -c 'import root 2' -c save" C D "" pw-r1 pw-r2 yes
grep -q "NOTE: view i[0-9][0-9] has another partition \"root\"" $T/out || fail "no note at the import: $(cat $T/out)"
krun "-c unlock-all -c keys" C D ""
grep -q "WARNING: \"root\" is in view i[0-9][0-9] and in view i[0-9][0-9] with other keys" $T/out &&
	grep -q "^  root .*(other keys in view i[0-9][0-9]" $T/out || fail "no warning: $(cat $T/out)"
krun "-c unlock-all -c 'lock 2' -c keys" C D ""
grep -q "view i[0-9][0-9] is locked" $T/out && ! grep -q "^  root .*other keys" $T/out || fail "lock 2: $(cat $T/out)"
ok "keystorectl: the import and unlock-all warn, keys marks root in both views; after lock 2 not"
run "-c unlock -c open-all" C
cmp -s -n 100000 $T/data $MNT/root || fail "view of C: root is not r1"
ctl -c "close root" > /dev/null
run "-c unlock -c unlock -c 'open root'" C D
grep -q "Enter passphrase" $T/out && fail "a passphrase was asked for root"
cmp -s -n 100000 $T/data2 $MNT/root || fail "view of D: root is not r2"
ctl -c "close root" > /dev/null
ok "scubed3ctl: root opens the partition of the view that is unlocked (one at a time, no clash)"

echo "10. copy, unlock-until, luksopen-all"
K=$T/keys
krun "-c unlock -c 'copy old r00t' -c save" A2 yes
grep -q '"r00t" has the keys of "old" in view i[0-9][0-9]' $T/out || fail "copy: $(cat $T/out)"
krun "-c unlock -c 'copy old r00t'" A2
grep -q '"r00t" is in view i[0-9][0-9] already' $T/out || fail "copy to an existing name: $(cat $T/out)"
run "-c unlock -c 'open r00t'" A2
cmp -s -n 100000 $T/data $MNT/r00t || fail "r00t is not the partition old"
ctl -c "close r00t" > /dev/null
ok "copy old r00t: r00t opens the partition old, a second copy is refused"
K=$T/keys2
# s1 and s2 (the view of Q): unlock-until opens them; root (used below) is
# open while they are made
printf 'A2\n' | ctl -K $T/keys -c unlock -c "open root" > /dev/null
printf 'Q\n' | ctl -c unlock -c "create s1" -c "resize s1 10" -c "create s2" -c "resize s2 10" > /dev/null
for n in s1 s2; do ctl -c "close $n" > /dev/null; done
printf 'wrong\nQ\nP\n' | ctl -c "unlock-until old" -c keys > $T/out 2> /dev/null
[ "$(sed -n 1p $T/out)" = "no view for this passphrase" ] &&
	sed -n 2p $T/out | grep -qx 'view i[0-9][0-9]: 2 entries, without "old"' &&
	grep -q '^2 partitions are opened$' $T/out &&
	grep -qx '"old" is in view i[0-9][0-9]' $T/out || fail "unlock-until: $(cat $T/out)"
[ "$(grep -c "^view i[0-9][0-9]:$" $T/out)" = 1 ] && grep -q "^  old" $T/out && ! grep -q "^  s2" $T/out ||
	fail "the view after unlock-until: $(cat $T/out)"
[ -e $MNT/s1 ] && [ -e $MNT/s2 ] || fail "s1 and s2 are not opened by unlock-until"
printf 'Q\n' | ctl -c "unlock-until old" -c keys > $T/out 2> /dev/null && fail "unlock-until without old succeeds"
grep -q '^no view with "old" is unlocked' $T/out && ! grep -q "^view i[0-9][0-9]:$" $T/out ||
	fail "unlock-until at the end of the input: $(cat $T/out)"
for n in s1 s2; do ctl -c "close $n" > /dev/null; done
ok "unlock-until: one answer per passphrase, the partitions of the other views are opened, one view stays unlocked; at the end of the input the next command is not done"
K=$T/keys
run "-c unlock -c 'open root' -c 'luksopen-all root'" A2
grep -q "1 LUKS volumes are opened" $T/out && [ -b /dev/mapper/scubed3-root ] ||
	fail "luksopen-all: $(cat $T/out)"
ok "luksopen-all root: the LUKS volume of root with its key from the store"
# luksstatus: root has LUKS (its key in view 1), a loop device, it is open
# as /dev/mapper/scubed3-root; plain (no LUKS) on a loop device with a
# file system, mounted; a partition that is not open
printf 'plainpw\nplainpw\n' | ctl -c "create plain" -c "resize plain 30" > /dev/null
[ -e $MNT/plain ] || fail "create plain"
run "-c unlock -c 'luksstatus root'" A2
grep -q "^root: LUKS2 header, its LUKS volume key is in view i[0-9][0-9] of the key store, /dev/loop[0-9]* open as /dev/mapper/scubed3-root$" $T/out ||
	fail "luksstatus root: $(cat $T/out)"
l=$(losetup -f --show $MNT/plain) || fail "losetup plain"
mke2fs -q -F $l > $T/mkfs.out 2>&1 || fail "mke2fs on plain: $(cat $T/mkfs.out)"
mkdir -p $T/m; mount $l $T/m || fail "mount plain"
ctl -c luksstatus > $T/out
umount $T/m; losetup -d $l
grep -q "^plain: no LUKS header, $l mounted on $T/m$" $T/out || fail "luksstatus plain: $(cat $T/out)"
grep -q "^root: LUKS2 header, /dev/loop" $T/out || fail "luksstatus without the key store: $(cat $T/out)"
ctl -c "luksstatus hid" > $T/out && fail "luksstatus of a partition that is not open succeeds"
grep -q "^hid: not open$" $T/out || fail "luksstatus hid: $(cat $T/out)"
ctl -c "close plain" > /dev/null
ok "luksstatus: the LUKS header, the key in the key store, the loop device, /dev/mapper, the mount point"
cryptsetup close scubed3-root
ctl -c "close root" > /dev/null
run "-c 'create lk'" lpw lpw
ctl -c "resize lk $LUKS" > /dev/null
printf 'lukspw' | cryptsetup -q luksFormat --key-file=- $MNT/lk || fail "luksFormat lk"
krun "-c unlock -c 'import lk 1' -c save" A2 lpw wrongpw yes
grep -q 'the volume key of the LUKS volume on "lk" is not in the key store' $T/out || fail "import with a wrong LUKS passphrase: $(cat $T/out)"
krun "-c unlock -c 'remove lk 1' -c save" A2 yes
krun "-c unlock -c 'import lk 1' -c save" A2 lpw lukspw yes
grep -q 'the volume key of the LUKS volume on "lk" is in the key store' $T/out || fail "import with LUKS: $(cat $T/out)"
# the number of keyslots of the LUKS volume on partition $1
lh() { cryptsetup luksDump $MNT/$1 | grep -c '^  [0-9]*: luks2'; }
[ "$(lh lk)" = 1 ] || fail "the LUKS volume of lk has $(lh lk) keys"
ctl -c "close lk" > /dev/null
run "-c unlock -c 'open lk' -c 'luksopen lk' -c keys" A2
[ -b /dev/mapper/scubed3-lk ] || fail "luksopen lk with the imported passphrase: $(cat $T/out)"
grep -q "^  lk \[LUKS\]" $T/out || fail "keys does not show LUKS for lk: $(cat $T/out)"
cryptsetup close scubed3-lk
ctl -c "close lk" > /dev/null
ok "import of an open partition with LUKS: with its passphrase its volume key goes into the key store (the volume is not changed)"
cp $K $T/keys.before
krun "-c unlock-all -c passphrase-add -c save" A2 "" "" ""
grep -q "^no passphrase" $T/out || fail "an empty new passphrase: $(cat $T/out)"
grep -q -i "autogenerat" $T/out && fail "an autogenerated passphrase: $(cat $T/out)"
krun "-c unlock-all -c passphrase-add -c save" A2 "" newpass1 newpass2
grep -q "^passphrases do not match" $T/out || fail "two different passphrases: $(cat $T/out)"
cmp -s $K $T/keys.before || fail "the key store changed"
run "-c unlock -c keys" newpass1
grep -q "no view for this passphrase" $T/out || fail "newpass1 has a view: $(cat $T/out)"
ok "a new passphrase is never generated: an empty one is no passphrase, two different ones do not match (the store does not change)"


echo "11. the slots of the views, -k and derive"
# a new view gets a slot at random (not the first free one): 300 new key
# stores with one view, 64 slots, about 4.7 per slot
: > $T/slots
i=0
while [ $i -lt 300 ]; do
	rm -f $T/k1
	ks -K $T/k1 -c create > /dev/null
	printf '\nS\nS\n' | ks -K $T/k1 -c unlock-all -c passphrase-add -c save |
		sed -n 's/.*view i\([0-9][0-9]\) is added (empty, in slot i\1 of i00..i63, chosen at random)$/\1/p' >> $T/slots
	i=$((i + 1))
done
[ "$(wc -l < $T/slots)" = 300 ] || fail "the slot of a new view is not shown"
n=$(sort -u $T/slots | wc -l); max=$(sort $T/slots | uniq -c | sort -n | tail -n 1 | awk '{ print $1 }')
[ $n -ge 50 ] && [ $max -le 18 ] || fail "the slots are not random: $n slots used, one $max times"
printf 'S\n' | ctl -K $T/k1 -c unlock -c keys > $T/out
grep -q "^view i$(tail -n 1 $T/slots): (empty)$" $T/out || fail "keys does not show the view by its slot: $(cat $T/out)"
ok "300 new views are in $n of the 64 slots (at most $max in one), their default name is the slot (iNN)"
key=$(printf 'S\n' | ks -K $T/k1 -c derive | sed -n 's/.*key: \([0-9a-f]\{64\}\)$/\1/p')
[ ${#key} = 64 ] || fail "derive: $(printf 'S\n' | ks -K $T/k1 -c derive)"
printf '%s\n' "$key" | ctl -k -K $T/k1 -c unlock -c keys > $T/out
grep -q "view i[0-9][0-9]: 0 entries" $T/out || fail "-k unlock with the key: $(cat $T/out)"
printf '%s\n' "$key" | tr a-f A-F | ctl -k -K $T/k1 -c unlock > $T/out
grep -q "view i[0-9][0-9]: 0 entries" $T/out || fail "-k unlock with the key in capitals: $(cat $T/out)"
printf 'S\n' | ctl -k -K $T/k1 -c unlock > $T/out && fail "-k takes a passphrase"
grep -q "not a key" $T/out || fail "-k with a passphrase: $(cat $T/out)"
printf '%s\n' "${key%?}" | ctl -k -K $T/k1 -c unlock > $T/out && fail "-k takes 63 digits"
printf '%s\n' "$key" | ks -k -K $T/k1 -c unlock -c keys > $T/out
grep -q "view i[0-9][0-9]: 0 entries" $T/out || fail "keystorectl -k unlock with the key: $(cat $T/out)"
# a view added with -k (its key from derive) opens with the passphrase
key2=$(printf 'T\n' | ks -K $T/k1 -c derive | sed -n 's/.*key: \([0-9a-f]\{64\}\)$/\1/p')
printf '%s\n\n%s\n%s\n' "$key" "$key2" "$key2" | ks -k -K $T/k1 -c unlock-all -c passphrase-add -c save > $T/out
grep -q "view i[0-9][0-9] is added" $T/out || fail "-k passphrase-add: $(cat $T/out)"
printf 'T\n' | ctl -K $T/k1 -c unlock > $T/out
grep -q "view i[0-9][0-9]: 0 entries" $T/out || fail "the passphrase of a view added with -k: $(cat $T/out)"
ok "derive gives the key of a passphrase, -k unlocks and adds views with it (not with a passphrase or a wrong key)"

echo "12. two keystorectl at the same time, scubed3ctl only reads"
# keystorectl does not use the daemon (only import does): two run at the
# same time; A reads the key store (unlock-all), waits for its new
# passphrase; B adds a view meanwhile: the save of A would lose it, it is
# refused
{ printf 'S\n\n'; sleep 4; printf 'U\nU\n'; } | ks -K $T/k1 -c unlock-all -c passphrase-add -c save > $T/outA 2>&1 &
pa=$!
sleep 2
printf 'S\n\nV\nV\n' | ks -K $T/k1 -c unlock-all -c passphrase-add -c save > $T/outB 2>&1
wait $pa
grep -q "the key store is written" $T/outB || fail "B: $(cat $T/outB)"
grep -q "is NOT written: another keystorectl changed it" $T/outA || fail "A: $(cat $T/outA)"
printf 'V\n' | ctl -K $T/k1 -c unlock > $T/out
grep -q "view i[0-9][0-9]: 0 entries" $T/out || fail "the view of B is lost: $(cat $T/out)"
printf 'U\n' | ctl -K $T/k1 -c unlock > $T/out
grep -q "no view for this passphrase" $T/out || fail "the view of A is written: $(cat $T/out)"
ls $T | grep -q '\.new' && fail "a temporary file is left: $(ls $T)"
ok "a save after another keystorectl wrote the key store is refused (it would lose that change)"
ks -K $T/k1 -c create > $T/out
grep -q "unable to create the key store" $T/out || fail "create over a key store: $(cat $T/out)"
printf 'V\n' | ctl -K $T/k1 -c unlock | grep -q "view i[0-9][0-9]: 0 entries" || fail "create changed the key store"
ok "create does not replace an existing key store"
# scubed3ctl has no commands that change the key store (they go to the
# daemon, it does not know them); it does not write it
cp $T/k1 $T/k1.before
for c in "add z 1" "passphrase-add" "save" "create z"; do
	printf 'V\n\nW\nW\n' | ctl -K $T/k1 -c unlock -c "$c" > /dev/null 2>&1
done
ctl -c "close z" > /dev/null 2>&1
cmp -s $T/k1 $T/k1.before || fail "scubed3ctl changed the key store"
ok "scubed3ctl does not change the key store"

echo "13. waiting for the daemon, killed scubed3ctl"
# the daemon serves one scubed3ctl at a time: the second says why it waits
{ sleep 5; printf 'S\n'; } | ctl -K $T/k1 -c unlock > /dev/null 2>&1 &
pa=$!
sleep 1
$C -C $T/s3.conf -c p > $T/out 2> $T/err
wait $pa
grep -q "waiting: the scubed3 daemon serves another scubed3ctl" $T/err || fail "no waiting message: $(cat $T/err)"
grep -q " blocks total" $T/out || fail "p after waiting: $(cat $T/out)"
$C -C $T/s3.conf -c p 2>&1 > /dev/null | grep -q waiting && fail "a waiting message without another scubed3ctl"
ok "a scubed3ctl that waits for another one says so"
# killed at a passphrase prompt on a terminal: echo is on again
python3 -I - "$C" $T/s3.conf $T/k1 << 'EOF' || fail "the terminal after a kill"
import os, pty, sys, time, termios, signal
pid, fd = pty.fork()
if pid == 0:
	os.execv(sys.argv[1], [sys.argv[1], "-q", "-C", sys.argv[2], "-K", sys.argv[3], "-c", "unlock"])
out = b""
end = time.time() + 10
while b"passphrase: " not in out and time.time() < end:
	out += os.read(fd, 1024)
time.sleep(0.3)
if termios.tcgetattr(fd)[3] & termios.ECHO:
	print("echo is on at the prompt"); sys.exit(1)
os.kill(pid, signal.SIGTERM)
os.waitpid(pid, 0)
if not termios.tcgetattr(fd)[3] & termios.ECHO:
	print("echo is off after SIGTERM"); sys.exit(1)
EOF
ok "killed (SIGTERM) at a passphrase prompt: the terminal has echo again"
# temporary files of a write that was killed with SIGKILL: removed by the
# next write if their process is gone
head -c 100 /dev/urandom > $T/k1.new.999999
: > $T/k1.new.$$
printf 'S\n\n' | ks -K $T/k1 -c unlock-all -c "add z1 1" -c save > $T/out
grep -q "the key store is written" $T/out || fail "add z1: $(cat $T/out)"
[ -e $T/k1.new.999999 ] && fail "the temporary file of a gone process is left"
[ -e $T/k1.new.$$ ] || fail "the temporary file of a running process is removed"
rm -f $T/k1.new.$$
ls /run | grep -q scubed3ctl- && fail "a key file is in /run"
ok "a temporary file of a killed write is removed by the next write, import leaves no key file"

echo "14. add-passphrase, luksformat with a passphrase"
printf 'S\n\npp3pass\npp3pass\n' | ks -K $T/k1 -c unlock-all -c "add-passphrase p3 1" -c save > $T/out
grep -q '"p3" has the key of its passphrase' $T/out || fail "add-passphrase p3: $(cat $T/out)"
printf 'S\n' | ctl -K $T/k1 -c unlock -c "create p3" -c "resize p3 $LUKS" > $T/out
[ -e $MNT/p3 ] || fail "create p3: $(cat $T/out)"
ctl -c "close p3" > /dev/null
printf 'pp3pass\n' | ctl -K $T/nokeys -c "open p3" > /dev/null
[ -e $MNT/p3 ] || fail "p3 does not open with its passphrase (without the key store)"
printf 'S\n\n\n\n' | ks -K $T/k1 -c unlock-all -c "add-passphrase p4 1" > $T/out
grep -q '"p4" has new random keys' $T/out || fail "add-passphrase with an empty passphrase: $(cat $T/out)"
printf 'S\n\nxx\nyy\n' | ks -K $T/k1 -c unlock-all -c "add-passphrase p5 1" > $T/out
grep -q "passphrases do not match" $T/out && ! grep -q '"p5"' $T/out || fail "two different passphrases: $(cat $T/out)"
ok "add-passphrase: the partition opens with its passphrase without the key store; empty: a random key"
# luksformat: the volume key goes into the key store, the one keyslot gets
# the passphrase (any length: 300 characters) or a random key (empty)
long=$(printf 'lp3pass%0293d' 0)
printf 'S\n%s\n%s\n' $long $long | ctl -K $T/k1 -c unlock -c "luksformat p3" -c "luksstatus p3" > $T/out
grep -q '"p3" is formatted, its volume key is in the key store' $T/out &&
	grep -q "its LUKS volume key is in view i[0-9][0-9] of the key store" $T/out || fail "luksformat p3: $(cat $T/out)"
[ "$(lh p3)" = 1 ] || fail "the LUKS volume of p3 has $(lh p3) keyslots"
printf %s $long | cryptsetup open --test-passphrase --key-file=- $MNT/p3 || fail "the keyslot of p3 has not its passphrase"
printf %s $long | cryptsetup open --key-file=- $MNT/p3 lp3 && cryptsetup close lp3 || fail "the passphrase does not open p3"
printf 'S\n' | ctl -K $T/k1 -c unlock -c "luksopen p3" > /dev/null
[ -b /dev/mapper/scubed3-p3 ] || fail "the volume key in the key store does not open p3"
cryptsetup close scubed3-p3
printf 'S\n\n\n' | ctl -K $T/k1 -c unlock -c "luksformat p3" > $T/out
[ "$(lh p3)" = 1 ] || fail "luksformat without a passphrase: $(cat $T/out)"
printf %s $long | cryptsetup open --test-passphrase --key-file=- $MNT/p3 2> /dev/null && fail "the old passphrase opens p3"
printf 'S\n' | ctl -K $T/k1 -c unlock -c "luksopen p3" > /dev/null
[ -b /dev/mapper/scubed3-p3 ] || fail "the volume key does not open p3 (no passphrase)"
cryptsetup close scubed3-p3
printf 'S\nab\ncd\n' | ctl -K $T/k1 -c unlock -c "luksformat p3" > $T/out
grep -q "passphrases do not match" $T/out && ! grep -q "is formatted, its volume key" $T/out || fail "two different passphrases: $(cat $T/out)"
ctl -c "close p3" > /dev/null
ok "luksformat: one keyslot with the passphrase (300 characters) or a random key, the volume key is in the key store (it opens the volume)"

echo "15. the names of the views"
# at the keys> prompt unlock asks a name (only digits is refused), keys
# shows it and the slot, name renames, lock and add take the name; quit
# asks if the changes are not saved
(for l in unlock S 12 work keys "name work home2" "add z9 home2" keys "lock home2" keys quit y; do
	sleep 1; printf '%s\n' "$l"; done; sleep 1) |
	script -qc "$KS -C $T/s3.conf -K $T/k1" /dev/null | tr -d '\r' |
	sed 's/\x1b\[?2004[hl]//g' > $T/out
grep -q "the name of a view is not only digits" $T/out && grep -q "view work: [0-9]* entries" $T/out ||
	fail "unlock with a name: $(cat $T/out)"
grep -q "^view work:.* \[slot i[0-9][0-9]\]$" $T/out || fail "keys with a name: $(cat $T/out)"
grep -q "view work is now view home2 (slot i[0-9][0-9]" $T/out && grep -q '"z9" has new random keys in view home2' $T/out &&
	grep -q "view home2 is locked" $T/out || fail "name, add, lock: $(cat $T/out)"
grep -q "the changes are NOT saved, quit anyway" $T/out || fail "quit with changes: $(cat $T/out)"
# with -c (scripts): the default name, the slot
printf 'S\n' | ks -K $T/k1 -c unlock -c keys > $T/out
slot=$(sed -n 's/.*view i\([0-9][0-9]\): [0-9]* entries$/\1/p' $T/out)
[ -n "$slot" ] && grep -q "^view i$slot:$" $T/out || fail "the default name: $(cat $T/out)"
printf 'S\n' | ks -K $T/k1 -c unlock -c "name i$slot x1" -c "name 1 x2" -c keys > $T/out
grep -q "view i$slot is now view x1" $T/out && grep -q "view x1 is now view x2" $T/out &&
	grep -q "^view x2: \[slot i$slot\]$" $T/out || fail "name with -c: $(cat $T/out)"
ok "the names of views: asked at the keys> prompt (default iNN, the slot), name renames, commands take the name (or the number)"

cleanup
echo "KEYSTORETEST OK"
