#!/bin/sh
# runall.sh: several torture runs, one after the other, summary in runall.out
T=$(cd "$(dirname "$0")" && pwd)
cd "${S3BUILD:-.}"
: > runall.out
for spec in "scubed3 250 201" "scubed3-crash 250 202 crash" "scubed3-crash 250 203 tear" "scubed3 250 204" "scubed3-crash 250 205 tear" "scubed3-crash 250 206 crash" "scubed3 250 207" "scubed3-crash 250 208 tear"; do
	fusermount -u -z /mnt/s3 2>/dev/null
	timeout 1500 python3 -I -u "$T"/torture.py $spec > torture.out 2>&1
	echo "[$spec] exit=$? $(tail -1 torture.out | cut -c1-600)" >> runall.out
	cp torture.log "torture.$(echo $spec | tr ' ' _).log"
done
echo DONE >> runall.out
