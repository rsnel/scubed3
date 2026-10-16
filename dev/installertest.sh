#!/bin/sh
# installertest.sh - partman-scubed3 (contrib/debian-installer) in a
# simulated debian-installer: the scripts run with their paths pointing into
# a temporary directory, with stand-ins for debconf (the answers come from
# a list), for the library of partman (partition lists are files), for
# blockdev-keygen, apt-install, in-target and log-output, and
# fake-cryptsetup (no device mapper); the daemon and scubed3ctl are real,
# the base device is a loop device (200 MiB, 50 blocks of 4 MiB)
#
# 1. Configure scubed3: a new store (filled with random data),
#    "root" (60 MB, LUKS) and "hidden" (20 MB) are created, finish: level
#    2, the unclaimed blocks are lent to root, LUKS on root; the sizes and
#    the reserved blocks are checked, the size of the file of root does not
#    change by the lending
# 2. init.d: /dev/mapper/scubed3-root becomes a disk in partman with one
#    partition (format, mount point /)
# 3. check.d: no /boot -> error; with /boot -> ok; /boot on scubed3 -> error;
#    /home on scubed3 without / on scubed3 -> error; with it: a note
# (and: home, 20 MB with LUKS, is mounted at /home; finish-install writes
# /etc/crypttab and nofail in /etc/fstab; at boot it is opened from there)
# 4. a file system on the LUKS volume is "installed" (a marker file)
# 5. finish-install: no package scubed3 (apt-install fails): the programs
#    and the initramfs-tools scripts are copied into the target,
#    /etc/scubed3.conf, /etc/scubed3/initramfs.conf (BASE, OPTIONS),
#    CRYPTSETUP=y, update-initramfs in the target; the lent blocks are given
#    back (root: 20 blocks, pending removals)
# 6. the boot of the installed system: the local-top script of the
#    initramfs (as initramfstest.sh) with the configuration that the
#    installer wrote, the root partition (on LUKS) has the marker, writes
#    drain the pending removals (the blocks are given back)
#
# usage: installertest.sh DAEMON CTL (as root, uses /dev/scubed3fs and
# /run/scubed3/control, like the installer)
D=$(realpath $1); C=$(realpath $2); T=$(mktemp -d)
S=$(cd "$(dirname "$0")/.." && pwd); P=$S/contrib/debian-installer/partman-scubed3
FUSERMOUNT=$(command -v fusermount3 || echo fusermount)
MNT=/dev/scubed3fs; LOOP=
cleanup() {
	umount $T/target/home 2> /dev/null; umount $T/target 2> /dev/null; umount $T/fs 2> /dev/null
	for m in /dev/mapper/scubed3-*; do
		[ -e "$m" ] && PATH=$T/bin:$PATH cryptsetup close ${m#/dev/mapper/}
	done
	$C -q -YYY -C $T/state/scubed3.conf -c "paranoia 1" > /dev/null 2>&1
	for n in root hidden home; do $C -C $T/state/scubed3.conf -c "close $n" > /dev/null 2>&1; done
	$FUSERMOUNT -u $MNT 2> /dev/null; sleep 1; rmdir $MNT 2> /dev/null
	[ -n "$LOOP" ] && losetup -d $LOOP
	rm -f /run/scubed3/fs
	rm -rf $T
}
fail() { echo "INSTALLERTEST FAILED: $1"; [ -f $T/log ] && tail -20 $T/log; cleanup; exit 1; }
ok() { echo "   ok: $1"; }
[ -e /run/scubed3/control ] && fail "a scubed3 daemon is running"

mkdir -p $T/bin $T/lib $T/devices $T/ans $T/db $T/target $T/fs $T/state

# ------------------------------------------------------------ stand-ins
ln -s $D $T/bin/scubed3
ln -s $C $T/bin/scubed3ctl
ln -s $S/dev/fake-cryptsetup $T/bin/cryptsetup
cat > $T/bin/log-output <<EOF
#!/bin/sh
[ "\$1" = -t ] && shift 2
echo "+ \$*" >> $T/log
"\$@" >> $T/log 2>&1
EOF
cat > $T/bin/blockdev-keygen <<EOF
#!/bin/sh
printf %s "\$(cat $T/lukspass)" > \$3
EOF
cat > $T/bin/apt-install <<EOF
#!/bin/sh
echo "\$*" >> $T/apt-install
[ "\$1" != scubed3 ]
EOF
cat > $T/bin/in-target <<EOF
#!/bin/sh
echo "\$*" >> $T/in-target
EOF
cat > $T/bin/modprobe <<EOF
#!/bin/sh
exit 0
EOF
chmod +x $T/bin/*
printf 'lukspass1' > $T/lukspass

# debconf: db_input takes the next answer of the template from $T/ans (if
# there is one), db_get gives the value or the default of the template
cat > $T/confmodule <<EOF
TEMPLATES=$P/templates
key() { echo "\$1" | tr / _; }
db_input() {
	local f=$T/ans/\$(key \$2)
	echo "\$2" >> $T/asked
	if [ -s \$f ]; then
		head -n1 \$f > $T/db/\$(key \$2); sed -i 1d \$f
	fi
	return 0
}
db_go() { return 0; }
db_set() { printf '%s\n' "\$2" > $T/db/\$(key \$1); }
db_get() {
	if [ -f $T/db/\$(key \$1) ]; then RET=\$(cat $T/db/\$(key \$1))
	else RET=\$(awk -v t="\$1" '\$1 == "Template:" { on = \$2 == t }
		on && \$1 == "Default:" { sub(/^Default: /, ""); print }' \$TEMPLATES)
	fi
}
db_metaget() {
	RET=\$(awk -v t="\$1" '\$1 == "Template:" { on = \$2 == t }
		on && \$1 == "Description:" { sub(/^Description: /, ""); print; exit }' \$TEMPLATES)
}
db_subst() { :; }
db_fset() { :; }
db_fget() { RET=false; }
db_reset() { :; }
db_register() { :; }
db_capb() { :; }
db_x_loadtemplatefile() { echo "loadtemplatefile \$*" >> $T/asked; }
db_progress() { :; }
EOF
# answers, one per line, per template
ans() { t=$(echo "$1" | tr / _); shift; for a in "$@"; do echo "$a" >> $T/ans/$t; done; }

# the library of partman: a device dir has a file "partitions" (lines: num
# id size type fs path name), open_dialog PARTITIONS reads it
cat > $T/lib/base.sh <<EOF
. $T/confmodule
DEVICES=$T/devices
NL="
"
TAB="	"
restore_ifs() { IFS="
"; }
log() { echo "partman: \$*" >> $T/log; }
open_dialog() {
	case \$1 in
	    PARTITIONS)	[ -f ./partitions ] && exec 9< ./partitions || exec 9< /dev/null ;;
	    NEW_LABEL)
		echo "-1 free1 \$(cat size) primary free \$(cat device) x" > ./partitions
		exec 9< /dev/null ;;
	    NEW_PARTITION)
		echo "1 p1 \$(cat size) primary \$3 \$(cat device) x" > ./partitions
		exec 9< ./partitions ;;
	    *)		exec 9< /dev/null ;;
	esac
}
read_line() { read "\$@" <&9 || true; }
close_dialog() { exec 9<&-; }
humandev() { echo "\$1"; }
mapdevfs() { echo "\$1"; }
partman_lock_unit() { echo "\$1" > $T/locked; }
update_partition() { :; }
stop_parted_server() { :; }
restart_partman() { $T/init.d-scubed3; }
valid_human() { expr "\$1" : '[0-9][0-9]* *[kKmMgGtT][bB]*\$' > /dev/null; }
human2longint() {
	local n=\$(echo "\$1" | tr -dc 0-9) s=\$(echo "\$1" | tr -dc kKmMgGtT)
	case \$s in
	    k|K) echo \${n}000 ;; m|M) echo \${n}000000 ;;
	    g|G) echo \${n}000000000 ;; t|T) echo \${n}000000000000 ;;
	    *) echo \${n}000000 ;;
	esac
}
EOF
cat > $T/lib/commit.sh <<EOF
confirm_changes() { echo confirm >> $T/asked; return 0; }
commit_changes() { return 0; }
EOF
cat > $T/lib/crypto-base.sh <<EOF
. $T/lib/base.sh
. $T/lib/commit.sh
EOF

# the scripts, with their paths in $T
fix() {
	sed -e "s|/lib/partman/lib/|$T/lib/|g" \
	    -e "s|/usr/share/debconf/confmodule|$T/confmodule|" \
	    -e "s|/var/lib/partman/scubed3|$T/state|" \
	    -e "s|S3DEFAULT_ITERATIONS=16777216|S3DEFAULT_ITERATIONS=1000|" \
	    -e "s|/bin/blockdev-keygen|$T/bin/blockdev-keygen|" \
	    -e "s|/proc/mounts|$T/mounts|" \
	    -e "s|/target|$T/target|g" \
	    -e "s|/usr/sbin/scubed3|$T/bin/scubed3|g" \
	    -e "s|SHARE=/usr/share/scubed3|SHARE=$T/share|" "$1" > "$2"
	chmod +x "$2"
}
fix $P/lib/scubed3-base.sh $T/lib/scubed3-base.sh
fix $P/choose_partition/scubed3/do_option $T/do_option
fix $P/choose_partition/scubed3/choices $T/choices
fix $P/init.d/scubed3 $T/init.d-scubed3
fix $P/check.d/scubed3_check $T/check
fix $P/finish-install.d/08scubed3 $T/finish-install
mkdir -p $T/share/initramfs-tools/hooks $T/share/initramfs-tools/scripts/local-top
cp $S/contrib/initramfs-tools/hooks/scubed3 $T/share/initramfs-tools/hooks/
cp $S/contrib/initramfs-tools/scripts/local-top/scubed3 \
	$T/share/initramfs-tools/scripts/local-top/
export PATH=$T/bin:$PATH

# the disk: one partition, the base device (a loop device, random data)
head -c $((200<<20)) /dev/urandom > $T/base
LOOP=$(losetup -f --show $T/base) || fail "losetup"
DISK=$T/devices/=dev=disk
mkdir -p $DISK/p1
echo /dev/disk > $DISK/device
echo "1 p1 209715200 primary unknown $LOOP base" > $DISK/partitions

sh $T/choices | grep -q "^scubed3	Configure scubed3" ||
	fail "Configure scubed3 is not offered without a base device"
sh $T/do_option > /dev/null 2>&1
tail -n1 $T/asked | grep -q no_base || fail "no no_base error without a base device"
echo scubed3 > $DISK/p1/method
sh $T/choices | grep -q "^scubed3	Configure scubed3" ||
	fail "Configure scubed3 is not offered"
grep -q loadtemplatefile $T/asked && fail "the templates are loaded again"
ok "\"Configure scubed3\" is always offered; without a base device it says how to make one"

echo "1. Configure scubed3"
ans partman-scubed3/store_type new
ans partman-scubed3/fill true
ans partman-scubed3/mainmenu create create create finish
ans partman-scubed3/name root hidden home
ans partman-scubed3/size 60M 20M 20M
ans partman-scubed3/luks true false true
ans partman-scubed3/passphrase rootpass1 hiddenpw1 homepass1
ans partman-scubed3/passphrase-again rootpass1 hiddenpw1 homepass1
ans partman-scubed3/level 2
ans partman-scubed3/boot_level 2
ans partman-scubed3/lend root
sh $T/do_option > $T/do_option.out 2>&1 || fail "do_option: $(cat $T/do_option.out)"
[ -S /run/scubed3/control ] || fail "the daemon does not run"
[ "$(cat $T/state/options)" = "-m 14 -M 22" ] || fail "options: $(cat $T/state/options)"
grep -q "^salt=..*" $T/state/scubed3.conf || fail "no salt"
[ "$(cat $T/locked)" = $LOOP ] || fail "the base device is not locked"
ok "the daemon runs on $LOOP with $(cat $T/state/options), a random salt"
ctl() { $C -q -YYY -C $T/state/scubed3.conf "$@" 2> /dev/null; }
info() { ctl -c "info $1" | sed -n "s/^$2=//p"; }
ctl -c p | grep -q "paranoia level 2 " || fail "level: $(ctl -c p)"
[ "$(info hidden no_macroblocks)" = 7 ] && [ "$(info home no_macroblocks)" = 7 ] ||
	fail "hidden: $(info hidden no_macroblocks) blocks, home: $(info home no_macroblocks)"
# root: 60 MB = 57 MiB, 15 data blocks of 4080 KiB: 20 blocks (5 reserved),
# 16 lent: 36 blocks, 21 reserved
[ "$(info root no_macroblocks)" = 36 ] && [ "$(info root reserved_macroblocks)" = 21 ] ||
	fail "root: $(info root no_macroblocks) blocks, $(info root reserved_macroblocks) reserved"
[ "$(cat $T/state/parts/root/lent)" = "20 5" ] || fail "lent: $(cat $T/state/parts/root/lent)"
[ "$(stat -c %s $MNT/root)" = $((15*4080*1024)) ] || fail "the size of root: $(stat -c %s $MNT/root)"
ok "hidden and home: 7 blocks; root: 20 blocks + 16 lent (as reserved blocks, the size of its file is the same)"
cryptsetup isLuks $MNT/root && [ -b /dev/mapper/scubed3-root ] || fail "LUKS on root"
cryptsetup isLuks $MNT/home && [ -b /dev/mapper/scubed3-home ] || fail "LUKS on home"
cryptsetup isLuks $MNT/hidden && fail "LUKS on hidden"
ok "LUKS on root and home (open as /dev/mapper/scubed3-NAME), not on hidden"

echo "2. init.d"
sh $T/init.d-scubed3 || fail "init.d"
R=$T/devices/=dev=mapper=scubed3-root
[ -f $R/partitions ] && [ "$(cat $R/p1/method)" = format ] && [ "$(cat $R/p1/mountpoint)" = / ] ||
	fail "partman disk: $(ls -R $R)"
H=$T/devices/=dev=mapper=scubed3-home
[ -f $H/partitions ] && [ "$(cat $H/p1/method)" = format ] && [ ! -f $H/p1/mountpoint ] ||
	fail "partman disk of home: $(ls -R $H)"
ok "/dev/mapper/scubed3-root and -home are disks in partman, one partition: format, root mount point /"

echo "3. check.d"
sh $T/check > /dev/null && fail "root on scubed3 without /boot is accepted"
tail -n1 $T/asked | grep -q root_needs_boot || fail "no root_needs_boot error"
echo "2 p2 1000000 primary ext2 /dev/disk2 boot" >> $DISK/partitions
mkdir -p $DISK/p2; echo format > $DISK/p2/method; echo /boot > $DISK/p2/mountpoint
sh $T/check || fail "check.d: $(tail -n1 $T/asked)"
echo /boot > $H/p1/mountpoint
sh $T/check > /dev/null && fail "/boot on scubed3 is accepted"
echo /home > $H/p1/mountpoint
echo /srv > $R/p1/mountpoint
sh $T/check > /dev/null && fail "/home on scubed3 without root on scubed3 is accepted"
tail -n1 $T/asked | grep -q others_need_root || fail "no others_need_root error"
echo / > $R/p1/mountpoint
sh $T/check || fail "check.d with /home: $(tail -n1 $T/asked)"
tail -n1 $T/asked | grep -q others_note || fail "no note about /home"
ok "/ on scubed3 needs /boot (not on scubed3), /home on scubed3 needs / on scubed3, a note"

echo "4. the installation"
mke2fs -q -F /dev/mapper/scubed3-root || fail "mke2fs"
mount /dev/mapper/scubed3-root $T/target || fail "mount"
echo "the installed system" > $T/target/marker
# the packages: 40 MB, written twice (the blocks get live and dead data)
for r in 1 2; do
	for i in 1 2 3 4 5 6 7 8 9 10; do
		head -c $((4<<20)) /dev/urandom > $T/target/pkg$i; sync
	done
done
mkdir -p $T/target/etc $T/target/home
mke2fs -q -F /dev/mapper/scubed3-home || fail "mke2fs home"
mount /dev/mapper/scubed3-home $T/target/home || fail "mount home"
echo "the home of the installed system" > $T/target/home/marker
printf '%s\n' "/dev/mapper/scubed3-root $T/target ext2 rw 0 0" \
	"/dev/mapper/scubed3-home $T/target/home ext2 rw 0 0" > $T/mounts
# /etc/fstab as partman-target writes it
printf '%s\n' "/dev/mapper/scubed3-root /               ext2    errors=remount-ro 0       1" \
	"/dev/mapper/scubed3-home /home           ext2    defaults        0       2" \
	"UUID=1234 /boot           ext2    defaults        0       2" > $T/target/etc/fstab
ok "file systems on /dev/mapper/scubed3-root and -home, mounted as the target, 80 MB written"

echo "5. finish-install"
ans partman-scubed3/store_root_name false
sh $T/finish-install > $T/finish.out 2>&1 || fail "finish-install: $(cat $T/finish.out)"
for f in usr/local/sbin/scubed3 usr/local/sbin/scubed3ctl etc/initramfs-tools/hooks/scubed3 \
		etc/initramfs-tools/scripts/local-top/scubed3 etc/scubed3.conf \
		etc/scubed3/initramfs.conf; do
	[ -e $T/target/$f ] || fail "$f is not in the target"
done
cmp -s $T/state/scubed3.conf $T/target/etc/scubed3.conf || fail "/etc/scubed3.conf"
grep -q "^BASE=$LOOP\$" $T/target/etc/scubed3/initramfs.conf &&
	grep -q '^OPTIONS="-m 14 -M 22"$' $T/target/etc/scubed3/initramfs.conf &&
	! grep -q "^ROOT_PARTITION" $T/target/etc/scubed3/initramfs.conf ||
	fail "initramfs.conf: $(cat $T/target/etc/scubed3/initramfs.conf)"
grep -qx "CRYPTSETUP=y" $T/target/etc/cryptsetup-initramfs/conf-hook || fail "conf-hook"
grep -qx "scubed3" $T/apt-install && grep -qx "fuse3" $T/apt-install &&
	grep -qx "cryptsetup cryptsetup-initramfs" $T/apt-install || fail "apt-install: $(cat $T/apt-install)"
grep -qx "update-initramfs -u -k all" $T/in-target || fail "update-initramfs"
[ "$(cat $T/target/etc/crypttab)" = "scubed3-home /run/scubed3/fs/home none luks,nofail" ] ||
	fail "crypttab: $(cat $T/target/etc/crypttab)"
grep -q "^/dev/mapper/scubed3-home /home  *ext2  *defaults,nofail  *0  *2\$" $T/target/etc/fstab &&
	grep -q "^/dev/mapper/scubed3-root /  *ext2  *errors=remount-ro  *0  *1\$" $T/target/etc/fstab ||
	fail "fstab: $(cat $T/target/etc/fstab)"
ok "/etc/crypttab opens home from /run/scubed3/fs/home, nofail (also in /etc/fstab)"
ok "scubed3 (copied: no package), its configuration, CRYPTSETUP=y, update-initramfs"
[ "$(info root no_macroblocks)" = 20 ] && [ "$(info root reserved_macroblocks)" = 5 ] ||
	fail "root after finish-install: $(info root no_macroblocks) blocks"
pending=$(info root pending_removals); ctl -c "info root" > $T/info.out; ctl -c p >> $T/info.out
[ "$(info root trims)" = 0 ] || fail "the give-back trims root: $(info root trims) trims"
# (blocks without live data are removed at once)
[ "$pending" = 0 ] || ctl -c p | grep -q "shrinking root" || fail "p does not show shrinking"
ok "the lent blocks are given back while root is in use: root has 20 blocks, $pending removals pending (no trim)"
ctl -c "resize-internal root 19 5" 2>&1 | grep -q "busy" ||
	fail "root (in use) can be made smaller"
[ "$(info root no_macroblocks)" = 20 ] || fail "root was resized"
ok "a partition in use cannot be made smaller"
cp -r $T/target/etc $T/target-etc
umount $T/target/home; umount $T/target

echo "6. the boot of the installed system"
cryptsetup close scubed3-root
cryptsetup close scubed3-home
for n in root hidden home; do ctl -c "close $n" > /dev/null; done
ctl -c "paranoia 1" > /dev/null
$FUSERMOUNT -u $MNT; sleep 1
# the initramfs, as initramfstest.sh, with the files that the installer wrote
I=$T/initramfs
mkdir -p $I/scripts $I/conf $I/sbin $I/etc
echo 'panic() { echo "PANIC: $*"; exit 1; }' > $I/scripts/functions
# the files of the installed system (as the hook copies them)
cp $T/target-etc/scubed3.conf $I/etc/scubed3.conf
cp $T/target-etc/scubed3/initramfs.conf $I/conf/scubed3.conf
cp $D "$I/sbin/@scubed3"
printf '#!/bin/sh\nPATH=%s/sbin:$PATH exec %s -C %s/etc/scubed3.conf "$@"\n' $I $C $I > $I/sbin/scubed3ctl
chmod +x $I/sbin/scubed3ctl
ln -s "$(command -v losetup)" $I/sbin/losetup-util
ln -s $S/dev/fake-cryptsetup $I/sbin/cryptsetup
sed -e "s|/scripts/functions|$I/scripts/functions|" -e "s|/conf/|$I/conf/|g" \
	-e "s|/sbin/|$I/sbin/|g" -e "s|PATH=/sbin:|PATH=$I/sbin:|" \
	-e "s|/etc/scubed3.conf|$I/etc/scubed3.conf|g" \
	$T/target-etc/initramfs-tools/scripts/local-top/scubed3 > $I/local-top
(for l in root rootpass1 hidden hiddenpw1 home homepass1 "" Yes lukspass1; do sleep 1; printf '%s\n' "$l"; done; sleep 3) |
	script -qc "sh $I/local-top" /dev/null | tr -d '\r' > $T/boot.out
grep -q PANIC $T/boot.out && fail "boot: $(cat $T/boot.out)"
grep -q "ROOT=/dev/mapper/scubed3-root" $I/conf/param.conf || fail "ROOT: $(cat $T/boot.out)"
mount /dev/mapper/scubed3-root $T/target || fail "mount of the root file system"
grep -q "the installed system" $T/target/marker || fail "the marker"
ok "the installed system boots: ROOT=/dev/mapper/scubed3-root, the marker is there"
# systemd-cryptsetup with /etc/crypttab of the installed system, then the
# mount of /etc/fstab
[ "$(readlink /run/scubed3/fs)" = $MNT ] || fail "/run/scubed3/fs: $(readlink /run/scubed3/fs)"
set -- $(cat $T/target-etc/crypttab)
printf 'lukspass1' | cryptsetup open --key-file=- $2 $1 || fail "crypttab: open $2"
mount /dev/mapper/scubed3-home $T/target/home || fail "mount of home"
grep -q "the home of the installed system" $T/target/home/marker || fail "the marker of home"
ok "home opens from /etc/crypttab (/run/scubed3/fs/home), its marker is there"
ctl -c p | grep -q "paranoia level 2 " || fail "level after boot: $(ctl -c p)"
rm -f $T/target/pkg*; sync
for i in 1 2 3 4 5 6 7 8; do
	head -c $((3<<20)) /dev/urandom > $T/target/f$i; sync
done
after=$(info root pending_removals)
[ "$after" = 0 ] || [ "$after" -lt "$pending" ] ||
	fail "the removals are not drained ($pending -> $after)"
ok "level 2 after the boot, writes drain the removals ($pending -> $after pending)"
cleanup
echo "INSTALLERTEST OK"
