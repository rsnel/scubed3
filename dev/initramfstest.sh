#!/bin/sh
# initramfstest.sh - the local-top script of contrib/initramfs-tools (root on
# scubed3) in a simulated initramfs: its paths point into a temporary
# directory, /scripts/functions is a stand-in (panic), the answers are typed
# on a terminal (script(1)): the name of the root partition (ROOT_PARTITION
# is not set), a wrong passphrase, the name again and the right passphrase,
# the name and passphrase of another partition, an empty name, Yes for
# paranoia level 2
#
# afterwards: ROOT in conf/param.conf is a loop device with the file system
# of the root partition, the daemon runs as @scubed3 (argv[0]) with
# oom_score_adj -1000, both partitions are open, the level is 2
#
# usage: initramfstest.sh DAEMON CTL (as root, in the directory with the
# binaries; uses /dev/scubed3fs and /run/scubed3/control, like a real boot)
D=$(realpath $1); C=$(realpath $2); P=$(mktemp -d); S=$(cd "$(dirname "$0")/.." && pwd)
FUSERMOUNT=$(command -v fusermount3 || echo fusermount)
FS=$P/fs; MNT=/dev/scubed3fs
cleanup() {
	umount $FS 2> /dev/null
	[ -n "$LOOP" ] && losetup -d $LOOP 2> /dev/null
	$P/sbin/scubed3ctl -q -YYY -c "paranoia 1" > /dev/null 2>&1
	for n in root hidden; do $P/sbin/scubed3ctl -c "close $n" > /dev/null 2>&1; done
	$FUSERMOUNT -u $MNT 2> /dev/null; $FUSERMOUNT -u /mnt/s3 2> /dev/null; sleep 1
	rmdir $MNT 2> /dev/null; rm -rf $P
}
fail() { echo "INITRAMFSTEST FAILED: $1"; [ -f $P/out ] && cat $P/out; cleanup; exit 1; }
[ -e /run/scubed3/control ] && fail "a scubed3 daemon is running"

# the initramfs: the configuration, the binaries, the stand-ins
mkdir -p $P/scripts $P/conf $P/sbin $P/etc $FS
cat > $P/scripts/functions <<EOF
panic() { echo "PANIC: \$*"; exit 1; }
EOF
printf 'iterations=1000\nparanoia=2\n' > $P/etc/scubed3.conf
printf 'BASE=%s\nOPTIONS="-m 12 -M 16"\n' $P/base > $P/conf/scubed3.conf
cp $D "$P/sbin/@scubed3"
printf '#!/bin/sh\nexec %s -C %s/etc/scubed3.conf "$@"\n' $C $P > $P/sbin/scubed3ctl
chmod +x $P/sbin/scubed3ctl
ln -s "$(command -v losetup)" $P/sbin/losetup-util
sed -e "s|/scripts/functions|$P/scripts/functions|" -e "s|/conf/|$P/conf/|g" \
	-e "s|/sbin/|$P/sbin/|g" -e "s|PATH=/sbin:|PATH=$P/sbin:|" \
	-e "s|/etc/scubed3.conf|$P/etc/scubed3.conf|g" \
	$S/contrib/initramfs-tools/scripts/local-top/scubed3 > $P/local-top
grep -q "^\. $P/scripts/functions" $P/local-top || fail "the paths of the script are not replaced"

# the partitions, made with a normal daemon: root (ext2, a file) and hidden
head -c $((200<<16)) /dev/urandom > $P/base
$FUSERMOUNT -u -z /mnt/s3 2> /dev/null
$D -b $P/base -m 12 -M 16 /mnt/s3 || fail "daemon"
# type answers (one per second) to a command on a terminal
typed() {
	cmd=$1; shift
	(for l in "$@"; do sleep 1; printf '%s\n' "$l"; done; sleep 3) |
		script -qc "$cmd" /dev/null | tr -d '\r' > $P/out
}
typed "$P/sbin/scubed3ctl -c 'create root'" pw pw
typed "$P/sbin/scubed3ctl -c 'create hidden'" hpw hpw
$P/sbin/scubed3ctl -q -YYY -c "resize root 60" > /dev/null
$P/sbin/scubed3ctl -q -YYY -c "resize hidden 40" > /dev/null
mke2fs -q -F /mnt/s3/root || fail "mke2fs"
mount -o loop /mnt/s3/root $FS && echo "the root file system" > $FS/marker && umount $FS
$P/sbin/scubed3ctl -c "close root" > /dev/null; $P/sbin/scubed3ctl -c "close hidden" > /dev/null
$FUSERMOUNT -u /mnt/s3; sleep 1

# the boot
typed "sh $P/local-top" root bad root pw hidden hpw "" Yes
cp $P/out $P/boot.out
grep -q PANIC $P/out && fail "panic"
grep -q "ROOT=/dev/loop" $P/conf/param.conf || fail "ROOT is not set: $(cat $P/conf/param.conf 2>&1)"
LOOP=$(sed -n 's/^ROOT=//p' $P/conf/param.conf)
echo "   ok: ROOT=$LOOP"
grep -q "no blocks found" $P/out || fail "the wrong passphrase was accepted"
echo "   ok: the wrong passphrase was asked again"
pid=
for p in /proc/[0-9]*; do
	[ "$(cat $p/comm 2> /dev/null)" = "@scubed3" ] && pid=${p#/proc/}
done
[ -n "$pid" ] || fail "no process @scubed3"
[ "$(head -c 1 /proc/$pid/cmdline)" = "@" ] || fail "argv[0] does not start with @"
# (a container may not allow to lower oom_score_adj)
if sh -c 'echo -1000 > /proc/self/oom_score_adj' 2> /dev/null; then
	[ "$(cat /proc/$pid/oom_score_adj)" = "-1000" ] || fail "oom_score_adj is $(cat /proc/$pid/oom_score_adj)"
	oom="oom_score_adj -1000"
else oom="(oom_score_adj cannot be lowered here, not checked)"
fi
echo "   ok: the daemon runs as $(tr '\0' ' ' < /proc/$pid/cmdline | cut -c1-40)..., $oom"
st=$($P/sbin/scubed3ctl -c p)
echo "$st" | grep -q "paranoia level 2 " || fail "level: $st"
echo "$st" | grep -q " blocks in root" && echo "$st" | grep -q " blocks in hidden" || fail "partitions: $st"
echo "   ok: root and hidden are open, paranoia level 2"
mount -o ro $LOOP $FS || fail "mount of $LOOP"
grep -q "the root file system" $FS/marker || fail "the file system of the root partition"
echo "   ok: $LOOP has the file system of the root partition"
umount $FS
cleanup
echo "INITRAMFSTEST OK"
