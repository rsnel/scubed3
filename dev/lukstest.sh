#!/bin/sh
# lukstest.sh - LUKS on top of a scubed3 partition with scubed3ctl, and the
# warning below the recommended paranoia level (paranoia= in the
# configuration file, default 2)
#
# in level 1 mount and luksformat warn, an answer other than yes cancels;
# luksformat, mke2fs and mount (in level 2) work on the LUKS volume, umount
# closes it, a file survives umount and mount, and its plaintext is not in
# the partition (it is encrypted by LUKS)
#
# usage: lukstest.sh DAEMON CTL (as root, needs cryptsetup and device
# mapper; LUKSTEST_FAKE=1: cryptsetup in PATH is a stand-in without
# encryption, the plaintext check is skipped)
D=$1; C=$2; T=$(mktemp -d); MNT=/mnt/s3; FS=$T/fs
FUSERMOUNT=$(command -v fusermount3 || echo fusermount)
cleanup() {
	umount $FS 2>/dev/null; cryptsetup close scubed3-t 2>/dev/null
	$FUSERMOUNT -u $MNT 2>/dev/null; wait; rm -rf $T
}
fail() { echo "LUKSTEST FAILED: $1"; [ -f $T/out ] && cat $T/out; cleanup; exit 1; }
command -v cryptsetup > /dev/null || { echo "LUKSTEST SKIPPED: no cryptsetup"; exit 0; }
[ -e /etc/scubed3.conf ] && fail "/etc/scubed3.conf exists, move it away first"

printf 'iterations=1000\n' > $T/s3.conf
head -c $((32<<22)) /dev/urandom > $T/base
$FUSERMOUNT -u -z $MNT 2>/dev/null; rm -f /run/scubed3/control
$D -b $T/base -f $MNT > $T/log 2>&1 & sleep 0.5

# run a scubed3ctl command on a terminal, the answers to its prompts are
# typed one per second
run() {
	cmd=$1; shift
	(for l in "$@"; do sleep 1; printf '%s\n' "$l"; done; sleep 2) |
		script -qc "$C -C $T/s3.conf -c '$cmd'" /dev/null | tr -d '\r' > $T/out
}
run "create t" pw pw
$C -q -YYY -C $T/s3.conf -c "resize t 20" > /dev/null
mkdir -p $FS

# level 1: a warning, only yes goes on
run "mount t $FS" no
grep -q "the recommended level is 2" $T/out || fail "no warning in level 1"
grep -q "cancelled" $T/out || fail "mount not cancelled"
mountpoint -q $FS && fail "mounted after no"
run "luksformat t" Yes
cryptsetup isLuks $MNT/t && fail "luksformat after Yes"
run "luksformat t" yes YES lpw lpw
cryptsetup isLuks $MNT/t || fail "no LUKS header after luksformat"
echo "   ok: level 1 warns, luksformat after yes"

# level 2: no warning
$C -q -YYY -C $T/s3.conf -c "paranoia 2" > /dev/null
run "mke2fs t" lpw
grep -q "recommended" $T/out && fail "warning in level 2"
[ -e /dev/mapper/scubed3-t ] && fail "LUKS volume still open after mke2fs"
run "mount t $FS" lpw
mountpoint -q $FS || fail "not mounted"
M=$(head -c 24 /dev/urandom | od -An -tx1 | tr -d ' \n')
for i in 1 2 3 4 5 6 7 8; do echo "marker $M" >> $FS/f; done; sync
run "umount t"
mountpoint -q $FS && fail "still mounted after umount"
[ -e /dev/mapper/scubed3-t ] && fail "LUKS volume not closed by umount"
run "mount t $FS" lpw
grep -q "marker $M" $FS/f || fail "the file is lost after umount and mount"
run "umount $FS"
[ -e /dev/mapper/scubed3-t ] && fail "LUKS volume not closed by umount MOUNTPOINT"
echo "   ok: mke2fs, mount, umount on LUKS (level 2)"
if [ -z "$LUKSTEST_FAKE" ]; then
	grep -q "marker $M" $MNT/t && fail "plaintext in the partition"
	echo "   ok: the partition does not contain the plaintext"
fi

$C -q -YYY -C $T/s3.conf -c "paranoia 1" > /dev/null
$C -q -YYY -C $T/s3.conf -c "close t" > /dev/null
cleanup
echo "LUKSTEST OK"
