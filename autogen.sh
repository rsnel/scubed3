#! /bin/sh
#if [ -d .svn/ ]; then
#	if ! svn up; then
#		echo "error running svn up to get uptodate revision"
#		exit 1
#	fi
#fi
autoreconf -f -i
echo "Now type \`./configure' to configure scubed3."
