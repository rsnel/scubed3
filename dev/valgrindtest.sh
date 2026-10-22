#!/bin/sh
# valgrindtest.sh - the daemon, scubed3ctl and keystorectl under valgrind
# (memcheck):
# no memory errors and no leaks (definitely, indirectly or possibly lost)
#
# the workload: partitions are created, resized, written and read; ext4 on
# a partition (a loop device) with discard (fstrim); paranoia level 2 and 3
# (in level 3 a writer that waits for its turn, reads of another partition
# and control commands at the same time);
# more reserved blocks and back (resize-internal), a shrink, a removal
# (wipe) in level 1; close, open, a wrong passphrase; the key store
# (keystorectl: create, passphrases, add, import, remove, change, save, quit
# without save, derive; scubed3ctl: unlock, open-all, unlock-until,
# luksopen-all, lock);
# then the daemon is started again on the same base device (replay), a
# removal with wipe in level 2, check-data-integrity
#
# with TOOL=helgrind (or drd) the daemon runs under that thread error
# detector instead (data races, lock order, misuse of the pthread API),
# scubed3ctl and keystorectl (one thread) without valgrind
#
# usage: [TOOL=helgrind] valgrindtest.sh DAEMON CTL (as root, keystorectl
# is next to CTL; binaries
# built with -g; -O1 gives better stack traces); slow: the cipher runs under
# valgrind
D=$(realpath $1); C=$(realpath $2); T=$(mktemp -d); MNT=/mnt/s3
KS=$(dirname $C)/keystorectl
FUSERMOUNT=$(command -v fusermount3 || echo fusermount)
TOOL=${TOOL:-memcheck}
case $TOOL in
memcheck)
	VG="valgrind --leak-check=full --show-leak-kinds=definite,indirect,possible
		--errors-for-leak-kinds=definite,indirect,possible --track-origins=yes
		--num-callers=30"
	VGCTL=$VG;;
helgrind|drd)
	# the known races inside libfuse are suppressed by function name
	# (valgrind.supp); that needs the debug information of libfuse,
	# without it every race reported inside libfuse is suppressed
	SUPP=$(cd "$(dirname "$0")" && pwd)/valgrind.supp
	lib=$(ldd $D | awk '/libfuse3/ { print $3 }')
	id=$(readelf -n "$lib" 2> /dev/null | sed -n 's/.*Build ID: *//p')
	if ! readelf -S "$lib" 2> /dev/null | grep -q debug_info &&
	   [ ! -f /usr/lib/debug/.build-id/$(echo $id | cut -c1-2)/$(echo $id | cut -c3-).debug ]; then
		echo "   (no debug information in $lib: races inside libfuse are suppressed broadly)"
		SUPP=${SUPP%.supp}-nodebug.supp
	fi
	VG="valgrind --tool=$TOOL --num-callers=30 --suppressions=$SUPP"
	VGCTL=;;
*)	echo "unknown TOOL $TOOL"; exit 1;;
esac
FS=$T/fs; LOOP=
cleanup() {
	umount $FS 2> /dev/null; [ -n "$LOOP" ] && losetup -d $LOOP 2> /dev/null
	$FUSERMOUNT -u $MNT 2> /dev/null; wait; rm -rf $T
}
fail() {
	echo "VALGRINDTEST FAILED: $1"
	mkdir -p valgrind-logs && cp $T/*.vg valgrind-logs/ 2> /dev/null &&
		echo "(the logs are in valgrind-logs/)"
	cleanup; exit 1
}
command -v valgrind > /dev/null || { echo "VALGRINDTEST SKIPPED: no valgrind"; exit 0; }
[ -e /run/scubed3/control ] && fail "a scubed3 daemon is running"
mkdir -p $MNT $FS
printf 'iterations=1000\nsalt=valgrindtest\n' > $T/conf
head -c $((300<<16)) /dev/urandom > $T/base
head -c 1500000 /dev/urandom > $T/data

n=0
daemon() {
	$VG --log-file=$T/daemon$1.vg $D -f -b $T/base -m 12 -M 16 $MNT > $T/daemon$1.out 2>&1 &
	DPID=$!
	i=0; while [ ! -S /run/scubed3/control ]; do
		sleep 1; i=$((i + 1)); [ $i -gt 120 ] && fail "the daemon does not start"
	done
}
stop() {
	$FUSERMOUNT -u $MNT; wait $DPID
	i=0; while [ -S /run/scubed3/control ]; do sleep 1; i=$((i + 1)); [ $i -gt 30 ] && break; done
}
ctl() { n=$((n + 1)); ${VGCTL:+$VGCTL --log-file=$T/ctl.$n.vg} $C -q -YYY -C $T/conf -K $T/keys "$@" >> $T/ctl.out 2>&1; }
ks() { n=$((n + 1)); ${VGCTL:+$VGCTL --log-file=$T/ctl.$n.vg} $KS -q -YYY -C $T/conf -K $T/keys "$@" >> $T/ctl.out 2>&1; }

echo "1. the daemon and scubed3ctl, a workload"
daemon 1
printf 'pa\npa\n' | ctl -c "create a"
printf 'pb\npb\n' | ctl -c "create b"
ctl -c "resize a 150" -c "resize b 50" -c p -c "info a" -c static-info -c help
dd if=$T/data of=$MNT/b bs=64k conv=notrunc 2> /dev/null; sync
cmp -s -n 1500000 $T/data $MNT/b || fail "the data of b"
LOOP=$(losetup -f --show $MNT/a) || fail "losetup"
mkfs.ext4 -q -F $LOOP 2> /dev/null && mount $LOOP $FS || fail "ext4 on a"
for f in 1 2 3; do head -c 1000000 /dev/urandom > $FS/f$f; done; sync
rm $FS/f1; fstrim $FS; sync
ctl -c "paranoia 2"
head -c 1000000 /dev/urandom > $FS/f4; sync
ctl -c "paranoia 3 4096"
# concurrently: a writer that waits for its turn, reads of another
# partition, control commands
(for i in 1 2 3 4 5 6; do cat $MNT/b > /dev/null; done) &
RPID=$!
head -c 500000 /dev/urandom > $FS/f5; sync &
WPID=$!
for i in 1 2 3; do ctl -c p -c "info b"; done
wait $WPID $RPID; sleep 5
ctl -c p -c "paranoia 1"
umount $FS; losetup -d $LOOP; LOOP=; sleep 1
ctl -c "resize-internal a 170 58"
ctl -c "resize-internal a 150 38"
ctl -c "resize b 30"
dd if=$T/data of=$MNT/b bs=64k count=20 conv=notrunc 2> /dev/null; sync
ctl -c "info b" -c "check-data-integrity b" -c "resize b 0"
sleep 2; ctl -c p -c "close b" -c "close a"
printf 'pa\n' | ctl -c "open a"
printf 'wrong\n' | ctl -c "open c"
ctl -c "close a"
ks -c create
printf '\nK1\nK1\n' | ks -c unlock-all -c passphrase-add -c save
printf 'K1\n\nK2\nK2\n' | ks -c unlock-all -c passphrase-add -c save
printf 'K1\nK2\n\n' | ks -c unlock-all -c "add x 1" -c "add x 2" -c "add y 2" -c save
printf 'K1\nK2\n\npa\n' | ks -c unlock-all -c "import a 2" -c keys -c save
printf 'K2\n' | ctl -c unlock -c keys -c open-all -c "open a"
printf 'K1\nK2\n' | ctl -c unlock -c unlock -c open-all -c p -c lock
ctl -c "close a"
printf 'K1\nK2\n' | ctl -c "unlock-until a" -c "luksopen-all a" -c keys
ctl -c "close a"
printf 'K1\nK2\n\nK3\nK3\n' | ks -c unlock-all -c "passphrase-change 1" -c "remove y 2" -c save
printf 'K3\nK2\n\n' | ks -c unlock-all -c "passphrase-remove 2" -c save
printf 'K3\n' | ks -c unlock -c "name 1 v" -c "add z v" -c "lock v"
printf 'K3\n' | ks -c derive
stop

echo "2. the daemon again (replay), a removal in level 2"
daemon 2
printf 'pa\n' | ctl -c "open a" -c "info a" -c "check-data-integrity a"
printf 'pc\npc\n' | ctl -c "create c" -c "resize c 40"
dd if=$T/data of=$MNT/c bs=64k conv=notrunc 2> /dev/null; sync
ctl -c "paranoia 2" -c "resize c 0"
for i in 1 2 3 4 5 6 7 8; do
	dd if=/dev/urandom of=$MNT/a bs=64k count=20 conv=notrunc 2> /dev/null; sync
done
ctl -c p -c "paranoia 1"
sleep 1; ctl -c p -c "close c" -c "close a"
stop

bad=0
for f in $T/daemon1.vg $T/daemon2.vg $(ls $T/ctl.*.vg 2> /dev/null); do
	e=$(sed -n 's/.*ERROR SUMMARY: \([0-9]*\) errors.*/\1/p' $f)
	if [ "$e" != 0 ]; then
		echo "   $(basename $f): $e errors"
		sed 's/^==[0-9]*== //' $f | head -60
		bad=1
	fi
done
[ $bad = 0 ] || fail "valgrind found errors or leaks"
if [ $TOOL = memcheck ]; then
	echo "   ok: the daemon (2 runs) and $n runs of scubed3ctl and keystorectl: no errors, no leaks"
else
	echo "   ok: the daemon (2 runs): no errors ($TOOL)"
fi
cleanup
echo "VALGRINDTEST OK"
