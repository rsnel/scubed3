#!/bin/sh
# batch.sh: all tests, one after the other; run as root in the directory with
# the binaries (see build.sh), every test writes NAME.out there, a summary
# (the last line of every test) goes to batch.summary
# (MKFS_XFS=path/to/mkfs.xfs adds XFS to discardtest.py, OLD_DAEMON=path/to/scubed3
# of version 0.4 adds the compatibility part of ivtest.py)
T=$(cd "$(dirname "$0")" && pwd)
export S3BUILD=$(pwd)
run() { name=$1; shift; "$@" > $name.out 2>&1; echo "$name: $(tail -n 1 $name.out | cut -c1-150)" >> batch.summary; }

: > batch.summary
sh $T/runall.sh; sh $T/runall2.sh; sh $T/runall0.sh; sh $T/runall3.sh
cat runall.out runall2.out runall0.out runall3.out | cut -c1-150 >> batch.summary
: > extra.out
for sd in 911 912 913 914; do
	timeout 1500 python3 -I -u $T/torture.py scubed3-crash 250 $sd tear > t.out 2>&1
	echo "[tear $sd] $(tail -n 1 t.out | cut -c1-80)" >> extra.out
	PARANOIA=2 timeout 1500 python3 -I -u $T/torture.py scubed3-crash 250 $sd tear > t.out 2>&1
	echo "[L2 tear $sd] $(tail -n 1 t.out | cut -c1-80)" >> extra.out
done
cat extra.out >> batch.summary
run paranoia python3 -u $T/paranoiatest.py scubed3-crash ./scubed3ctl 4
run discard python3 -u $T/discardtest.py scubed3 13
run killtest python3 -u $T/killtest.py scubed3 20 91
run killtest2 env DISCARD=1 python3 -u $T/killtest.py scubed3 10 92
run fstest python3 -u $T/fstest.py scubed3 8 93
run fsrepair python3 -u $T/fsrepair.py
run corrupt python3 -u $T/corrupttest.py scubed3 5 94
run ioerr python3 -u $T/ioerrtest.py scubed3-crash
run opener python3 -u $T/openertest.py scubed3 ./scubed3ctl 30
run trunc python3 -u $T/truncatetest.py scubed3
run config sh $T/configtest.sh ./scubed3 ./scubed3ctl
run socket sh $T/sockettest.sh ./scubed3 ./scubed3ctl
run luks sh $T/lukstest.sh ./scubed3 ./scubed3ctl
run keystore sh $T/keystoretest.sh ./scubed3 ./scubed3ctl
run rootswitch python3 -u $T/rootswitchtest.py scubed3 18
run initramfs sh $T/initramfstest.sh ./scubed3 ./scubed3ctl
run installer sh $T/installertest.sh ./scubed3 ./scubed3ctl
run valgrind sh $T/valgrindtest.sh ./scubed3 ./scubed3ctl
run helgrind env TOOL=helgrind sh $T/valgrindtest.sh ./scubed3 ./scubed3ctl
run size python3 -u $T/sizetest.py scubed3
run resizestat python3 -u $T/resizestat.py scubed3-crash 1500 11
run level0 python3 -u $T/level0test.py scubed3-crash 3
run level3 python3 -u $T/level3test.py scubed3-crash ./scubed3ctl 5
run churn1 python3 -u $T/churntest.py scubed3 60 11 1
run churn2 python3 -u $T/churntest.py scubed3 60 12 2
run churn3 python3 -u $T/churntest.py scubed3 10 13 3
run wipe python3 -u $T/wipetest.py scubed3-crash 14
run iv python3 -u $T/ivtest.py scubed3-crash 17 $OLD_DAEMON
run waitread2 python3 -u $T/waitreadtest.py scubed3 20 15 2
run waitread3 python3 -u $T/waitreadtest.py scubed3 30 16 3 640 0.5
run opentime python3 -u $T/opentime.py scubed3
run memoryless python3 -u $T/memoryless.py scubed3-crash 6000
run jstat ./jstat 200000
run indexfuzz python3 -u $T/indexfuzz.py scubed3 300 1
echo ALLDONE >> batch.summary
