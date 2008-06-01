#! /bin/sh
if ! svn up; then
	echo "error running svn up to get uptodate revision"
	exit 1
fi
autoreconf -f -i
echo "Now type \`./configure' to configure scubed3."
