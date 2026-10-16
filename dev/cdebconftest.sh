#!/bin/sh
# cdebconftest.sh - the passphrase questions of partman-scubed3
# (s3_ask_passphrase) with the real cdebconf of the debian-installer (its
# text frontend, on a pty): every debconf command sets RET (db_set gives
# "value set"), installertest.sh only has stand-ins
#
# 1. two equal passphrases are taken
# 2. two different ones: the error, then two equal ones are taken
# 3. an empty one: the error (it is never generated), a short one: the
#    question about a weak passphrase (no), then a long one is taken
# 4. a short one, weak passphrase yes: it is taken
#
# usage: cdebconftest.sh PREFIX SRC (as root: it writes /etc/cdebconf.conf,
# an existing one is kept and put back)
#
# PREFIX is where cdebconf is installed, SRC its source (the shell library
# src/client/confmodule is not installed by make install), built with the
# text frontend (it needs libdebian-installer):
#
#	libdebian-installer$ autoreconf -fi && ./configure --disable-check \
#		--prefix=PREFIX && make && make install
#	cdebconf$ autoreconf -fi && PKG_CONFIG_PATH=PREFIX/lib/pkgconfig \
#		./configure --prefix=PREFIX --with-frontend=text \
#		--without-textwrap
#	cdebconf$ make CPPFLAGS=-IPREFIX/include LDFLAGS="-L$PWD/src \
#		-LPREFIX/lib" && make install
I=$(realpath $1); SRC=$(realpath $2); T=$(mktemp -d)
DEV=$(cd "$(dirname "$0")" && pwd)
P=$DEV/../contrib/debian-installer/partman-scubed3
M=$I/libexec/cdebconf
[ -x $M/debconf ] || { echo "CDEBCONFTEST FAILED: no $M/debconf"; exit 1; }
[ -f /etc/cdebconf.conf ] && cp -p /etc/cdebconf.conf $T/cdebconf.conf.orig
cleanup() {
	if [ -f $T/cdebconf.conf.orig ]; then cp -p $T/cdebconf.conf.orig /etc/cdebconf.conf
	else rm -f /etc/cdebconf.conf; fi
	rm -rf $T
}
fail() { echo "CDEBCONFTEST FAILED: $1"; cleanup; exit 1; }
ok() { echo "   ok: $1"; }

export LD_LIBRARY_PATH=$I/lib:$M DEBIAN_FRONTEND=text
unset DEBIAN_PRIORITY DEBIAN_HAS_FRONTEND DEBCONF_REDIR
mkdir $T/db
cat > /etc/cdebconf.conf << EOF
global {
  module_path {
    frontend "$M/frontend";
    database "$M/db";
  };
  default {
    frontend "default_fe";
    template "templatedb";
    config "configdb";
  };
};
frontend {
  instance "default_fe" {
    driver "text";
  };
};
template {
  instance "templatedb" {
    driver "rfc822db";
    path "$T/db/templates.dat";
  };
};
config {
  instance "config_gen_db" {
    driver "rfc822db";
    path "$T/db/questions.dat";
    reject_types { "password"; };
  };
  instance "config_passwd_db" {
    driver "rfc822db";
    path "$T/db/passwords.dat";
    mode 600;
    accept_types { "password"; };
  };
  instance "configdb" {
    driver "stack";
    stack { "config_gen_db"; "config_passwd_db"; };
  };
};
EOF
$M/debconf-loadtemplate partman-scubed3 $P/templates || fail "debconf-loadtemplate"
sed "s|/usr/lib/cdebconf|$M|g" $SRC/src/client/confmodule > $T/confmodule 2> /dev/null ||
	fail "no $SRC/src/client/confmodule"
sed -n '/^s3_ask_passphrase () {/,/^}/p' $P/lib/scubed3-base.sh > $T/ask.sh
cat > $T/ask << EOF
#!/bin/sh
. $T/confmodule
. $T/ask.sh
db_capb backup align
db_metaget partman-scubed3/text/boot_passphrase description
s3_ask_passphrase "\$RET" yes
echo "rc=\$? PASS=[\$PASS]" > $T/result
EOF
chmod +x $T/ask

# the answers on a pty: "PROMPT=>ANSWER|..."
cat > $T/drive.py << 'EOF'
import os, pty, sys, time, select
pid, fd = pty.fork()
if pid == 0:
	os.execv(sys.argv[1], sys.argv[1:])
out = b""
def read(t):
	global out
	r, _, _ = select.select([fd], [], [], t)
	if not r: return False
	try: d = os.read(fd, 4096)
	except OSError: return None
	if not d: return None
	out += d
	return True
for step in os.environ["STEPS"].split("|"):
	pat, ans = step.split("=>", 1)
	end = time.time() + 10
	while pat.encode() not in out:
		if read(0.2) is None or time.time() > end:
			sys.stdout.write(out.decode(errors="replace"))
			print("\nno prompt " + pat); os.kill(pid, 9); sys.exit(1)
	sys.stdout.write(out.decode(errors="replace"))
	out = out[out.index(pat.encode()) + len(pat):]
	os.write(fd, ans.encode() + b"\n")
while read(2): pass
sys.stdout.write(out.decode(errors="replace"))
os.waitpid(pid, 0)
EOF
run() {
	rm -f $T/result
	STEPS=$1 timeout 120 python3 -I $T/drive.py $M/debconf $T/ask > $T/out 2>&1
	[ "$(cat $T/result 2> /dev/null)" = "rc=0 PASS=[$2]" ] ||
		fail "$1: $(cat $T/result 2> /dev/null), $(tail -n 5 $T/out)"
}

echo "1. two equal passphrases"
run 'Passphrase:=>secret one|verify:=>secret one' "secret one"
grep -aq 'ERROR' $T/out && fail "an error: $(grep -a ERROR $T/out)"
ok "taken"
echo "2. two different passphrases"
run 'Passphrase:=>secret one|verify:=>secret two|continue]=>|Passphrase:=>secret three|verify:=>secret three' "secret three"
[ "$(grep -ac 'ERROR: Passphrase input error' $T/out)" = 1 ] || fail "no error: $(cat $T/out)"
ok "the error, then two equal ones are taken"
echo "3. an empty one, a weak one"
run 'Passphrase:=>|verify:=>|continue]=>|Passphrase:=>short|verify:=>short|default=2>=>2|Passphrase:=>a long one|verify:=>a long one' "a long one"
[ "$(grep -ac 'ERROR: Empty passphrase' $T/out)" = 1 ] || fail "no error: $(cat $T/out)"
grep -aq 'Use weak passphrase' $T/out || fail "no question about a weak passphrase"
ok "an empty one: the error (not generated), a short one: weak passphrase (no), a long one is taken"
echo "4. a weak one, yes"
run 'Passphrase:=>short|verify:=>short|default=2>=>1' "short"
ok "taken"
cleanup
echo "CDEBCONFTEST OK"
