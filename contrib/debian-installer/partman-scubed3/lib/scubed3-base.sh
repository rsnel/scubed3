# scubed3-base.sh - functions of partman-scubed3 (see ../README)
#
# the state of the installer is kept in $S3STATE:
#
#	base		the base device (a partition with method scubed3)
#	options		-m MESOBLOCK_LOG -M MACROBLOCK_LOG of the daemon
#	scubed3.conf	salt, iterations, paranoia (for the installed system)
#	level		the paranoia level during the installation (after
#			finish)
#	scubed3.keys	the key store (it goes to /etc/scubed3.keys): view
#			1 is the view of the boot passphrase (root and the
#			partitions that are opened at boot with it), views
#			2, ... of hidden passphrases (typed at boot before
#			the boot passphrase); the keys of the passphrases
#			(keys-derive: the KDF takes long, it is computed
#			once per passphrase) are only in the memory of
#			do_option (S3VIEWKEYS), not the passphrases
#	views		the number of views
#	parts/NAME/	a partition, opened or created by the installer:
#		view		the view of the key store it is in
#		luks		it has (or gets) LUKS on it
#		luks_open	LUKS is open as /dev/mapper/HOSTNAME-NAME (the
#				prefix HOSTNAME- is luks_prefix= in scubed3.conf)
#		lent		"BLOCKS RESERVED": its size before blocks were
#				lent to it, the blocks are given back at the
#				end of the installation (finish-install)
#
# the root file system goes on the partition "root" (view 1, with LUKS):
# the initramfs of the installed system boots from it
# the daemon runs as in the initramfs: mount point /dev/scubed3fs, control
# socket /run/scubed3/control

. /lib/partman/lib/base.sh

# the templates are loaded when the udeb is configured by anna; a udeb that
# is installed by hand (udpkg -i in a shell, see ../README) while the
# installer runs is not seen by the debconf of the installer (it keeps its
# database in memory): then the templates are loaded through it (a copy of
# them is in the udeb)
S3TEMPLATES=/usr/share/partman-scubed3/templates
if ! db_metaget partman-scubed3/text/configure description 2>/dev/null &&
		[ -f $S3TEMPLATES ]; then
	db_x_loadtemplatefile $S3TEMPLATES partman-scubed3 2>/dev/null || true
fi

S3STATE=/var/lib/partman/scubed3
S3MNT=/dev/scubed3fs
S3SOCK=/run/scubed3/control
S3CONF=$S3STATE/scubed3.conf
S3KEYS=$S3STATE/scubed3.keys
S3MACRO_LOG=22
S3DEFAULT_ITERATIONS=16777216

# scubed3ctl, without questions (-YYY: the questions are asked by debconf),
# its output is logged
s3ctl () {
	log-output -t partman-scubed3 scubed3ctl -q -YYY -C $S3CONF "$@"
}

# scubed3ctl, its output on stdout
s3ctl_out () {
	scubed3ctl -q -YYY -C $S3CONF "$@" 2>/dev/null
}

s3_running () {
	[ -S $S3SOCK ]
}

# the sizes of the blocks need 64 bit arithmetic
s3_math64 () {
	[ "$(( 1 << 40 ))" = 1099511627776 ]
}

# the partitions (their partman device dir and id) with method scubed3, one
# per line: DEVDIR<TAB>ID<TAB>PATH
s3_bases () {
	local dev num id size type fs path name
	for dev in $DEVICES/*; do
		[ -d "$dev" ] || continue
		cd $dev
		open_dialog PARTITIONS
		while { read_line num id size type fs path name; [ "$id" ]; }; do
			[ "$fs" != free ] || continue
			[ -f $id/method ] || continue
			[ "$(cat $id/method)" = scubed3 ] || continue
			printf "%s\t%s\t%s\n" "$dev" "$id" "$path"
		done
		close_dialog
	done
}

# the names of the LUKS volumes start with HOSTNAME- (as LVM names the
# volume group HOSTNAME-vg): luks_prefix= in scubed3.conf, for scubed3ctl;
# the hostname that was asked before partitioning
s3_hostname () {
	local h
	h=
	if db_get netcfg/get_hostname 2>/dev/null; then
		h=$RET
	fi
	[ "$h" ] || h=$(hostname 2>/dev/null)
	h=$(echo "$h" | sed 's/\..*//; s/[^A-Za-z0-9_-]//g' | cut -c1-40)
	echo "${h:-debian}"
}

s3_luks_prefix () {
	if [ -f $S3CONF ] && grep -q '^luks_prefix=' $S3CONF; then
		sed -n 's/^luks_prefix=//p' $S3CONF | head -n 1
	else
		echo scubed3-
	fi
}

# the device of the LUKS volume on partition NAME
s3_luks_dev () {
	echo /dev/mapper/$(s3_luks_prefix)$1
}

# is DEVDIR a disk that scubed3 made (the LUKS volume of a partition of the
# installer)?
s3_is_ours () {
	local dev p
	dev=$(cat $1/device 2>/dev/null)
	for p in $S3STATE/parts/*; do
		[ -d "$p" ] || continue
		[ "$dev" = "$(s3_luks_dev ${p##*/})" ] && return 0
	done
	return 1
}

# the size of a block device in MiB
s3_size_mib () {
	local dev sectors
	dev=$(readlink -f "$1")
	sectors=$(cat /sys/class/block/${dev#/dev/}/size 2>/dev/null) || return 1
	[ "$sectors" ] || return 1
	echo $(( sectors / 2048 ))
}

# the mesoblock size (log 2) for a base device of MIB MiB: the index (one
# mesoblock) has a bit for every macroblock, larger devices need larger
# mesoblocks (see "Example calculation of indexblock size" in the README)
s3_choose_meso () {
	local total m max
	total=$(( $1 >> (S3MACRO_LOG - 20) ))
	for m in 14 15 16 17 18 19 20 21; do
		[ $m -lt $S3MACRO_LOG ] || return 1
		max=$(( ((1 << m) - 256 - (12 << (S3MACRO_LOG - m))) * 8 ))
		if [ $max -ge $total ] &&
		   [ $(( total << (S3MACRO_LOG - m) )) -lt 4294967295 ]; then
			echo $m
			return 0
		fi
	done
	return 1
}

# the mesoblock and macroblock size (log 2) from the options
s3_meso_log () {
	set -- $(cat $S3STATE/options)
	while [ $# -gt 0 ]; do
		[ "$1" = -m ] && echo $2 && return 0
		shift
	done
	echo 14
}
s3_macro_log () {
	set -- $(cat $S3STATE/options)
	while [ $# -gt 0 ]; do
		[ "$1" = -M ] && echo $2 && return 0
		shift
	done
	echo 22
}

# KiB of data in a macroblock (the index takes one mesoblock)
s3_kib_per_block () {
	local m M
	m=$(s3_meso_log); M=$(s3_macro_log)
	echo $(( ((1 << (M - m)) - 1) << (m - 10) ))
}

# the blocks reserved for the garbage collector (as scubed3ctl resize)
s3_reserved () {
	echo $(( ($1 - 1)/4 + 1 ))
}

# the number of blocks of a partition with at least MIB MiB of data
s3_blocks_for () {
	local kib data n
	kib=$(s3_kib_per_block)
	data=$(( ($1*1024 + kib - 1)/kib ))
	n=$(( data*4/3 ))
	[ $n -ge 2 ] || n=2
	while [ $(( n - $(s3_reserved $n) )) -lt $data ]; do
		n=$((n + 1))
	done
	echo $n
}

# MiB of data in a partition of BLOCKS blocks
s3_mib_of () {
	echo $(( ($1 - $(s3_reserved $1)) * $(s3_kib_per_block) / 1024 ))
}

# a number from the output of the p command: s3_p_number "blocks total"
s3_p_number () {
	s3ctl_out -c p | sed -n "s/^0*\([0-9][0-9]*\) $1\$/\1/p" | head -n1
}

# a value of the info command of partition NAME: s3_info NAME no_macroblocks
s3_info () {
	s3ctl_out -c "info $1" | sed -n "s/^$2=//p" | head -n1
}

s3_valid_name () {
	case "$1" in
	    ""|.*|*[!a-zA-Z0-9_.-]*)	return 1 ;;
	esac
	[ ${#1} -le 30 ]
}

# ask a passphrase with debconf (the template partman-scubed3/passphrase,
# with DESCRIPTION), twice if VERIFY is yes; with EMPTY "empty" both fields
# may be left empty (a new partition or LUKS volume: a random key, PASS is
# empty; never for the key store); the passphrase is in $PASS,
# returns 1 if the user went back (db_set and db_go overwrite RET: the
# values are kept before the questions are cleared)
s3_ask_passphrase () {
	local desc verify empty templ again
	desc=$1
	verify=$2
	empty=$3
	PASS=
	while :; do
		db_subst partman-scubed3/passphrase DESCRIPTION "$desc"
		db_set partman-scubed3/passphrase ""
		db_fset partman-scubed3/passphrase seen false
		db_input critical partman-scubed3/passphrase
		if [ "$verify" = yes ]; then
			db_set partman-scubed3/passphrase-again ""
			db_fset partman-scubed3/passphrase-again seen false
			db_input critical partman-scubed3/passphrase-again
		fi
		db_go || return 1
		db_get partman-scubed3/passphrase
		PASS="$RET"
		again=
		if [ "$verify" = yes ]; then
			db_get partman-scubed3/passphrase-again
			again="$RET"
			db_set partman-scubed3/passphrase-again ""
		fi
		db_set partman-scubed3/passphrase ""
		RET=
		if [ -z "$PASS" ] && [ "$empty" = empty ] && [ -z "$again" ]; then
			return 0
		elif [ -z "$PASS" ] && [ "$empty" = empty ]; then
			again=
			templ=partman-scubed3/passphrase-mismatch
		elif [ -z "$PASS" ]; then
			again=
			templ=partman-scubed3/passphrase-empty
		elif [ "$verify" = yes ] && [ "$PASS" != "$again" ]; then
			PASS=
			again=
			templ=partman-scubed3/passphrase-mismatch
		elif [ "$verify" = yes ] && [ ${#PASS} -lt 8 ]; then
			again=
			db_set partman-scubed3/weak_passphrase false
			db_fset partman-scubed3/weak_passphrase seen false
			db_input critical partman-scubed3/weak_passphrase
			db_go || true
			db_get partman-scubed3/weak_passphrase
			[ "$RET" = true ] && return 0
			PASS=
			continue
		else
			again=
			return 0
		fi
		db_fset $templ seen false
		db_input critical $templ
		db_go || true
	done
}

# show an error (a template with ${ERROR}: the last lines of the log)
s3_error () {
	db_subst $1 ERROR "$(tail -n 4 /var/log/syslog 2>/dev/null | sed 's/^.*partman-scubed3: //')"
	db_subst $1 NAME "$2"
	db_fset $1 seen false
	db_input critical $1
	db_go || true
}

# a random salt for a new set of macroblocks
s3_random_salt () {
	local s
	s=$(dd if=/dev/urandom bs=18 count=1 2>/dev/null | base64 2>/dev/null)
	[ "$s" ] || s=$(dd if=/dev/urandom bs=18 count=1 2>/dev/null |
		od -An -tx1 2>/dev/null | tr -d ' \n')
	[ "$s" ] || s=$(dd if=/dev/urandom bs=18 count=1 2>/dev/null |
		hexdump -e '18/1 "%02x"' 2>/dev/null)
	echo "$s"
}

# the kernel modules: fuse (the daemon), loop (LUKS on a partition goes
# through a loop device), dm-mod and dm-crypt (LUKS); they are in the
# kernel of the installer or in its module udebs (fuse-modules,
# loop-modules: anna installs them with partman-crypto-dm, or load them by
# hand), they are not a dependency of partman-scubed3: their names change
# with the kernel; insmod if modprobe does not find them
# a kernel module: modprobe; a module udeb that anna installed after the
# start of the installer may not be in modules.dep yet (then the kernel
# cannot load it on demand either: "crypt: unknown target type"): depmod,
# modprobe again, otherwise insmod of its file
s3_load_module () {
	local f
	modprobe -q $1 2>/dev/null && return 0
	depmod -a 2>/dev/null
	modprobe -q $1 2>/dev/null && return 0
	f=$(find /lib/modules/$(uname -r) -name "$1.ko*" 2>/dev/null | head -n 1)
	[ "$f" ] || f=$(find /lib/modules/$(uname -r) \
		-name "$(echo $1 | tr - _).ko*" 2>/dev/null | head -n 1)
	[ "$f" ] && insmod $f 2>/dev/null
}

s3_modules () {
	local m
	for m in fuse loop dm-mod dm-crypt; do
		s3_load_module $m || true
	done
}

# start the daemon on BASE
s3_start () {
	local i
	s3_modules
	if [ ! -c /dev/fuse ]; then
		logger -t partman-scubed3 "/dev/fuse does not exist: the" \
			"kernel module fuse is not loaded (modprobe fuse)"
		return 1
	fi
	mkdir -p $S3MNT
	log-output -t partman-scubed3 scubed3 -b "$1" $(cat $S3STATE/options) $S3MNT || return 1
	i=0
	while ! s3_running; do
		i=$((i + 1))
		[ $i -le 50 ] || return 1
		sleep 1
	done
	return 0
}

# fill DEV with random data from /dev/urandom (a CSPRNG: free blocks look
# like the blocks scubed3 writes; on a recent kernel it is about as fast as
# a disk), in pieces of 64 MiB, with a progress bar that can be cancelled
# (the rest of the partition stays as it is); no dm-crypt and no wipe
# function of partman-crypto: they differ between releases (an error there
# left nothing in the log); a failure is logged (see s3_error)
s3_fill () {
	local dev sectors mib rest i n err cancelled=
	dev=$1
	sectors=$(cat /sys/class/block/$(basename $(readlink -f $dev))/size 2>/dev/null)
	if [ -z "$sectors" ]; then
		logger -t partman-scubed3 "filling $dev: its size is not known"
		return 1
	fi
	mib=$(( sectors / 2048 ))
	rest=$(( sectors % 2048 ))
	logger -t partman-scubed3 "filling $dev ($mib MiB) with random data"

	db_subst partman-scubed3/progress/fill_title DEVICE "$(humandev $dev)"
	db_subst partman-scubed3/progress/fill_text DEVICE "$(humandev $dev)"
	db_capb backup align progresscancel
	db_progress START 0 $(( mib / 64 + 1 )) partman-scubed3/progress/fill_title
	db_progress INFO partman-scubed3/progress/fill_text
	i=0
	while [ $(( i * 64 )) -lt $mib ]; do
		n=$(( mib - i * 64 ))
		[ $n -le 64 ] || n=64
		if ! err=$(dd if=/dev/urandom of=$dev bs=1048576 count=$n \
				seek=$(( i * 64 )) conv=notrunc 2>&1); then
			logger -t partman-scubed3 "filling $dev at $(( i * 64 )) MiB: $err"
			db_progress STOP
			db_capb backup align
			return 1
		fi
		i=$(( i + 1 ))
		if ! db_progress SET $i; then
			logger -t partman-scubed3 "filling $dev cancelled at $(( i * 64 )) MiB"
			cancelled=1
			break
		fi
	done
	# the last part, smaller than a MiB
	if [ $rest -gt 0 ] && [ $(( i * 64 )) -ge $mib ] &&
			! err=$(dd if=/dev/urandom of=$dev bs=512 count=$rest \
				seek=$(( mib * 2048 )) conv=notrunc 2>&1); then
		logger -t partman-scubed3 "filling the end of $dev: $err"
		db_progress STOP
		db_capb backup align
		return 1
	fi
	sync
	db_progress STOP
	db_capb backup align
	[ "$cancelled" ] || logger -t partman-scubed3 "$dev is filled with random data"
	return 0
}

# a list of the partitions of the installer, for the menu
s3_partition_list () {
	local p n blocks
	for p in $S3STATE/parts/*; do
		[ -d "$p" ] || continue
		n=${p##*/}
		blocks=$(s3_info $n no_macroblocks)
		printf "%s (%s MiB%s%s)\n" "$n" "$(s3_mib_of ${blocks:-0})" \
			"$([ -f $p/luks ] && echo ", LUKS")" \
			"$([ "$(cat $p/view 2>/dev/null)" != 1 ] && echo ", hidden")"
	done
}

# scubed3ctl with the key store, all its views unlocked: the keys of the
# passphrases (S3VIEWKEYS, one per line, of the boot passphrase first: view
# 1, scubed3ctl -k) come first on its input, then the input of the caller
# (for the commands)
s3ks () {
	{ printf '%s\n\n' "$S3VIEWKEYS"; cat; } |
		log-output -t partman-scubed3 scubed3ctl -q -YYY -C $S3CONF \
			-K $S3KEYS -k -c unlock-all "$@"
}

# the same, its output on stdout
s3ks_out () {
	{ printf '%s\n\n' "$S3VIEWKEYS"; cat; } |
		scubed3ctl -q -YYY -C $S3CONF -K $S3KEYS -k -c unlock-all "$@" \
			2>/dev/null
}

# a message while something takes long (the KDF of a passphrase, LUKS):
# without it the screen of the installer stays empty; $1 is the text
# (partman-scubed3/progress/$1); the bar moves while it runs: a ticker in
# the background sets it every second, to 90% at the duration of the last
# time (S3STATE/busy_time.$1, 10 seconds the first time), then slower to
# 99%; s3_busy_done stops it (no debconf command of ours runs before the
# ticker is gone: they share the debconf pipes)
s3_busy () {
	local e
	db_progress START 0 100 partman-scubed3/progress/busy_title
	db_progress INFO partman-scubed3/progress/$1
	S3BUSY_TYPE=$1
	S3BUSY_START=$(date +%s)
	e=$(cat $S3STATE/busy_time.$1 2>/dev/null)
	[ "${e:-0}" -gt 0 ] 2>/dev/null || e=10
	rm -f $S3STATE/busy.stop
	# (an asynchronous list gets /dev/null as its input: the answers of
	# debconf come through fd 8)
	exec 8<&0
	(
		t=0
		while [ ! -f $S3STATE/busy.stop ]; do
			sleep 1
			[ -f $S3STATE/busy.stop ] && break
			t=$((t + 1))
			if [ $t -lt $e ]; then
				db_progress SET $((90*t/e))
			else
				db_progress SET $((99 - 9*e/t))
			fi
		done
	) <&8 &
	S3BUSY_PID=$!
	exec 8<&-
}

s3_busy_done () {
	local t
	>$S3STATE/busy.stop
	wait $S3BUSY_PID 2>/dev/null
	rm -f $S3STATE/busy.stop
	t=$(( $(date +%s) - S3BUSY_START ))
	[ $t -gt 0 ] || t=1
	echo $t > $S3STATE/busy_time.$S3BUSY_TYPE
	db_progress SET 100
	db_progress STOP
}

# cryptsetup (for LUKS, root has it): partman-crypto-dm depends on
# cryptsetup-udeb, but a udeb installed by hand (udpkg -i) gets no
# dependencies; it is installed if it is missing
s3_cryptsetup () {
	local u m
	if ! command -v cryptsetup > /dev/null 2>&1; then
		for u in cryptsetup-udeb dmsetup-udeb crypto-dm-modules; do
			log-output -t partman-scubed3 anna-install $u ||
				logger -t partman-scubed3 "anna-install $u failed"
		done
		command -v cryptsetup > /dev/null 2>&1 || return 1
	fi
	# the device mapper target crypt (dm-crypt), and the cipher of LUKS
	# (aes-xts-plain64); they are loaded now, not on demand (a module
	# udeb installed by anna is not in modules.dep before depmod)
	for m in dm-mod dm-crypt xts aesni-intel aes-generic; do
		s3_load_module $m || true
	done
	if command -v dmsetup > /dev/null 2>&1 &&
			! dmsetup targets 2>/dev/null | grep -q '^crypt '; then
		logger -t partman-scubed3 "the device mapper has no target crypt" \
			"(the kernel module dm-crypt is not loaded)"
		return 1
	fi
	return 0
}

# the layout for the menu: the base device, the partitions (size, view,
# LUKS), the unclaimed blocks, the slots of the views of the key store;
# the lines are separated by \n (debconf with the capability escape)
s3_layout () {
	local mib total free level p n blocks view line lent slots
	mib=$(( 1 << ($(sed -n 's/.*-M \([0-9]*\).*/\1/p' $S3STATE/options) - 20) ))
	total=$(s3_p_number "blocks total")
	free=$(s3_p_number "blocks unclaimed")
	level=$(s3ctl_out -c p | sed -n 's/^paranoia level \([0-3]\) .*/\1/p')
	printf '%s: %s blocks of %s MiB, paranoia level %s' \
		"$(humandev $(cat $S3STATE/base))" "${total:-0}" $mib "${level:-?}"
	for p in $S3STATE/parts/*; do
		[ -d "$p" ] || continue
		n=${p##*/}
		blocks=$(s3_info $n no_macroblocks)
		view=$(cat $p/view 2>/dev/null || echo 1)
		line="$n: $(s3_mib_of ${blocks:-0}) MiB (${blocks:-0} blocks)"
		if [ -f $p/lent ]; then
			lent=$(( ${blocks:-0} - $(cut -d' ' -f1 $p/lent) ))
			line="$line, of them $lent lent until the end of the installation"
		fi
		if [ "$view" = 1 ]; then
			line="$line, view 1 (boot passphrase)"
		else
			line="$line, view $view (hidden passphrase)"
		fi
		if [ ! -f $p/luks ]; then
			line="$line, no LUKS"
		elif [ -f $p/luks_open ]; then
			line="$line, LUKS open as $(s3_luks_dev $n)"
		elif cryptsetup isLuks $S3MNT/$n 2>/dev/null; then
			line="$line, LUKS"
		else
			line="$line, LUKS at finish"
		fi
		printf '\\n%s' "$line"
	done
	printf '\\nunclaimed: %s blocks' "${free:-0}"
	# (the answers of unlock-all, in the order of S3VIEWKEYS: view 1 is
	# the boot passphrase; the default name of a view is its slot)
	slots=$(s3ks_out < /dev/null |
		sed -n 's/.*view i\([0-9][0-9]\): [0-9]* entries.*/\1/p' |
		awk '{ printf "%sview %d in slot %s", (NR > 1 ? ", " : ""), NR, $1 }')
	printf '\\nkey store: %s (of the slots 00 to 63, chosen at random)\n' "$slots"
}

# the key of passphrase $1 of the key store in KEY (64 hexadecimal digits,
# keys-derive of scubed3ctl): the KDF takes seconds (minutes in a slow
# virtual machine), it is computed once per passphrase; returns 1 if it
# fails
s3_key_of () {
	s3_busy derive
	KEY=$(printf '%s\n' "$1" | s3ctl_out -c keys-derive |
		sed -n 's/.*key: \([0-9a-f]\{64\}\)$/\1/p')
	s3_busy_done
	[ ${#KEY} = 64 ] && return 0
	KEY=
	logger -t partman-scubed3 "keys-derive failed"
	return 1
}

# the number of views that the keys in S3VIEWKEYS unlock (the answers of
# unlock-all: "view NAME: N entries"; the prompts of scubed3ctl are on the
# same lines)
s3_unlocked_views () {
	s3ks_out < /dev/null | sed -n 's/.*\(view [^ :]*\): [0-9]* entries.*/\1/p' |
		sort -u | wc -l | tr -d ' '
}

# the view of key $1 (its line in S3VIEWKEYS), 0 if it has none
s3_view_of () {
	local n l
	n=0
	while IFS= read -r l; do
		n=$((n + 1))
		[ "$l" = "$1" ] && echo $n && return 0
	done <<EOF
$S3VIEWKEYS
EOF
	echo 0
}

# a new key store, with the boot passphrase (view 1)
s3_keystore_new () {
	db_metaget partman-scubed3/text/boot_passphrase description
	s3_ask_passphrase "$RET" yes || return 1
	if ! s3_key_of "$PASS"; then
		PASS=
		s3_error partman-scubed3/keystore_failed
		return 1
	fi
	PASS=
	rm -f $S3KEYS
	if ! s3ctl -K $S3KEYS -c keys-create; then
		KEY=
		s3_error partman-scubed3/keystore_failed
		return 1
	fi
	printf '\n%s\n%s\n' "$KEY" "$KEY" |
		s3ctl -K $S3KEYS -k -c unlock-all -c keys-passphrase-add
	S3VIEWKEYS=$KEY
	KEY=
	if [ "$(s3_unlocked_views)" != 1 ]; then
		S3VIEWKEYS=
		rm -f $S3KEYS
		return 1
	fi
	echo 1 > $S3STATE/views
	db_fset partman-scubed3/backup_note seen false
	db_input critical partman-scubed3/backup_note
	db_go || true
	return 0
}

# Configure scubed3 again: the passphrases of the key store are asked again
# (the boot passphrase, then the hidden ones), a change of the key store
# needs all of them (the views that are not unlocked are lost)
s3_keystore_unlock () {
	local views n
	views=$(cat $S3STATE/views 2>/dev/null || echo 1)
	S3VIEWKEYS=
	n=1
	while [ $n -le $views ]; do
		if [ $n = 1 ]; then
			db_metaget partman-scubed3/text/boot_passphrase description
		else
			db_metaget partman-scubed3/text/hidden_passphrase description
		fi
		s3_ask_passphrase "$RET" no || return 1
		if ! s3_key_of "$PASS"; then
			PASS=
			continue
		fi
		PASS=
		if [ "$(s3_view_of "$KEY")" != 0 ]; then
			KEY=
			continue
		fi
		S3VIEWKEYS="${S3VIEWKEYS:+$S3VIEWKEYS
}$KEY"
		KEY=
		if [ "$(s3_unlocked_views)" = $n ]; then
			n=$((n + 1))
			continue
		fi
		# not a passphrase of the key store: again
		S3VIEWKEYS=$(printf '%s\n' "$S3VIEWKEYS" | sed '$d')
		db_fset partman-scubed3/wrong_passphrase seen false
		db_input critical partman-scubed3/wrong_passphrase
		db_go || true
	done
	return 0
}

# the view for partition $1: 1 for root, otherwise the boot passphrase (1)
# or a hidden one (the view of a hidden passphrase, a new one if it has
# none); in $VIEW, returns 1 if the user went back
s3_choose_view () {
	local v
	VIEW=1
	[ "$1" = root ] && return 0
	db_subst partman-scubed3/view NAME "$1"
	db_set partman-scubed3/view boot
	db_fset partman-scubed3/view seen false
	db_input critical partman-scubed3/view
	db_go || return 1
	db_get partman-scubed3/view
	[ "$RET" = boot ] && return 0

	db_metaget partman-scubed3/text/hidden_passphrase description
	s3_ask_passphrase "$RET" yes || return 1
	if ! s3_key_of "$PASS"; then
		PASS=
		return 1
	fi
	PASS=
	v=$(s3_view_of "$KEY")
	if [ $v = 1 ]; then
		KEY=
		db_fset partman-scubed3/hidden_is_boot seen false
		db_input critical partman-scubed3/hidden_is_boot
		db_go || true
		return 1
	fi
	if [ $v != 0 ]; then
		VIEW=$v
		KEY=
		return 0
	fi
	printf '%s\n%s\n' "$KEY" "$KEY" | s3ks -c keys-passphrase-add
	S3VIEWKEYS="$S3VIEWKEYS
$KEY"
	KEY=
	VIEW=$(( $(printf '%s\n' "$S3VIEWKEYS" | wc -l) ))
	if [ "$(s3_unlocked_views)" != $VIEW ]; then
		S3VIEWKEYS=$(printf '%s\n' "$S3VIEWKEYS" | sed '$d')
		return 1
	fi
	echo $VIEW > $S3STATE/views
	return 0
}

# is $1 a passphrase of the LUKS volume on partition $2? (it is opened and
# closed again)
s3_luks_check () {
	local keyfile ret
	keyfile=$S3STATE/key.$$
	printf %s "$1" > $keyfile
	ret=1
	s3_busy luks_check
	if log-output -t partman-scubed3 cryptsetup open --key-file $keyfile \
			$S3MNT/$2 scubed3-check-$2; then
		log-output -t partman-scubed3 cryptsetup close scubed3-check-$2
		ret=0
	fi
	s3_busy_done
	dd if=/dev/urandom of=$keyfile bs=$(( ${#1} + 1 )) count=1 conv=notrunc 2>/dev/null
	rm -f $keyfile
	return $ret
}

# the stable name of the base device (for the initramfs of the installed
# system): /dev/disk/by-partuuid, by-id or by-path
s3_stable_name () {
	local real l
	real=$(readlink -f "$1")
	for l in /dev/disk/by-partuuid/* /dev/disk/by-id/* /dev/disk/by-path/*; do
		[ -e "$l" ] || continue
		if [ "$(readlink -f $l)" = "$real" ]; then
			echo $l
			return 0
		fi
	done
	echo "$1"
}
