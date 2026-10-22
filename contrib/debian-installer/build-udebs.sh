#!/bin/sh
# build-udebs.sh [OUTDIR] - build the packages for root on scubed3 with the
# debian-installer (see README), with dpkg-deb, on a Debian system of the
# release that is installed:
#
#	scubed3-udeb_VERSION_ARCH.udeb	the daemon, scubed3ctl and
#					keystorectl (without
#					readline, statically linked) for the
#					installer, and the initramfs-tools
#					scripts
#	partman-scubed3_VERSION_all.udeb	"Configure scubed3" in partman
#	scubed3_VERSION_ARCH.deb	scubed3 for the installed system (with
#					the initramfs-tools scripts in
#					/usr/share/initramfs-tools)
#
# needs: a C compiler, autoconf, automake, pkg-config, the development
# files of libfuse 3, libgcrypt and readline, with the static libraries
# (libfuse3-dev, libgcrypt20-dev, libgpg-error-dev have them), dpkg-dev
# (dpkg-shlibdeps)
#
# the programs of the udeb are linked statically: they do not depend on the
# library udebs of the release of the installer (their names change); with
# STATIC=no they are linked dynamically, the udeb then depends on the
# library udebs (dpkg-shlibdeps)
#
# the configure step gets CPPFLAGS, LDFLAGS and PKG_CONFIG_PATH from the
# environment
OUT=${1:-.}
DI=$(cd "$(dirname "$0")" && pwd); TOP=$(cd "$DI/../.." && pwd)
VERSION=$(cat "$TOP/VERSION" | tr - '~')
ARCH=$(dpkg --print-architecture)
MAINT="Rik Snel <rik@snel.it>"
B=$(mktemp -d)
trap 'rm -rf $B' EXIT

fail() { echo "build-udebs.sh: $*" >&2; exit 1; }

mkdir -p "$OUT" && OUT=$(cd "$OUT" && pwd) || exit 1

STATIC=${STATIC:-yes}

# build scubed3 twice: with readline (the .deb) and without (the udeb,
# statically linked)
build() {
	local static=
	[ $1 = no ] && [ $STATIC = yes ] && static=--enable-static-programs
	mkdir -p $B/src-$1
	(cd "$TOP" && tar cf - --exclude=.svn --exclude=autom4te.cache \
		--exclude=testing .) | tar xf - -C $B/src-$1
	cd $B/src-$1 || exit 1
	# (configure.ac may be newer than configure)
	autoreconf -fi > $B/autoreconf.log 2>&1 ||
		fail "autoreconf failed, see $B/autoreconf.log"
	make distclean > /dev/null 2>&1
	./configure --prefix=/usr --with-readline=$1 $static > $B/configure-$1.log 2>&1 ||
		fail "configure failed: $(tail -3 $B/configure-$1.log)"
	make > $B/make-$1.log 2>&1 || fail "make failed: $(tail -5 $B/make-$1.log)"
	if [ "$static" ] && ldd src/scubed3 > /dev/null 2>&1; then
		fail "src/scubed3 is not linked statically (see $B/make-$1.log)"
	fi
	strip --strip-unneeded src/scubed3 src/scubed3ctl src/keystorectl
}
build yes
build no

# Depends from the libraries (dpkg-shlibdeps, -tudeb: the udebs of the
# libraries); FALLBACK if dpkg-shlibdeps cannot find them
shlibdeps() {
	local type=$1 fallback=$2; shift 2
	mkdir -p $B/shl/debian
	printf 'Source: scubed3\n\nPackage: scubed3\nArchitecture: any\n' \
		> $B/shl/debian/control
	if (cd $B/shl && dpkg-shlibdeps -t$type -O "$@" 2> $B/shlibdeps.log) |
			sed -n 's/^shlibs:Depends=//p' > $B/deps && [ -s $B/deps ]; then
		cat $B/deps
	else
		echo "WARNING: dpkg-shlibdeps failed (see below), using: $fallback" >&2
		cat $B/shlibdeps.log >&2
		echo "$fallback"
	fi
}

# ----------------------------------------------------------- scubed3-udeb
P=$B/scubed3-udeb
mkdir -p $P/DEBIAN $P/usr/sbin $P/usr/share/scubed3/initramfs-tools/hooks \
	$P/usr/share/scubed3/initramfs-tools/scripts/local-top
cp $B/src-no/src/scubed3 $B/src-no/src/scubed3ctl $B/src-no/src/keystorectl \
	$P/usr/sbin/
cp "$TOP/contrib/initramfs-tools/hooks/scubed3" \
	$P/usr/share/scubed3/initramfs-tools/hooks/
cp "$TOP/contrib/initramfs-tools/scripts/local-top/scubed3" \
	$P/usr/share/scubed3/initramfs-tools/scripts/local-top/
chmod 755 $P/usr/share/scubed3/initramfs-tools/hooks/scubed3 \
	$P/usr/share/scubed3/initramfs-tools/scripts/local-top/scubed3
if [ $STATIC = yes ]; then
	DEPENDS=
else
	DEPENDS="Depends: $(shlibdeps udeb "libfuse3-4-udeb | libfuse3-3-udeb, libgcrypt20-udeb, libgpg-error0-udeb" \
		$P/usr/sbin/scubed3 $P/usr/sbin/scubed3ctl $P/usr/sbin/keystorectl)"
fi
cat > $P/DEBIAN/control <<EOF
Package: scubed3-udeb
Package-Type: udeb
Source: scubed3
Version: $VERSION
Architecture: $ARCH
Maintainer: $MAINT
Section: debian-installer
Priority: optional
${DEPENDS}
Description: Simple Steganographic Store (scubed3) for the installer
EOF
# (no empty line in the control file)
sed -i '/^$/d' $P/DEBIAN/control
dpkg-deb -Zxz --root-owner-group -b $P "$OUT/scubed3-udeb_${VERSION}_$ARCH.udeb" > /dev/null ||
	fail "dpkg-deb scubed3-udeb"

# -------------------------------------------------------- partman-scubed3
# a directory with _numbers is installed as NNname (as dh-di does)
P=$B/partman-scubed3
S=$DI/partman-scubed3
mkdir -p $P/DEBIAN $P/lib/partman/lib $P/usr/lib/finish-install.d
cp $S/lib/scubed3-base.sh $P/lib/partman/lib/
for d in choose_partition choose_method init.d update.d check.d; do
	mkdir -p $P/lib/partman/$d
	while read num name; do
		[ "$name" ] || continue
		cp -r $S/$d/$name $P/lib/partman/$d/$num$name
	done < $S/$d/_numbers
done
cp $S/finish-install.d/08scubed3 $P/usr/lib/finish-install.d/
find $P/lib $P/usr -type f ! -name '*.sh' -exec chmod 755 {} +
cp $S/templates $P/DEBIAN/templates
# a copy, for a udeb that is installed by hand (see lib/scubed3-base.sh)
mkdir -p $P/usr/share/partman-scubed3
cp $S/templates $P/usr/share/partman-scubed3/templates
cat > $P/DEBIAN/control <<EOF
Package: partman-scubed3
Package-Type: udeb
Source: scubed3
Version: $VERSION
Architecture: all
Maintainer: $MAINT
Section: debian-installer
Priority: optional
Depends: partman-base, partman-crypto-dm, cdebconf-udeb, di-utils, scubed3-udeb (= $VERSION)
Description: Add to partman support for scubed3 (hidden encrypted partitions)
EOF
dpkg-deb -Zxz --root-owner-group -b $P "$OUT/partman-scubed3_${VERSION}_all.udeb" > /dev/null ||
	fail "dpkg-deb partman-scubed3"

# ---------------------------------------------------------------- scubed3
P=$B/scubed3
mkdir -p $P/DEBIAN $P/usr/sbin $P/usr/share/initramfs-tools/hooks \
	$P/usr/share/initramfs-tools/scripts/local-top $P/usr/share/doc/scubed3
cp $B/src-yes/src/scubed3 $B/src-yes/src/scubed3ctl \
	$B/src-yes/src/keystorectl $P/usr/sbin/
cp "$TOP/contrib/initramfs-tools/hooks/scubed3" $P/usr/share/initramfs-tools/hooks/
cp "$TOP/contrib/initramfs-tools/scripts/local-top/scubed3" \
	$P/usr/share/initramfs-tools/scripts/local-top/
chmod 755 $P/usr/share/initramfs-tools/hooks/scubed3 \
	$P/usr/share/initramfs-tools/scripts/local-top/scubed3
cp "$TOP/README" $P/usr/share/doc/scubed3/README
cp "$TOP/contrib/initramfs-tools/README" $P/usr/share/doc/scubed3/README.initramfs
cp "$TOP/contrib/initramfs-tools/initramfs.conf.example" $P/usr/share/doc/scubed3/
cat > $P/usr/share/doc/scubed3/copyright <<EOF
scubed3, the Simple Steganographic Store

Copyright (C) 2019-2026 Rik Snel <rik@snel.it>

License: GPL-3+
 This program is free software: you can redistribute it and/or modify it
 under the terms of the GNU General Public License as published by the Free
 Software Foundation, either version 3 of the License, or (at your option)
 any later version.
 .
 On Debian systems, the full text of the GNU General Public License version
 3 can be found in the file /usr/share/common-licenses/GPL-3.
EOF
DEPS=$(shlibdeps deb "libfuse3-4 | libfuse3-3, libgcrypt20, libgpg-error0, libreadline8t64 | libreadline8" \
	$P/usr/sbin/scubed3 $P/usr/sbin/scubed3ctl $P/usr/sbin/keystorectl)
cat > $P/DEBIAN/control <<EOF
Package: scubed3
Source: scubed3
Version: $VERSION
Architecture: $ARCH
Maintainer: $MAINT
Section: utils
Priority: optional
Depends: $DEPS, fuse3
Recommends: cryptsetup, cryptsetup-initramfs
Homepage: https://penta.snel.it/~rsnel/scubed/
Description: Simple Steganographic Store: deniable encrypted partitions
 scubed3 hides any number of encrypted partitions in one device full of
 random data. Without its passphrase a partition cannot be told apart from
 unused space. The partitions are files (FUSE), managed with scubed3ctl;
 their keys can be in a key store (keystorectl).
 .
 With /etc/scubed3/initramfs.conf the root file system can be on a scubed3
 partition (initramfs-tools).
EOF
dpkg-deb -Zxz --root-owner-group -b $P "$OUT/scubed3_${VERSION}_$ARCH.deb" > /dev/null ||
	fail "dpkg-deb scubed3"

echo "built in $OUT:"
ls -1 "$OUT"/*scubed3*_${VERSION}_*
