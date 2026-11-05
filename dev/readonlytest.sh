#!/bin/sh
# readonlytest.sh - read-only partitions (set-readonly NAME 1): p shows
# [R], info readonly=1, the file has mode 0400; opening it for writing
# fails also for root (dd, fallocate), reading works and the data is
# unchanged; it can grow but not be made smaller or removed; the flag is
# changed only when it is not in use (or to the value it has); scubed3ctl
# refuses mke2fs and luksformat, mounts it read-only (with and without
# LUKS: cryptsetup open --readonly), without a warning about the paranoia
# level; in level 2 the writes of another partition do not change its
# data; after close and open it is writable again
#
# usage: readonlytest.sh DAEMON SCUBED3CTL (as root; CRYPTSETUP=path: the
# cryptsetup to use, default the stand-in dev/fake-cryptsetup)
D=$(realpath $1); C=$(realpath $2); T=$(mktemp -d); MNT=/mnt/s3
DEV=$(cd "$(dirname "$0")" && pwd)
FUSERMOUNT=$(command -v fusermount3 || echo fusermount)
mkdir -p $T/bin $T/m
ln -s ${CRYPTSETUP:-$DEV/fake-cryptsetup} $T/bin/cryptsetup
export PATH=$T/bin:$PATH
cleanup() {
	umount $T/m 2> /dev/null; cryptsetup close scubed3-l 2> /dev/null
	$FUSERMOUNT -u $MNT 2> /dev/null; wait; rm -rf $T
}
fail() { echo "READONLYTEST FAILED: $1"; cleanup; exit 1; }
ok() { echo "   ok: $1"; }
ctl() { $C -q -YYY -C $T/s3.conf "$@"; }

printf 'iterations=1000\nsalt=readonlytest\n' > $T/s3.conf
head -c $((300<<16)) /dev/urandom > $T/base
$FUSERMOUNT -u -z $MNT 2> /dev/null; rm -f /run/scubed3/control
$D -b $T/base -m 12 -M 16 -f $MNT > $T/log 2>&1 &
until [ -S /run/scubed3/control ]; do sleep 0.1; done

echo "1. the flag"
printf 'pa\npa\n' | ctl -c "create a" -c "resize a 30" > /dev/null
head -c 500000 /dev/urandom > $T/data
dd if=$T/data of=$MNT/a conv=notrunc 2> /dev/null; sync
ctl -c "set-readonly a 1" > $T/out || fail "set-readonly: $(cat $T/out)"
ctl -c p | grep -q " blocks in a .*\[R\]" || fail "p: $(ctl -c p)"
ctl -c "info a" | grep -qx "readonly=1" || fail "info: $(ctl -c "info a")"
sleep 1.5 # the kernel keeps the attributes for a second
[ "$(stat -c %a $MNT/a)" = 400 ] || fail "mode $(stat -c %a $MNT/a)"
ctl -c "set-readonly a maybe" | grep -q "expected boolean" || fail "a wrong value"
ok "p shows [R], info readonly=1, mode 0400, only a boolean is taken"

echo "2. no writes"
dd if=/dev/zero of=$MNT/a bs=4096 count=1 conv=notrunc 2> /dev/null && fail "dd wrote"
python3 -c "import os,sys; os.open(sys.argv[1], os.O_RDWR)" $MNT/a 2> /dev/null && fail "O_RDWR"
fallocate -p -o 0 -l 65536 $MNT/a 2> /dev/null && fail "fallocate punched a hole"
cmp -s -n 500000 $T/data $MNT/a || fail "the data changed"
ok "root cannot open it for writing (dd, O_RDWR, fallocate), reading works, the data is unchanged"

echo "3. resize and the flag while in use"
ctl -c "resize a 20" | grep -q "read-only, it cannot be made smaller" || fail "shrink"
ctl -c "resize a 0" | grep -q "read-only, it cannot be made smaller" || fail "removal"
ctl -c "resize a 40" > $T/out; ctl -c "info a" | grep -qx "no_macroblocks=40" || fail "grow: $(cat $T/out)"
exec 3< $MNT/a
ctl -c "set-readonly a 0" | grep -q "is busy" || fail "set-readonly 0 while in use"
ctl -c "set-readonly a 1" > $T/out; [ -s $T/out ] && fail "the same value while in use: $(cat $T/out)"
exec 3<&-
ok "it can grow, not shrink or be removed; the flag changes only when it is not in use"

echo "4. scubed3ctl"
ctl -c "set-readonly a 0" > /dev/null
ctl -c "mke2fs a" > /dev/null 2>&1
ctl -c "mount a $T/m" > /dev/null 2>&1 && echo hello > $T/m/f && umount $T/m || fail "mount rw"
ctl -c "set-readonly a 1" > /dev/null
ctl -c "mke2fs a" > $T/out; grep -q "is read-only" $T/out || fail "mke2fs: $(cat $T/out)"
ctl -c "luksformat a" > $T/out; grep -q "is read-only" $T/out || fail "luksformat: $(cat $T/out)"
$C -C $T/s3.conf -c "mount a $T/m" < /dev/null > $T/out 2>&1
grep -q WARNING $T/out && fail "a warning about the level: $(cat $T/out)"
grep -q " $T/m ext2 ro[,]" /proc/mounts || fail "not mounted read-only: $(grep $T/m /proc/mounts)"
[ "$(cat $T/m/f)" = hello ] || fail "the file system"
touch $T/m/g 2> /dev/null && fail "a file was made"
umount $T/m
ok "mke2fs and luksformat refuse, mount mounts it read-only (no warning)"

echo "5. LUKS"
printf 'pl\npl\n' | ctl -c "create l" -c "resize l 30" > /dev/null
printf 'lukspw' | cryptsetup -q luksFormat --key-file=- $MNT/l || fail "luksFormat"
ctl -c "set-readonly l 1" > /dev/null
printf 'lukspw\n' | ctl -c "luksopen l" > $T/out 2>&1
[ -b /dev/mapper/scubed3-l ] || fail "luksopen: $(cat $T/out)"
[ "$(blockdev --getro /dev/mapper/scubed3-l)" = 1 ] || fail "the LUKS volume is writable"
cryptsetup close scubed3-l
ok "luksopen opens the LUKS volume read-only"

echo "6. level 2, close"
printf 'pb\npb\n' | ctl -c "create b" -c "resize b 60" > /dev/null
ctl -c "paranoia 2" > /dev/null
for i in 1 2 3 4 5 6; do head -c 300000 /dev/urandom | dd of=$MNT/b conv=notrunc 2> /dev/null; sync; done
ctl -c "paranoia 1" > /dev/null
cmp -s -n 4096 $T/data $MNT/a && fail "the file system is not on a"
dd if=$MNT/a of=$T/a1 bs=4096 count=100 2> /dev/null
ctl -c "close a" > /dev/null; printf 'pa\n' | ctl -c "open a" > /dev/null
cmp -s $T/a1 $MNT/a -n 409600 || fail "the data of a changed in level 2"
ctl -c "info a" | grep -qx "readonly=0" || fail "read-only after close and open"
dd if=/dev/zero of=$MNT/a bs=4096 count=1 seek=50 conv=notrunc 2> /dev/null || fail "not writable after open"
ok "level 2: its data stays; after close and open it is writable"

cleanup
echo "READONLYTEST OK"
