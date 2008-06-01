#!/bin/sh

if [ ! -d .svn/ ]; then
	echo UNKNOWN
	exit
fi
test "$1" && extra="-$1"

svn_revision=`LC_ALL=C svn info 2> /dev/null | grep Revision | cut -d' ' -f2`
test $svn_revision || svn_revision=`grep revision .svn/entries 2>/dev/null | cut -d '"' -f2`
test $svn_revision || svn_revision=`sed -n -e '/^dir$/{n;p;q;}' .svn/entries 2>/dev/null`
test $svn_revision || svn_revision=UNKNOWN

dirty=""
if [ "`svn status | wc -l`" != "0" ]; then
	dirty="dirty"
fi
echo SVN-r${svn_revision}${dirty}
