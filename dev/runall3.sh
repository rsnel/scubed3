#!/bin/sh
# runall3.sh: torture runs in paranoia level 3 with 64 KiB blocks (TORTURE_SIZES) and 50 MiB/s: as fast as the disk can; CRASH_MAX=40: the clock writes too, summary in runall3.out
FUSERMOUNT=$(command -v fusermount3 || echo fusermount)
T=$(cd "$(dirname "$0")" && pwd)
cd "${S3BUILD:-.}"
: > runall3.out
for spec in "scubed3 250 901" "scubed3-crash 250 902 crash" "scubed3-crash 250 903 tear" "scubed3-crash 250 904 crash" "scubed3-crash 250 905 tear"; do
	$FUSERMOUNT -u -z /mnt/s3 2>/dev/null
	PARANOIA="3 51200" CRASH_MAX=40 TORTURE_SIZES="12 16" timeout 1500 python3 -I -u "$T"/torture.py $spec > torture.out 2>&1
	echo "[$spec] exit=$? $(tail -1 torture.out | cut -c1-600)" >> runall3.out
done
echo DONE >> runall3.out
