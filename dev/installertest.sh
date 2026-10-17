#!/bin/sh
# installertest.sh - partman-scubed3 (contrib/debian-installer) in a
# simulated debian-installer: the scripts run with their paths pointing into
# a temporary directory, with stand-ins for debconf (the answers come from
# a list), for the library of partman (partition lists are files), for
# apt-install, in-target and log-output, and
# fake-cryptsetup (no device mapper); the daemon and scubed3ctl are real,
# the base device is a loop device (200 MiB, 50 blocks of 4 MiB)
#
# 1. Configure scubed3: an existing store (its salt; it has the partition
#    "old" of an earlier installation, with its own passphrase and LUKS), a
#    key store with the boot passphrase; "root" (60 MB, LUKS, the boot
#    passphrase), "hidden" (20 MB, a hidden passphrase) and "home" (20 MB,
#    LUKS, the boot passphrase) are created, after "old" is opened (its
#    passphrase, a wrong and the right LUKS passphrase, the boot
#    passphrase: its keys and a new LUKS key go into the key store),
#    finish: level 2, the unclaimed blocks are lent to root, LUKS on root
#    and home (keys in the key store); the sizes and the reserved blocks are
#    checked, the size of the file of root does not change by the lending;
#    the views of the key store; Configure scubed3 again: the passphrases of
#    the key store are asked again (filling a new store with random data is
#    not done here, see the README)
# 2. init.d: /dev/mapper/testhost-root (testhost is the hostname, the
#    prefix luks_prefix= in scubed3.conf) becomes a disk in partman with one
#    partition (format, mount point /)
# 3. check.d: no /boot -> error; with /boot -> ok; /boot on scubed3 -> error;
#    /home on scubed3 without / on scubed3 -> error; with it: a note
# (and: home is mounted at /home; at boot the initramfs opens its LUKS
# volume, it is in the view of the boot passphrase; / on a partition that is
# not root -> error)
# 4. a file system on the LUKS volume is "installed" (a marker file)
# 5. finish-install: no package scubed3 (apt-install fails): the programs
#    and the initramfs-tools scripts are copied into the target,
#    /etc/scubed3.conf, /etc/scubed3.keys, /etc/scubed3/initramfs.conf
#    (BASE, OPTIONS), CRYPTSETUP=y, update-initramfs in the target, no
#    /etc/crypttab; the lent blocks are given back (root: 20 blocks,
#    pending removals)
# 6. the boot of the installed system: the local-top script of the
#    initramfs (as initramfstest.sh) with the configuration and the key
#    store that the installer wrote: the hidden passphrase, then the boot
#    passphrase; the root partition (on LUKS) has the marker, home is open
#    (LUKS) with its marker, hidden is open, writes drain the pending
#    removals (the blocks are given back)
#
# usage: installertest.sh DAEMON CTL (as root, uses /dev/scubed3fs and
# /run/scubed3/control, like the installer)
D=$(realpath $1); C=$(realpath $2); T=$(mktemp -d)
S=$(cd "$(dirname "$0")/.." && pwd); P=$S/contrib/debian-installer/partman-scubed3
FUSERMOUNT=$(command -v fusermount3 || echo fusermount)
MNT=/dev/scubed3fs; LOOP=
cleanup() {
	umount $T/target/home 2> /dev/null; umount $T/target 2> /dev/null; umount $T/fs 2> /dev/null
	for m in /dev/mapper/testhost-* /dev/mapper/scubed3-*; do
		[ -e "$m" ] && PATH=$T/bin:$PATH cryptsetup close ${m#/dev/mapper/}
	done
	$C -q -YYY -C $T/state/scubed3.conf -c "paranoia 1" > /dev/null 2>&1
	for n in root hidden home; do $C -C $T/state/scubed3.conf -c "close $n" > /dev/null 2>&1; done
	$FUSERMOUNT -u $MNT 2> /dev/null; sleep 1; rmdir $MNT 2> /dev/null
	[ -n "$LOOP" ] && losetup -d $LOOP
	rm -f /run/scubed3/fs
	rm -rf $T
}
fail() { echo "INSTALLERTEST FAILED: $1"; [ -f $T/log ] && tail -20 $T/log; [ -f $T/do_option.out ] && tail -20 $T/do_option.out; [ -f $T/asked ] && tail -8 $T/asked; cleanup; exit 1; }
ok() { echo "   ok: $1"; }
[ -e /run/scubed3/control ] && fail "a scubed3 daemon is running"

mkdir -p $T/bin $T/lib $T/devices $T/ans $T/db $T/target $T/fs $T/state

# ------------------------------------------------------------ stand-ins
ln -s $D $T/bin/scubed3
# scubed3ctl: its arguments are recorded (the KDF of a passphrase of the
# key store is computed once: keys-derive, the other runs get the keys, -k)
printf '#!/bin/sh\necho "$*" >> %s/ctl.calls\nexec %s "$@"\n' $T $C > $T/bin/scubed3ctl
chmod +x $T/bin/scubed3ctl
ln -s $S/dev/fake-cryptsetup $T/bin/cryptsetup
cat > $T/bin/log-output <<EOF
#!/bin/sh
[ "\$1" = -t ] && shift 2
echo "+ \$*" >> $T/log
"\$@" >> $T/log 2>&1
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
# modprobe: dm-crypt gives the device mapper the target crypt (dmsetup
# targets), not with $T/no_dm_crypt (a module that cannot be loaded)
cat > $T/bin/modprobe <<EOF
#!/bin/sh
echo "\$*" >> $T/modprobe
[ "\$2" = dm-crypt ] || exit 0
[ -f $T/no_dm_crypt ] && exit 1
>$T/dm_crypt
EOF
cat > $T/bin/dmsetup <<EOF
#!/bin/sh
[ "\$1" = targets ] || exit 1
[ -f $T/dm_crypt ] && echo "crypt            v1.27.0"
echo "linear           v1.4.0"
EOF
printf '#!/bin/sh\necho depmod >> %s/modprobe\n' $T > $T/bin/depmod
chmod +x $T/bin/*

# debconf: db_input takes the next answer of the template from $T/ans (if
# there is one), db_get gives the value or the default of the template; as
# with cdebconf every command sets RET (db_set: "value set", ...)
cat > $T/confmodule <<EOF
TEMPLATES=$P/templates
key() { echo "\$1" | tr / _; }
db_input() {
	local f=$T/ans/\$(key \$2)
	echo "\$2" >> $T/asked
	if [ -s \$f ]; then
		head -n1 \$f > $T/db/\$(key \$2); sed -i 1d \$f
	fi
	RET="question will be asked"
	return 0
}
db_go() { RET=ok; return 0; }
db_set() { printf '%s\n' "\$2" > $T/db/\$(key \$1); RET="value set"; }
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
db_subst() { printf "%s %s=%s\\n" "\$1" "\$2" "\$3" >> $T/subst; RET=; }
db_fset() { RET=\$3; }
db_fget() { RET=false; }
db_reset() { :; }
db_register() { :; }
db_capb() { :; }
db_x_loadtemplatefile() { echo "loadtemplatefile \$*" >> $T/asked; }
db_progress() { [ "\$1" = INFO ] && echo "progress \$2" >> $T/asked; :; }
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
# without cryptsetup (a udeb installed by hand gets no dependencies): it is
# anna-installed, if that fails an error
mv $T/bin/cryptsetup $T/bin/cryptsetup.off
if ! command -v cryptsetup > /dev/null; then
	printf '#!/bin/sh\necho "$*" >> %s/anna\nexit 1\n' $T > $T/bin/anna-install
	chmod +x $T/bin/anna-install
	: > $T/asked
	sh $T/do_option > /dev/null 2>&1
	grep -q '^cryptsetup-udeb$' $T/anna || fail "cryptsetup-udeb is not anna-installed"
	grep -q no_cryptsetup $T/asked || fail "no error without cryptsetup: $(cat $T/asked)"
	ok "without cryptsetup: anna-install cryptsetup-udeb, then an error"
fi
mv $T/bin/cryptsetup.off $T/bin/cryptsetup
# dm-crypt cannot be loaded: depmod, an error
>$T/no_dm_crypt; rm -f $T/dm_crypt
: > $T/asked; : > $T/modprobe
sh $T/do_option > /dev/null 2>&1
grep -q no_cryptsetup $T/asked || fail "no error without dm-crypt: $(cat $T/asked)"
grep -q '^-q dm-crypt$' $T/modprobe && grep -q '^depmod$' $T/modprobe ||
	fail "dm-crypt is not loaded: $(cat $T/modprobe)"
rm -f $T/no_dm_crypt
ok "without the target crypt: modprobe dm-crypt, depmod, then an error"

# an earlier installation: the partition old, with its own passphrase and
# LUKS
printf 'salt=oldsalt\niterations=1000\n' > $T/old.conf
mkdir -p $MNT; $D -b $LOOP -m 14 -M 22 $MNT 2> /dev/null || fail "the daemon (old)"
printf 'oldpass1\noldpass1\n' | $C -q -YYY -C $T/old.conf -c "create old" > /dev/null 2>&1
$C -q -YYY -C $T/old.conf -c "resize old 3" > /dev/null 2>&1
printf 'oldluks1' | cryptsetup -q luksFormat --key-file=- $MNT/old || fail "LUKS on old"
$C -q -YYY -C $T/old.conf -c "close old" > /dev/null 2>&1
$FUSERMOUNT -u $MNT; sleep 1

echo "1. Configure scubed3"
# the hostname (netcfg asks it before partitioning): the LUKS volumes are
# /dev/mapper/testhost-NAME
echo testhost > $T/db/netcfg_get_hostname
ans partman-scubed3/store_type existing
ans partman-scubed3/salt oldsalt
ans partman-scubed3/iterations 1000
ans partman-scubed3/mainmenu open create create create layout finish
ans partman-scubed3/name old root hidden home
ans partman-scubed3/size 60M 20M 20M
ans partman-scubed3/luks true false true
ans partman-scubed3/view boot hidden boot
# (the hidden passphrase of the key store: first empty, then twice a
# different one; root gets a passphrase and its LUKS volume one (first a
# passphrase of 64 characters, too long), hidden (no LUKS) and home random
# keys)
long=$(printf '%064d' 0)
ans partman-scubed3/passphrase bootpass1 oldpass1 wrongluks oldluks1 rootpass1 $long luksroot1 "" hiddenpw1 hiddenpw1 "" "" ""
ans partman-scubed3/passphrase-again bootpass1 rootpass1 $long luksroot1 "" hiddenpwX hiddenpw1 "" "" ""
ans partman-scubed3/level 2
ans partman-scubed3/boot_level 2
sh $T/do_option > $T/do_option.out 2>&1 || fail "do_option: $(cat $T/do_option.out)"
[ -S /run/scubed3/control ] || fail "the daemon does not run"
[ "$(cat $T/state/options)" = "-m 14 -M 22" ] || fail "options: $(cat $T/state/options)"
grep -q "^salt=oldsalt$" $T/state/scubed3.conf || fail "the salt"
grep -q "^luks_prefix=testhost-$" $T/state/scubed3.conf || fail "luks_prefix: $(cat $T/state/scubed3.conf)"
[ "$(cat $T/locked)" = $LOOP ] || fail "the base device is not locked"
ok "the daemon runs on $LOOP with $(cat $T/state/options), the salt of the store"
grep -q backup_note $T/asked || fail "no note about a backup of the key store"
[ "$(grep -c '^partman-scubed3/passphrase-empty$' $T/asked)" = 1 ] &&
	[ "$(grep -c '^partman-scubed3/passphrase-mismatch$' $T/asked)" = 1 ] ||
	fail "an empty or a different passphrase: $(grep -e -empty -e -mismatch $T/asked)"
grep -q generated_passphrase $T/asked $T/subst && fail "an autogenerated passphrase"
grep -q '^partman-scubed3/layout$' $T/asked || fail "the layout is not shown"
layout=$(sed -n 's/^partman-scubed3\/layout LAYOUT=//p' $T/subst)
for l in "^$LOOP: 50 blocks of 4 MiB, paranoia level 1" "^root: [0-9]* MiB (20 blocks), view 1 (boot passphrase), LUKS at finish" \
		"^hidden: [0-9]* MiB (7 blocks), view 2 (hidden passphrase), no LUKS" "^old: .* view 1 .*, LUKS$" \
		"^unclaimed: [0-9]* blocks" "^key store: view 1 in slot [0-9]*, view 2 in slot [0-9]* of 64 slots"; do
	printf '%s\n' "$layout" | sed 's/\\n/\n/g' | grep -q "$l" || fail "the layout ($l): $layout"
done
ok "the layout: the base device, the partitions (size, view, LUKS), the unclaimed blocks, the slots of the views"
[ "$(grep -c -- '-c keys-derive' $T/ctl.calls)" = 2 ] ||
	fail "keys-derive is not run once per passphrase: $(grep -- keys-derive $T/ctl.calls)"
grep -- '-K' $T/ctl.calls | grep -v -- ' -c keys-derive\| -c keys-create$' | grep -v -- ' -k ' &&
	fail "scubed3ctl runs with the key store without -k"
grep -q 'progress partman-scubed3/progress/derive' $T/asked &&
	grep -q 'progress partman-scubed3/progress/open' $T/asked &&
	grep -q 'progress partman-scubed3/progress/luks$' $T/asked || fail "no message while the KDF or LUKS runs"
ok "the KDF of a passphrase of the key store is computed once (keys-derive, then -k), a message is shown meanwhile"
hiddenpw1=hiddenpw1
ok "a note about a backup; an empty and a different hidden passphrase are refused (not autogenerated), equal ones are taken"
ctl() { $C -q -YYY -C $T/state/scubed3.conf "$@" 2> /dev/null; }
info() { ctl -c "info $1" | sed -n "s/^$2=//p"; }
ctl -c p | grep -q "paranoia level 2 " || fail "level: $(ctl -c p)"
[ "$(info hidden no_macroblocks)" = 7 ] && [ "$(info home no_macroblocks)" = 7 ] ||
	fail "hidden: $(info hidden no_macroblocks) blocks, home: $(info home no_macroblocks)"
# root: 60 MB = 57 MiB, 15 data blocks of 4080 KiB: 20 blocks (5 reserved),
# 13 lent (50 - 20 - 7 - 7 - 3 of old): 33 blocks, 18 reserved
[ "$(info root no_macroblocks)" = 33 ] && [ "$(info root reserved_macroblocks)" = 18 ] ||
	fail "root: $(info root no_macroblocks) blocks, $(info root reserved_macroblocks) reserved"
[ "$(cat $T/state/parts/root/lent)" = "20 5" ] || fail "lent: $(cat $T/state/parts/root/lent)"
[ "$(stat -c %s $MNT/root)" = $((15*4080*1024)) ] || fail "the size of root: $(stat -c %s $MNT/root)"
ok "hidden and home: 7 blocks; root: 20 blocks + 13 lent (as reserved blocks, the size of its file is the same)"
cryptsetup isLuks $MNT/root && [ -b /dev/mapper/testhost-root ] || fail "LUKS on root"
cryptsetup isLuks $MNT/home && [ -b /dev/mapper/testhost-home ] || fail "LUKS on home"
cryptsetup isLuks $MNT/hidden && fail "LUKS on hidden"
ok "LUKS on root and home (open as /dev/mapper/testhost-NAME), not on hidden"
K=$T/state/scubed3.keys
keys() { printf '%s\n' "$@" "" | $C -q -YYY -C $T/state/scubed3.conf -K $K -c unlock-all -c keys 2> /dev/null; }
keys bootpass1 > $T/keys.out
grep -q "^  root \[LUKS, passphrase\]" $T/keys.out && grep -q "^  home \[LUKS\]" $T/keys.out &&
	grep -q "^  old \[LUKS, passphrase\]" $T/keys.out &&
	! grep -q hidden $T/keys.out || fail "the view of the boot passphrase: $(cat $T/keys.out)"
keys "$hiddenpw1" > $T/keys.out
grep -q "^  hidden" $T/keys.out && ! grep -q root $T/keys.out || fail "the hidden view: $(cat $T/keys.out)"
[ "$(cat $T/state/views)" = 2 ] || fail "views: $(cat $T/state/views)"
grep -q luks_open_failed $T/asked || fail "the wrong LUKS passphrase of old was accepted"
[ -b /dev/mapper/testhost-old ] || fail "the LUKS volume of old is not open (with its new key)"
ok "the key store: the boot passphrase has root, home and old (a new LUKS key, a wrong LUKS passphrase was refused), the hidden passphrase hidden"
# Configure scubed3 again: a wrong passphrase, then both
ans partman-scubed3/passphrase wrong bootpass1 "$hiddenpw1"
ans partman-scubed3/mainmenu finish
cp $T/asked $T/asked.first; : > $T/asked
: > $T/ctl.calls
sh $T/do_option > $T/do_option.out 2>&1 || fail "do_option again: $(cat $T/do_option.out)"
[ "$(grep -c -- '-c keys-derive' $T/ctl.calls)" = 3 ] || fail "keys-derive again: $(grep -- keys-derive $T/ctl.calls)"
grep -q wrong_passphrase $T/asked || fail "a wrong passphrase was accepted: $(cat $T/asked)"
[ "$(keys bootpass1 "$hiddenpw1" | grep -o 'view [0-9]*:' | sort -u | wc -l)" = 2 ] || fail "a view is lost"
ok "Configure scubed3 again: the passphrases are asked again (a wrong one again), no view is lost"

echo "2. init.d"
sh $T/init.d-scubed3 || fail "init.d"
R=$T/devices/=dev=mapper=testhost-root
[ -f $R/partitions ] && [ "$(cat $R/p1/method)" = format ] && [ "$(cat $R/p1/mountpoint)" = / ] ||
	fail "partman disk: $(ls -R $R)"
H=$T/devices/=dev=mapper=testhost-home
[ -f $H/partitions ] && [ "$(cat $H/p1/method)" = format ] && [ ! -f $H/p1/mountpoint ] ||
	fail "partman disk of home: $(ls -R $H)"
ok "/dev/mapper/testhost-root and -home are disks in partman, one partition: format, root mount point /"

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
echo / > $H/p1/mountpoint
echo /srv > $R/p1/mountpoint
sh $T/check > /dev/null && fail "/ on scubed3 not on root is accepted"
tail -n1 $T/asked | grep -q root_not_root || fail "no root_not_root error"
echo /home > $H/p1/mountpoint
echo / > $R/p1/mountpoint
sh $T/check || fail "check.d with /home: $(tail -n1 $T/asked)"
tail -n1 $T/asked | grep -q others_note || fail "no note about /home"
ok "/ on scubed3 needs /boot (not on scubed3) and the partition root, /home on scubed3 needs / on scubed3, a note"

echo "4. the installation"
mke2fs -q -F /dev/mapper/testhost-root || fail "mke2fs"
mount /dev/mapper/testhost-root $T/target || fail "mount"
echo "the installed system" > $T/target/marker
# the packages: 40 MB, written twice (the blocks get live and dead data)
for r in 1 2; do
	for i in 1 2 3 4 5 6 7 8 9 10; do
		head -c $((4<<20)) /dev/urandom > $T/target/pkg$i; sync
	done
done
mkdir -p $T/target/etc $T/target/home
mke2fs -q -F /dev/mapper/testhost-home || fail "mke2fs home"
mount /dev/mapper/testhost-home $T/target/home || fail "mount home"
echo "the home of the installed system" > $T/target/home/marker
printf '%s\n' "/dev/mapper/testhost-root $T/target ext2 rw 0 0" \
	"/dev/mapper/testhost-home $T/target/home ext2 rw 0 0" > $T/mounts
# /etc/fstab as partman-target writes it
printf '%s\n' "/dev/mapper/testhost-root /               ext2    errors=remount-ro 0       1" \
	"/dev/mapper/testhost-home /home           ext2    defaults        0       2" \
	"UUID=1234 /boot           ext2    defaults        0       2" > $T/target/etc/fstab
ok "file systems on /dev/mapper/testhost-root and -home, mounted as the target, 80 MB written"

echo "5. finish-install"
sh $T/finish-install > $T/finish.out 2>&1 || fail "finish-install: $(cat $T/finish.out)"
for f in usr/local/sbin/scubed3 usr/local/sbin/scubed3ctl etc/initramfs-tools/hooks/scubed3 \
		etc/initramfs-tools/scripts/local-top/scubed3 etc/scubed3.conf \
		etc/scubed3.keys etc/scubed3/initramfs.conf; do
	[ -e $T/target/$f ] || fail "$f is not in the target"
done
cmp -s $T/state/scubed3.conf $T/target/etc/scubed3.conf || fail "/etc/scubed3.conf"
cmp -s $T/state/scubed3.keys $T/target/etc/scubed3.keys && [ "$(stat -c %a $T/target/etc/scubed3.keys)" = 600 ] ||
	fail "/etc/scubed3.keys"
grep -q "^BASE=$LOOP\$" $T/target/etc/scubed3/initramfs.conf &&
	grep -q '^OPTIONS="-m 14 -M 22"$' $T/target/etc/scubed3/initramfs.conf &&
	! grep -q "^ROOT_PARTITION" $T/target/etc/scubed3/initramfs.conf ||
	fail "initramfs.conf: $(cat $T/target/etc/scubed3/initramfs.conf)"
grep -qx "CRYPTSETUP=y" $T/target/etc/cryptsetup-initramfs/conf-hook || fail "conf-hook"
grep -qx "scubed3" $T/apt-install && grep -qx "fuse3" $T/apt-install &&
	grep -qx "cryptsetup cryptsetup-initramfs" $T/apt-install || fail "apt-install: $(cat $T/apt-install)"
grep -qx "update-initramfs -u -k all" $T/in-target || fail "update-initramfs"
[ -e $T/target/etc/crypttab ] && fail "crypttab: $(cat $T/target/etc/crypttab)"
ok "scubed3 (copied: no package), its configuration, the key store, CRYPTSETUP=y, update-initramfs, no crypttab"
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
for n in root home old; do cryptsetup close testhost-$n; done
for n in root hidden home old; do ctl -c "close $n" > /dev/null; done
[ -e /dev/mapper/scubed3-check-home ] && fail "a check of a LUKS passphrase is left open"
ctl -c "paranoia 1" > /dev/null
$FUSERMOUNT -u $MNT; sleep 1
# the initramfs, as initramfstest.sh, with the files that the installer wrote
I=$T/initramfs
mkdir -p $I/scripts $I/conf $I/sbin $I/etc $I/run
echo 'panic() { echo "PANIC: $*"; exit 1; }' > $I/scripts/functions
# the files of the installed system (as the hook copies them)
cp $T/target-etc/scubed3.conf $I/etc/scubed3.conf
cp $T/target-etc/scubed3/initramfs.conf $I/conf/scubed3.conf
cp $T/target-etc/scubed3.keys $I/conf/scubed3.keys
cp $D "$I/sbin/@scubed3"
printf '#!/bin/sh\nPATH=%s/sbin:$PATH exec %s -C %s/etc/scubed3.conf "$@"\n' $I $C $I > $I/sbin/scubed3ctl
chmod +x $I/sbin/scubed3ctl
ln -s "$(command -v losetup)" $I/sbin/losetup-util
ln -s $S/dev/fake-cryptsetup $I/sbin/cryptsetup
sed -e "s|/scripts/functions|$I/scripts/functions|" -e "s|/conf/|$I/conf/|g" \
	-e "s|/sbin/|$I/sbin/|g" -e "s|PATH=/sbin:|PATH=$I/sbin:|" \
	-e "s|/etc/scubed3.conf|$I/etc/scubed3.conf|g" \
	-e "s|/lib/cryptsetup/askpass|$I/askpass|" -e "s|/run/initramfs/|$I/run/|" \
	$T/target-etc/initramfs-tools/scripts/local-top/scubed3 > $I/local-top
printf '%s\nbootpass1\n' "$hiddenpw1" | sh $I/local-top > $T/boot.out 2>&1
grep -q PANIC $T/boot.out && fail "boot: $(cat $T/boot.out)"
grep -q "ROOT=/dev/mapper/testhost-root" $I/conf/param.conf || fail "ROOT: $(cat $T/boot.out)"
mount /dev/mapper/testhost-root $T/target || fail "mount of the root file system"
grep -q "the installed system" $T/target/marker || fail "the marker"
grep -q "set up successfully" $T/boot.out || fail "boot: $(cat $T/boot.out)"
ok "the installed system boots (the hidden passphrase, the boot passphrase): ROOT=/dev/mapper/testhost-root, the marker is there"
# the mount of /etc/fstab
[ -b /dev/mapper/testhost-home ] || fail "the LUKS volume of home is not open"
mount /dev/mapper/testhost-home $T/target/home || fail "mount of home"
grep -q "the home of the installed system" $T/target/home/marker || fail "the marker of home"
ctl -c p | grep -q " blocks in hidden" || fail "hidden is not open"
[ -b /dev/mapper/testhost-old ] || fail "the LUKS volume of old is not open"
ok "the initramfs opened home and old (LUKS, the view of root) and hidden, the marker of home is there"
ctl -c p | grep -q "paranoia level 2 " || fail "level after boot: $(ctl -c p)"
rm -f $T/target/pkg*; sync
for i in 1 2 3 4 5 6 7 8; do
	head -c $((3<<20)) /dev/urandom > $T/target/f$i; sync
done
after=$(info root pending_removals)
[ "$after" = 0 ] || [ "$after" -lt "$pending" ] ||
	fail "the removals are not drained ($pending -> $after)"
ok "level 2 after the boot, writes drain the removals ($pending -> $after pending)"
# the passphrases of root: its LUKS volume has the passphrase as its one
# key, home and old one key too (random, the passphrase of old); root opens
# with its passphrase without the key store
lh() { dd if=$MNT/$1 bs=1 skip=8 count=512 2> /dev/null | tr -d '\0' | fold -w 64; }
[ "$(lh root)" = "$(printf luksroot1 | sha256sum | cut -c1-64)" ] || fail "the LUKS key of root is not its passphrase"
[ "$(lh old)" = "$(printf oldluks1 | sha256sum | cut -c1-64)" ] || fail "the LUKS volume of old has another key"
[ "$(lh home | grep -c .)" = 1 ] || fail "LUKS keys of home: $(lh home | grep -c .)"
grep -q luks_passphrase_long $T/asked.first || fail "a LUKS passphrase of 64 characters is accepted"
umount $T/target/home; umount $T/target
cryptsetup close testhost-root
ctl -c "close root" > /dev/null
printf 'rootpass1\n' | ctl -c "open root" > /dev/null
[ "$(info root no_macroblocks)" = 20 ] || fail "root does not open with its passphrase"
ok "root opens with its passphrase (without the key store), its LUKS volume has its passphrase as its one key (64 characters refused); home a random key, old keeps its passphrase"
cleanup
echo "INSTALLERTEST OK"
