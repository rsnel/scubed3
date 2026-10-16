# scubed3-base.sh - functions of partman-scubed3 (see ../README)
#
# the state of the installer is kept in $S3STATE:
#
#	base		the base device (a partition with method scubed3)
#	options		-m MESOBLOCK_LOG -M MACROBLOCK_LOG of the daemon
#	scubed3.conf	salt, iterations, paranoia (for the installed system)
#	level		the paranoia level during the installation (after
#			finish)
#	parts/NAME/	a partition, opened or created by the installer:
#		luks		it has (or gets) LUKS on it
#		luks_open	LUKS is open as /dev/mapper/scubed3-NAME
#		lent		"BLOCKS RESERVED": its size before blocks were
#				lent to it, the blocks are given back at the
#				end of the installation (finish-install)
#
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

# is DEVDIR a disk that scubed3 made (a partition of the installer)?
s3_is_ours () {
	case "$(cat $1/device 2>/dev/null)" in
	    /dev/mapper/scubed3-*)	return 0 ;;
	esac
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
# with DESCRIPTION), twice if VERIFY is yes; the passphrase is in $PASS,
# returns 1 if the user went back
s3_ask_passphrase () {
	local desc verify templ
	desc=$1
	verify=$2
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
		db_set partman-scubed3/passphrase ""
		if [ -z "$PASS" ]; then
			templ=partman-scubed3/passphrase-empty
		elif [ "$verify" = yes ]; then
			db_get partman-scubed3/passphrase-again
			db_set partman-scubed3/passphrase-again ""
			if [ "$PASS" != "$RET" ]; then
				templ=partman-scubed3/passphrase-mismatch
			elif [ ${#PASS} -lt 8 ]; then
				db_set partman-scubed3/weak_passphrase false
				db_fset partman-scubed3/weak_passphrase seen false
				db_input critical partman-scubed3/weak_passphrase
				db_go || true
				db_get partman-scubed3/weak_passphrase
				[ "$RET" = true ] && return 0
				continue
			else
				return 0
			fi
		else
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
s3_modules () {
	local m
	for m in fuse loop dm-mod dm-crypt; do
		modprobe -q $m 2>/dev/null || insmod $m 2>/dev/null || true
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
		printf "%s (%s MiB%s)\n" "$n" "$(s3_mib_of ${blocks:-0})" \
			"$([ -f $p/luks ] && echo ", LUKS")"
	done
}

# LUKS on partition NAME with a new passphrase (blockdev-keygen of
# partman-crypto asks it), opened as /dev/mapper/scubed3-NAME
s3_luks_format () {
	local name keyfile ret
	name=$1
	keyfile=$S3STATE/key.$$
	db_metaget partman-scubed3/text/luks_description description
	if ! /bin/blockdev-keygen "$(printf "$RET" "$name")" passphrase $keyfile; then
		rm -f $keyfile
		return 1
	fi
	ret=0
	log-output -t partman-scubed3 cryptsetup luksFormat --batch-mode \
		--key-file $keyfile $S3MNT/$name || ret=1
	if [ $ret = 0 ]; then
		log-output -t partman-scubed3 cryptsetup open --key-file $keyfile \
			$S3MNT/$name scubed3-$name || ret=1
	fi
	dd if=/dev/urandom of=$keyfile bs=$(wc -c < $keyfile) count=1 conv=notrunc 2>/dev/null
	rm -f $keyfile
	[ $ret = 0 ] && >$S3STATE/parts/$name/luks_open
	return $ret
}

# open the existing LUKS volume on partition NAME
s3_luks_open () {
	local name keyfile
	name=$1
	keyfile=$S3STATE/key.$$
	while :; do
		db_metaget partman-scubed3/text/luks_description description
		s3_ask_passphrase "$(printf "$RET" "$name")" no || return 1
		printf %s "$PASS" > $keyfile
		PASS=
		if log-output -t partman-scubed3 cryptsetup open --key-file $keyfile \
				$S3MNT/$name scubed3-$name; then
			rm -f $keyfile
			>$S3STATE/parts/$name/luks_open
			return 0
		fi
		rm -f $keyfile
		s3_error partman-scubed3/luks_open_failed "$name"
	done
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
