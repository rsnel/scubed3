#!/bin/sh
# build.sh [OUTDIR]: build the binaries the tests need in OUTDIR (default: the
# current directory): scubed3, scubed3-crash (with -DSCUBED3_CRASH_TEST, see
# README), scubed3ctl and keystorectl from ../src, and jstat (unit test of
# juggler.c)
#
# the configure step of the main tree must have been run (config.h); extra
# compiler and linker flags can be given in EXTRA_CFLAGS and EXTRA_LDFLAGS
# (for libraries in a non-standard place); libfuse 3 is found with
# pkg-config
DEV=$(cd "$(dirname "$0")" && pwd); TOP=$DEV/..; SRC=$TOP/src; OUT=${1:-.}
CC=${CC:-gcc}
FUSE_CFLAGS=$(pkg-config --cflags fuse3 2>/dev/null || echo -I/usr/include/fuse3)
FUSE_LIBS=$(pkg-config --libs fuse3 2>/dev/null || echo -lfuse3)
CFLAGS="-D_GNU_SOURCE -O3 -g -Wall -Werror -D_FILE_OFFSET_BITS=64 -I$SRC -I$TOP $EXTRA_CFLAGS"
LDFLAGS="$EXTRA_LDFLAGS"
DAEMON="scubed3.c binio.c bitmap.c blockio.c cipher.c dllarr.c fuse_io.c gcry.c
	hashtbl.c pthd.c util.c verbose.c cipher_null.c cipher_cbc.c control.c
	ecch.c random.c juggler.c"
COMMON="ctl_common.c keystore.c verbose.c gcry.c ecch.c hashtbl.c pthd.c util.c"
DLIBS="-lpthread $FUSE_LIBS -lgcrypt -lm -lrt -lgpg-error"
CLIBS="-lreadline -lgcrypt -lpthread -lgpg-error"

[ -f "$TOP/config.h" ] || { echo "run configure in $TOP first" >&2; exit 1; }
mkdir -p "$OUT" && OUT=$(cd "$OUT" && pwd) || exit 1
cd "$SRC" || exit 1
$CC $CFLAGS $FUSE_CFLAGS -o "$OUT/scubed3" $DAEMON $LDFLAGS $DLIBS || exit 1
$CC $CFLAGS $FUSE_CFLAGS -DSCUBED3_CRASH_TEST -o "$OUT/scubed3-crash" \
	$DAEMON $LDFLAGS $DLIBS || exit 1
$CC $CFLAGS -o "$OUT/scubed3ctl" scubed3ctl.c $COMMON $LDFLAGS $CLIBS || exit 1
$CC $CFLAGS -o "$OUT/keystorectl" keystorectl.c $COMMON $LDFLAGS $CLIBS || exit 1
$CC $CFLAGS -o "$OUT/jstat" "$DEV/jstat.c" juggler.c random.c verbose.c util.c \
	$LDFLAGS -lm || exit 1
echo "built scubed3 scubed3-crash scubed3ctl keystorectl jstat in $OUT"
