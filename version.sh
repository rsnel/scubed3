#!/bin/sh
# the version of scubed3: from git (git describe) or svn if the source is a
# working copy, otherwise from the file VERSION

test "$1" && extra="-$1"

if [ -d .git ] && git describe --always --dirty >/dev/null 2>&1; then
	echo "`git describe --always --dirty`$extra"
	exit
fi

if [ -d .svn/ ] && command -v svn >/dev/null; then
	svn_revision=`LC_ALL=C svn info 2> /dev/null | grep Revision | cut -d' ' -f2`
	test $svn_revision || svn_revision=`grep revision .svn/entries 2>/dev/null | cut -d '"' -f2`
	test $svn_revision || svn_revision=`sed -n -e '/^dir$/{n;p;q;}' .svn/entries 2>/dev/null`
	test $svn_revision || svn_revision=UNKNOWN

	dirty=""
	if [ "`svn status | wc -l`" != "0" ]; then
		dirty="dirty"
	fi
	echo SVN-r${svn_revision}${dirty}$extra
	exit
fi

if [ -f VERSION ]; then
	echo "`cat VERSION`$extra"
	exit
fi

echo UNKNOWN
