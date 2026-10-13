#!/bin/sh
# runall2.sh: torture runs in paranoia level 2, summary in runall2.out
T=$(cd "$(dirname "$0")" && pwd)
cd "${S3BUILD:-.}"
: > runall2.out
for spec in "scubed3 250 801" "scubed3-crash 250 802 crash" "scubed3-crash 250 803 tear" "scubed3-crash 250 804 crash" "scubed3-crash 250 805 tear"; do
	fusermount -u -z /mnt/s3 2>/dev/null
	PARANOIA=2 timeout 1500 python3 -I -u "$T"/torture.py $spec > torture.out 2>&1
	echo "[$spec] exit=$? $(tail -1 torture.out | cut -c1-600)" >> runall2.out
done
echo DONE >> runall2.out
