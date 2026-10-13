#!/bin/sh
# sockettest.sh - the control socket: private directory (0700) and socket
# (0600), refusal of unsafe directories, no takeover of a running daemon,
# takeover of a stale socket, peer check, removal at exit
#
# usage: sockettest.sh DAEMON SCUBED3CTL (as root, user nobody must exist)
D=$1; C=$2; T=$(mktemp -d); M=/mnt/s3; S=/run/scubed3/control
fail() { echo "SOCKETTEST FAILED: $*"; exit 1; }
send() { python3 -c "
import socket,sys
s=socket.socket(socket.AF_UNIX)
try: s.connect(sys.argv[1])
except OSError as e: print('connect:', e.strerror); sys.exit()
s.sendall(b'static-info\nexit\n'); b=b''
while True:
    d=s.recv(9999)
    if not d: break
    b+=d
print(b.decode().split(chr(10))[0] if b else 'closed without answer')" $1; }
waitsock() { for i in $(seq 50); do [ -S $1 ] && return; sleep 0.1; done; fail "no socket $1"; }
mode() { stat -c '%a %U %F' $1; }
start() { $D -b $1 -f $M $2 > $T/log.$3 2>&1 & }
for i in 1 2; do head -c $((4<<22)) /dev/urandom > $T/base$i; done
mkdir -p $T/m2
fusermount -u -z $M 2>/dev/null

start $T/base1 "" 1; waitsock $S; P1=$!
[ "$(mode /run/scubed3)" = "700 root directory" ] || fail "dir: $(mode /run/scubed3)"
[ "$(mode $S)" = "600 root socket" ] || fail "socket: $(mode $S)"
echo "default: $(mode /run/scubed3) / $(mode $S)"
[ "$(send $S)" = OK ] || fail "connect as root"
$C -q -c static-info > /dev/null || fail "scubed3ctl default socket"
echo "as nobody: $(runuser -u nobody -- python3 -c "
import socket
s=socket.socket(socket.AF_UNIX)
try: s.connect('$S'); print('connected')
except OSError as e: print(e.strerror)")"

# a second daemon must not take over the socket of a running one
$D -b $T/base2 -f $T/m2 > $T/log.2 2>&1; grep -q "another scubed3 process is listening" $T/log.2 || fail "takeover: $(tail -1 $T/log.2)"
[ "$(send $S)" = OK ] || fail "first daemon lost its socket"
echo "second daemon: $(tail -1 $T/log.2)"

# peer check (normally the directory already keeps others out)
chmod 711 /run/scubed3; chmod 666 $S
r=$(runuser -u nobody -- python3 -c "
import socket
s=socket.socket(socket.AF_UNIX); s.connect('$S')
try:
    s.sendall(b'static-info\nexit\n')
    print('answer' if s.recv(99) else 'closed without answer')
except (ConnectionResetError, BrokenPipeError): print('closed without answer')")
chmod 700 /run/scubed3; chmod 600 $S
[ "$r" = "closed without answer" ] || fail "peer check: $r"
grep -q "refused connection on the control socket from uid 65534" $T/log.1 || fail "no refusal logged"
echo "peer check: nobody's connection $r"

# stale socket after kill -9 is taken over
kill -9 $P1; wait $P1 2>/dev/null; fusermount -u -z $M
[ -S $S ] || fail "socket gone after kill -9?"
start $T/base1 "" 3; P3=$!; sleep 1
[ "$(send $S)" = OK ] || fail "stale socket not taken over: $(tail -2 $T/log.3)"
echo "stale socket taken over"
fusermount -u $M; wait $P3
[ -e $S ] && fail "socket not removed at exit"
echo "socket removed at exit"

# unsafe or unusable places are refused
mkdir -m 755 $T/open; ln -s $T/open $T/link; mkdir -m 700 $T/priv; touch $T/priv/file
for a in /tmp/control $T/open/control $T/link/control $T/priv/file relative/control; do
	$D -b $T/base1 -f $M -a $a > $T/log.4 2>&1 && fail "-a $a accepted"
	echo "-a $a: $(tail -1 $T/log.4 | sed 's/^[^:]*:fatal://')"
done

# a new private directory is made, scubed3ctl -a
start $T/base1 "-a $T/new/control" 5; waitsock $T/new/control; P5=$!
[ "$(mode $T/new)" = "700 root directory" ] || fail "new dir: $(mode $T/new)"
$C -q -a $T/new/control -c static-info > /dev/null || fail "scubed3ctl -a"
echo "-a $T/new/control: directory created, $(mode $T/new)"
fusermount -u $M; wait $P5
rm -rf $T
echo "SOCKETTEST OK"
