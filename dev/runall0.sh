#!/bin/sh
# runall0.sh: torture runs in paranoia level 0, summary in runall0.out
FUSERMOUNT=$(command -v fusermount3 || echo fusermount)
T=$(cd "$(dirname "$0")" && pwd)
cd "${S3BUILD:-.}"
: > runall0.out
for spec in "scubed3 250 701" "scubed3-crash 250 702 crash" "scubed3-crash 250 703 tear" "scubed3-crash 250 704 crash" "scubed3-crash 250 705 tear"; do
	$FUSERMOUNT -u -z /mnt/s3 2>/dev/null
	PARANOIA=0 timeout 1500 python3 -I -u "$T"/torture.py $spec > torture.out 2>&1
	echo "[$spec] exit=$? $(tail -1 torture.out | cut -c1-600)" >> runall0.out
done
echo DONE >> runall0.out
