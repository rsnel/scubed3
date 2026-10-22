#!/bin/sh
# initramfstest.sh - the local-top script of contrib/initramfs-tools (root on
# scubed3) in a simulated initramfs: its paths point into a temporary
# directory, /scripts/functions is a stand-in (panic), askpass is a stand-in
# (it shows its prompt, it reads a line), cryptsetup is fake-cryptsetup
#
# the key store has two views: A with root (LUKS, the root file system) and
# home (LUKS), H with hidden (LUKS); the answers: a wrong passphrase, H, A;
# what is on the console: three times the prompt of cryptroot ("Please
# unlock disk base_crypt: ", base is the base device), twice its message of
# a wrong passphrase, once "set up successfully", nothing else; afterwards
# ROOT in conf/param.conf is /dev/mapper/myhost-root (luks_prefix=myhost-
# in /etc/scubed3.conf) with the file system,
# the daemon runs as @scubed3 (argv[0]) with oom_score_adj -1000, root, home
# and hidden are open, the LUKS volumes of root and home (the view of root)
# are open, not that of hidden, the level is 2, the copy of the key store is
# gone
#
# a second boot (CRYPTTAB_NAME=sda5_crypt): three wrong passphrases: the
# prompt with that name, the message of cryptroot that the maximum number of
# tries is exceeded, no ROOT, no partition is open
#
# usage: [KEEP=1] initramfstest.sh DAEMON CTL (as root, in the directory with the
# binaries; uses /dev/scubed3fs and /run/scubed3/control, like a real boot)
D=$(realpath $1); C=$(realpath $2); P=$(mktemp -d); S=$(cd "$(dirname "$0")/.." && pwd)
FUSERMOUNT=$(command -v fusermount3 || echo fusermount)
FS=$P/fs; MNT=/dev/scubed3fs
# what a boot leaves behind
unboot() {
	umount $FS 2> /dev/null
	[ -n "$LOOP" ] && losetup -d $LOOP 2> /dev/null
	for n in root home hidden; do
		[ -e /dev/mapper/myhost-$n ] && PATH=$P/sbin:$PATH cryptsetup close myhost-$n
	done
	$P/sbin/scubed3ctl -q -YYY -c "paranoia 1" > /dev/null 2>&1
	for n in root home hidden; do $P/sbin/scubed3ctl -c "close $n" > /dev/null 2>&1; done
	$FUSERMOUNT -u $MNT 2> /dev/null; $FUSERMOUNT -u /mnt/s3 2> /dev/null; sleep 1
	rmdir $MNT 2> /dev/null; LOOP=; rm -f $P/conf/param.conf
}
cleanup() { unboot; [ -n "$KEEP" ] || rm -rf $P; rm -f /run/scubed3/fs; }
fail() { echo "INITRAMFSTEST FAILED: $1"; [ -f $P/out ] && cat $P/out; cleanup; exit 1; }
[ -e /run/scubed3/control ] && fail "a scubed3 daemon is running"

# the initramfs: the configuration, the binaries, the stand-ins
mkdir -p $P/scripts $P/conf $P/sbin $P/etc $P/lib/cryptsetup $P/run/initramfs $FS
cat > $P/scripts/functions <<EOF
panic() { echo "PANIC: \$*"; exit 1; }
EOF
printf 'iterations=1000\nparanoia=2\nluks_prefix=myhost-\n' > $P/etc/scubed3.conf
printf 'BASE=%s\nOPTIONS="-m 12 -M 16"\n' $P/base > $P/conf/scubed3.conf
cp $D "$P/sbin/@scubed3"
printf '#!/bin/sh\nPATH=%s/sbin:$PATH exec %s -C %s/etc/scubed3.conf "$@"\n' $P $C $P > $P/sbin/scubed3ctl
chmod +x $P/sbin/scubed3ctl
# keystorectl (only to make the key store, it is not in the initramfs)
K=$(dirname $C)/keystorectl
ln -s "$(command -v losetup)" $P/sbin/losetup-util
ln -s $S/dev/fake-cryptsetup $P/sbin/cryptsetup
# askpass: the prompt on stderr, the passphrase (a line of stdin) on stdout
cat > $P/lib/cryptsetup/askpass <<'EOF2'
#!/bin/sh
printf '%s' "$1" >&2
read -r p
printf '%s' "$p"
EOF2
chmod +x $P/lib/cryptsetup/askpass
sed -e "s|/scripts/functions|$P/scripts/functions|" -e "s|/conf/|$P/conf/|g" \
	-e "s|/sbin/|$P/sbin/|g" -e "s|PATH=/sbin:|PATH=$P/sbin:|" \
	-e "s|/etc/scubed3.conf|$P/etc/scubed3.conf|g" \
	-e "s|/lib/cryptsetup/askpass|$P/lib/cryptsetup/askpass|" \
	-e "s|/run/initramfs/|$P/run/initramfs/|" \
	$S/contrib/initramfs-tools/scripts/local-top/scubed3 > $P/local-top
grep -q "^\. $P/scripts/functions" $P/local-top || fail "the paths of the script are not replaced"

# the partitions, made with a normal daemon, with the keys of the key store
KS=$P/conf/scubed3.keys
head -c $((300<<16)) /dev/urandom > $P/base
$FUSERMOUNT -u -z /mnt/s3 2> /dev/null
$D -b $P/base -m 12 -M 16 /mnt/s3 || fail "daemon"
# type answers (one per second) to a command on a terminal
typed() {
	cmd=$1; shift
	(for l in "$@"; do sleep 1; printf '%s\n' "$l"; done; sleep 3) |
		script -qc "$cmd" /dev/null | tr -d '\r' > $P/out
}
k="$K -C $P/etc/scubed3.conf -K $KS"
c="$P/sbin/scubed3ctl -K $KS"
typed "$k -c create -c unlock-all -c passphrase-add -c save" "" A A yes
typed "$k -c unlock-all -c passphrase-add -c save" A "" H H yes
typed "$k -c unlock-all -c 'add root 1' -c 'add home 1' -c 'add hidden 2' -c save" A H "" yes
grep -q "the key store is written" $P/out || fail "the key store: $(cat $P/out)"
typed "$c -c unlock -c 'create root' -c 'create home' -c unlock -c 'create hidden'" A H
for n in root:60 home:30 hidden:30; do $P/sbin/scubed3ctl -q -YYY -c "resize ${n%:*} ${n#*:}" > /dev/null; done
typed "$c -c unlock -c 'luksformat hidden' -c unlock -c 'luksformat root' -c 'luksformat home' -c 'mke2fs root' -c 'mount root $FS'" \
	H yes yes "" "" A yes yes "" "" yes yes "" "" yes yes
mountpoint -q $FS || fail "the LUKS volume is not mounted: $(cat $P/out)"
echo "the root file system on LUKS" > $FS/marker
typed "$P/sbin/scubed3ctl -c 'umount root'"
for n in root home hidden; do $P/sbin/scubed3ctl -c "close $n" > /dev/null 2>&1; done
$FUSERMOUNT -u /mnt/s3; sleep 1
cp $KS $P/keys.saved

# the boot: a wrong passphrase, H (a view without root), A
printf 'wrong\nH\nA\n' | sh $P/local-top > $P/out 2>&1
cp $P/out $P/boot.out
grep -q PANIC $P/out && fail "panic"
grep -q "ROOT=/dev/mapper/myhost-root" $P/conf/param.conf || fail "ROOT is not set: $(cat $P/conf/param.conf 2>&1)"
echo "   ok: ROOT=/dev/mapper/myhost-root (opened with the key from the store)"
expect="Please unlock disk base_crypt: cryptsetup: ERROR: base_crypt: cryptsetup failed, bad password or options?
Please unlock disk base_crypt: cryptsetup: ERROR: base_crypt: cryptsetup failed, bad password or options?
Please unlock disk base_crypt: cryptsetup: base_crypt: set up successfully"
[ "$(cat $P/out)" = "$expect" ] || fail "the console is not as with cryptroot:
$(cat $P/out)
---- expected:
$expect"
echo "   ok: the console shows only what cryptroot shows (prompt, wrong passphrase, set up successfully)"
[ -e $KS ] && fail "the key store is not removed from the initramfs"
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
for n in root home hidden; do
	echo "$st" | grep -q " blocks in $n" || fail "$n is not open: $st"
done
[ -b /dev/mapper/myhost-home ] || fail "the LUKS volume of home is not open"
[ -e /dev/mapper/myhost-hidden ] && fail "the LUKS volume of hidden is open"
echo "   ok: root, home and hidden are open, the LUKS volumes of root and home (not of hidden), level 2"
mount -o ro /dev/mapper/myhost-root $FS || fail "mount of /dev/mapper/myhost-root"
grep -q "the root file system on LUKS" $FS/marker || fail "the file system on LUKS"
echo "   ok: /dev/mapper/myhost-root has the file system"
umount $FS
unboot

# a second boot: three wrong passphrases
cp $P/keys.saved $KS
echo CRYPTTAB_NAME=sda5_crypt >> $P/conf/scubed3.conf
printf 'x\ny\nz\n' | sh $P/local-top > $P/out 2>&1
grep -q PANIC $P/out && fail "panic: $(cat $P/out)"
[ "$(grep -c 'Please unlock disk sda5_crypt: ' $P/out)" = 3 ] || fail "the prompt: $(cat $P/out)"
tail -n 1 $P/out | grep -q "^cryptsetup: ERROR: sda5_crypt: maximum number of tries exceeded$" ||
	fail "no message of the maximum number of tries: $(cat $P/out)"
grep -q ROOT $P/conf/param.conf 2> /dev/null && fail "ROOT is set"
$P/sbin/scubed3ctl -c p | grep -q " blocks in " && fail "a partition is open"
echo "   ok: three wrong passphrases: as cryptroot (maximum number of tries exceeded), no ROOT, nothing open"
cleanup
echo "INITRAMFSTEST OK"
