/* ctl_common.c - what scubed3ctl and keystorectl share (see ctl_common.h)
 *
 * Copyright (C) 2019  Rik Snel <rik@snel.it>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>
#include <signal.h>
#include <limits.h>
#include <ctype.h>
#include <strings.h>
#undef NDEBUG /* we need sanity checking */
#include <termios.h>
#include <assert.h>
#include "config.h"
#ifndef NO_READLINE
#include <readline/readline.h>
#include <readline/history.h>
#endif
#include "verbose.h"
#include "control.h"
#include "gcry.h"
#include "util.h"
#include "keystore.h"
#include "ctl_common.h"

/* the key of a partition in the key store is a key of a partition */
_Static_assert(KS_KEY == KEY_LENGTH, "the keys of the key store have KEY_LENGTH bytes");

const char *command_options[MAX_COMMANDS];
int no_commands = 0;
// PBKDF2 requires salt
const char *kdf_salt = DEFAULT_KDF_SALT;
const char *control_socket = CONTROL_SOCKET;
unsigned long kdf_iterations = DEFAULT_KDF_ITERATIONS;
int recommended_paranoia = DEFAULT_PARANOIA;
static const char *config_file = NULL; /* NULL: DEFAULT_CONFIG_FILE, if it exists */
char luks_prefix[65] = "scubed3-";

/* the key store (see keystore.h), read when it is used */
keystore_t ks = { .path = DEFAULT_KEYSTORE_FILE };
int assume_yes = 0;
/* -k: the passphrases of the key store are given as their keys (64
 * hexadecimal digits, from keystorectl derive): a script (the
 * debian-installer) computes the KDF of a passphrase once, not every time
 * it runs scubed3ctl or keystorectl */
int key_input = 0;
int interactive = 0;

/* in non-interactive mode, the exit status of
 * scubed3ctl must match the exit status of the
 * command that was requested using -c */
int exit_status = EXIT_SUCCESS;

result_t result;

static int vvar_system(const char *format, va_list ap) {
	char *string;
	ssize_t len;
	int ret;

	if ((len = vasprintf(&string, format, ap)) == -1) {
		ERROR("vasprintf: %s", strerror(errno));
		return -1;
	}

	ret = system(string);

	wipememory(string, len);

	free(string);
	if (ret) exit_status = EXIT_FAILURE;

	return ret;
}

int var_system(const char *format, ...) {
	va_list ap;
	int ret;

	va_start(ap, format);
	ret = vvar_system(format, ap);
	va_end(ap);

	return ret;
}

static int vwrite_line(int s, const char *format, va_list ap) {
	char *string;
	ssize_t sent = 0, len, n;
	if ((len = vasprintf(&string, format, ap)) == -1) {
		ERROR("vasprintf: %s", strerror(errno));
		return -1;
	}

	//VERBOSE("send: %s", string);

	string[len++] = '\n';

	do {
		n = send(s, string + sent, len - sent, 0);
		if (n == 0) {
			wipememory(string, len);
			FATAL("send: connection reset by peer");
		}
		if (n < 0) {
			wipememory(string, len);
			FATAL("send: %s", strerror(errno));
		}
		sent += n;
	} while (sent < len);

	wipememory(string, len);

	free(string);
	return 0;
}

/* the daemon serves one scubed3ctl at a time: the first command waits with
 * a timeout (SO_RCVTIMEO), if it expires a message says why we wait */
static int wait_note = 0;

int do_server_command(int s, int echo, char *format, ...) {
	int ret = 0, buf_len = 0, start = 0, i, done = 0;
	int status_known = 0;
	ssize_t  n;
	va_list ap;

	va_start(ap, format);
	ret = vwrite_line(s, format, ap);
	va_end(ap);

	if (ret) return ret;

	result.status = 0;
	result.argc = 0;
	do {
		if (buf_len == BUF_SIZE) FATAL("response too long");
		n = recv(s, result.buf + buf_len, BUF_SIZE - buf_len, 0);
		if (n == 0) FATAL("recv: connection reset by peer");
		if (n < 0 && errno == EINTR) continue;
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) &&
				wait_note) {
			struct timeval tv = { 0, 0 };

			fprintf(stderr, "%s: waiting: the scubed3 daemon "
					"serves another scubed3ctl (an s3> "
					"prompt?),\nthis one goes on when that "
					"one quits\n", exec_name);
			wait_note = 0;
			if (setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv,
						sizeof(tv)))
				FATAL("setsockopt: %s", strerror(errno));
			continue;
		}
		if (n < 0) FATAL("recv: %s", strerror(errno));

		for (i = buf_len; i < buf_len + n; i++) {
			if (result.buf[i] == '\n') {
				result.buf[i] = '\0';
				if (*(result.buf + start) == '.' &&
						i - start == 1) {
					done = 1;
					if (!status_known)
						WARNING("message terminates "
								"without "
								"known "
								"status");
					break;
				}
				if (status_known) {
					if (echo) printf("%s\n",
							result.buf + start);
					if (result.argc == MAX_RESULT_LINES)
						FATAL("too many lines "
								"in response");
					result.argv[result.argc++] =
						result.buf + start;
				} else if (!strcmp(result.buf + start,
							"ERR") ||
						!strcmp(result.buf + start,
							"OK")) {
					status_known = 1;
					if (!strcmp(result.buf + start,
								"ERR")) {
						/* the caller sees it in
						 * result.status */
						result.status = -1;
						exit_status = EXIT_FAILURE;
					}
				} else {
					ERROR("malformed response, "
						"expected OK or ERR");
					return -1;
				}

				start = i + 1;
			}
		}

		buf_len += n;
	} while (!done);

	return 0;
}

static int do_local_command(ctl_priv_t *priv,
		ctl_command_t *cmnd, char *format, ...) {
	int argc = 0, ret;
	char *args;
	char *argv[MAX_ARGC+1];
	va_list ap;

	va_start(ap, format);
	ret = vasprintf(&args, format, ap);
	va_end(ap);

	if (ret == -1) return -1;

	argv[argc] = args;

	assert(argv[argc]);

	while (*argv[argc] != '\0') {
		while (*argv[argc] == ' ') argv[argc]++;

		argc++;

		if (argc > MAX_ARGC) {
			printf("too many arguments, discarding command\n");
			free(args);
			return 0;
		}

		argv[argc] = argv[argc-1];

		while (*argv[argc] != '\0' && *argv[argc] != ' ') argv[argc]++;

		if (argv[argc] == argv[argc-1]) {
			argc--; // last arg empty
			break;
		}

		if (*argv[argc] == ' ') *argv[argc]++ = '\0';
	}

	if (*argv[0] == '\0' && argc > 0) argc--;
	if (argc < cmnd->argc || argc > cmnd->argc + cmnd->optargc) {
		printf("usage: %s%s\n", cmnd->head.key, cmnd->usage);
		free(args);
		return 0;
	}
	argv[argc] = NULL;

	ret = (*cmnd->command)(priv, argv);
	free(args);
	return ret;
}

#ifdef NO_READLINE
/* without readline (configure --without-readline, for the
 * debian-installer): a line without editing, the result is malloc'ed, NULL
 * at EOF */
static char *readline(const char *prompt) {
	char *line = NULL;
	size_t n = 0;
	ssize_t len;

	if (prompt) {
		printf("%s", prompt);
		fflush(stdout);
	}
	if ((len = getline(&line, &n, stdin)) == -1) {
		free(line);
		return NULL;
	}
	if (len > 0 && line[len - 1] == '\n') line[len - 1] = '\0';

	return line;
}
#endif

char *ctl_readline(const char *prompt) {
	return readline(prompt);
}

/* killed (Ctrl-C, kill) while echo is off: the terminal gets it back; a
 * key store that is written is not replaced (its temporary file is
 * removed), the old one stays */
static struct termios saved_tty;
static volatile sig_atomic_t tty_fd = -1;

static void on_signal(int sig) {
	if (tty_fd != -1) (void) tcsetattr(tty_fd, TCSANOW, &saved_tty);
	ks_signal_cleanup();
	signal(sig, SIG_DFL);
	raise(sig);
}

/* a line (without echo on a terminal), with its newline (one is added if
 * the last line has none), *n is its length; -1 at the end of the input
 * (also Ctrl-D on a terminal) */
static int my_getpass(char **lineptr, size_t *n, FILE *stream) {
	struct termios old, new;
	ssize_t len;

	if (!isatty(fileno(stream))) {
		len = getline(lineptr, n, stream);
	} else {
		/* Turn echoing off and fail if we can't. */
		if (tcgetattr(fileno(stream), &old) != 0) return -1;

		new = old;
		new.c_lflag &= ~ECHO;
		saved_tty = old;
		tty_fd = fileno(stream);
		if (tcsetattr(fileno(stream), TCSAFLUSH, &new) != 0) {
			tty_fd = -1;
			return -1;
		}

		/* Read the password. */
		len = getline(lineptr, n, stream);

		/* Restore terminal. */
		(void) tcsetattr(fileno(stream), TCSAFLUSH, &old);
		tty_fd = -1;

		printf("\n");
	}
	if (len <= 0) return -1;

	/* the last line may have no newline */
	if ((*lineptr)[len - 1] != '\n') {
		char *tmp;
		if (!(tmp = realloc(*lineptr, len + 2))) return -1;
		*lineptr = tmp;
		(*lineptr)[len++] = '\n';
		(*lineptr)[len] = '\0';
	}
	*n = len;

	return 0;
}

/* ask a passphrase (twice if verify), NULL if it could not be read or the
 * two do not match; *len is its length (without the newline) */
char *ask_passphrase(const char *prompt, int verify, size_t *len) {
	char *pw = NULL, *pw2 = NULL;
	size_t pw_len, pw2_len;

	printf("%s", prompt);
	if (my_getpass(&pw, &pw_len, stdin) == -1) {
		/* (getline allocates a buffer at the end of the input too) */
		free(pw);
		ERROR("unable to get password");
		return NULL;
	}
	if (verify) {
		printf("Verify passphrase: ");
		if (my_getpass(&pw2, &pw2_len, stdin) == -1) {
			wipememory(pw, pw_len);
			free(pw);
			free(pw2);
			ERROR("unable to get password for verification");
			return NULL;
		}
		if (pw_len != pw2_len || strcmp(pw, pw2)) {
			wipememory(pw, pw_len);
			wipememory(pw2, pw2_len);
			free(pw);
			free(pw2);
			printf("passphrases do not match\n");
			exit_status = EXIT_FAILURE;
			return NULL;
		}

		wipememory(pw2, pw2_len);
		free(pw2);
	}

	assert(pw_len > 0);
	*len = pw_len - 1; /* the newline */
	pw[*len] = '\0';

	return pw;
}

void free_passphrase(char *pw, size_t len) {
	if (!pw) return;
	wipememory(pw, len);
	free(pw);
}

/* the key of a passphrase: PBKDF2-SHA256 with the salt of the
 * configuration file, and suffix (the key store uses another key than a
 * partition with the same passphrase) */
void derive_key(const char *pw, size_t len, const char *suffix,
		unsigned char *key) {
	char *salt;

	/* when the number of iterations is high, let
	 * the user know that computing the key
	 * may take some time, the threshold is somewhat
	 * arbitrary, but it seems useful the let the user
	 * know that computing the KDF may take some time */
	if (kdf_iterations >= 10000)
		VERBOSE("computing %ld iterations of %s(%s), please wait...",
				kdf_iterations,
				DEFAULT_KDF_FUNCTION,
				DEFAULT_KDF_HASH);

	if (asprintf(&salt, "%s%s", kdf_salt, suffix) == -1)
		FATAL("out of memory");
	gcry_call(kdf_derive, pw, len, GCRY_KDF_PBKDF2,
			gcry_md_map_name(DEFAULT_KDF_HASH), salt, strlen(salt),
			kdf_iterations, KEY_LENGTH, key);
	free(salt);
}

/* the name of unlocked view v (in keystorectl the name given at unlock,
 * default iNN: NN is its slot) */
const char *vname(int v) {
	return ks.views[v]->name;
}

/* the entry of partition name in the unlocked views of the key store,
 * NULL if it is in none; if two views have other keys for it (two
 * partitions with the same name, for example a root partition you can
 * admit and a hidden one: only in keystorectl, scubed3ctl has one view),
 * the name is ambiguous: an error, NULL and *ambiguous = 1, one of the
 * views must be locked first */
ks_entry_t *ks_lookup(const char *name, int *ambiguous) {
	ks_entry_t *e;
	int view, other;

	*ambiguous = 0;
	if (!(e = ks_find(&ks, -1, name, &view))) return NULL;
	if ((other = ks_conflict(&ks, view, name)) >= 0) {
		printf("\"%s\" is in view %s and in view %s of the key store, "
				"with other keys: two\npartitions with the same "
				"name; lock one of the views first (lock VIEW)\n",
				name, vname(view), vname(other));
		exit_status = EXIT_FAILURE;
		*ambiguous = 1;
		return NULL;
	}

	return e;
}

int yesno(const char *prompt) {
	char *line, *answer;
	if (assume_yes >= 3) {
		printf("%s Yes (assumed)\n", prompt);
		return 1;
	}
label:
	/* no answer (EOF) is No */
	if (!(line = readline(prompt))) return 0;
	answer = line;
	while (*answer == ' ') answer++;

	if (answer[0] == '\0' || !strcmp("No", answer)) {
		free(line);
		return 0; /* No */
	}
	if (strcmp("Yes", answer)) {
		prompt = "Please answer Yes or No. [No] ";
		free(line);
		goto label;
	}
	free(line);
	return 1; /* Yes */
}

/* for an operation that cannot be undone: the answer must be "yes" (the
 * whole word), anything else (also nothing) is no */
int typed_yes(const char *prompt) {
	char *answer;
	int ret;

	if (assume_yes >= 3) {
		printf("%s yes (assumed)\n", prompt);
		return 1;
	}
	if (!(answer = readline(prompt))) return 0;
	ret = !strcmp(answer, "yes");
	free(answer);

	return ret;
}

void warning(void) {
	printf("---WARNING---WARNING---WARNING---WARNING"
			"---WARNING---WARNING---WARNING---\n");
}


/* a decimal int; strtol gives a long, a value that does not fit in an int
 * is out of range (it would wrap around) */
int parse_int(int *r, const char *in) {
	char *end;
	long ret;

	errno = 0;
	ret = strtol(in, &end, 10);

	if (errno == ERANGE || ret < INT_MIN || ret > INT_MAX) {
		printf("integer out of range\n");
		exit_status = EXIT_FAILURE;
		return -1;
	}
	if (end == in || *end != '\0') {
		printf("unable to parse ->%s<-\n", in);
		exit_status = EXIT_FAILURE;
		return -1;
	}
	*r = ret;

	return 0;
}

/* does the partition have a LUKS header? (it is not counted as an error if
 * it has not) */
int is_luks(ctl_priv_t *priv, const char *name) {
	char *cmd;
	int ret;

	if (asprintf(&cmd, "cryptsetup isLuks %s/%s 2>/dev/null",
				priv->mountpoint, name) == -1)
		FATAL("out of memory");
	ret = system(cmd);
	free(cmd);

	return !ret;
}

/* run a cryptsetup command with data (a key) on its standard input, the
 * key is never written to a file */
static int vcryptsetup_with_data(const void *data, size_t len,
		const char *format, va_list ap) {
	char *cmd;
	FILE *fp;
	int ret;

	if (vasprintf(&cmd, format, ap) == -1) FATAL("out of memory");
	if (!(fp = popen(cmd, "w"))) FATAL("popen: %s", strerror(errno));
	free(cmd);
	if (len && fwrite(data, len, 1, fp) != 1)
		WARNING("writing the key to cryptsetup: %s", strerror(errno));
	if ((ret = pclose(fp))) exit_status = EXIT_FAILURE;

	return ret;
}

int cryptsetup_with_data(const void *data, size_t len,
		const char *format, ...) {
	va_list ap;
	int ret;

	va_start(ap, format);
	ret = vcryptsetup_with_data(data, len, format, ap);
	va_end(ap);

	return ret;
}

/* with the key of the LUKS volume of e on its standard input (the key file
 * of a keyslot, --key-file=-, or the volume key, --volume-key-file
 * /dev/stdin: see KS_LUKS_VK) */
int cryptsetup_with_key(ks_entry_t *e, const char *format, ...) {
	va_list ap;
	int ret;

	va_start(ap, format);
	ret = vcryptsetup_with_data(e->luks, KS_LUKS, format, ap);
	va_end(ap);

	return ret;
}

/* overwrite the memory file fd (len bytes) with zeroes */
void wipe_fd(int fd, size_t len) {
	char zero[256] = { };
	size_t n;

	if (lseek(fd, 0, SEEK_SET)) return;
	while (len) {
		n = len < sizeof(zero)?len:sizeof(zero);
		if (write(fd, zero, n) != (ssize_t)n) break;
		len -= n;
	}
}

/* the key store (see keystore.h): it is read when it is first used */
int ks_open(void) {
	if (!ks_read(&ks)) return 1;
	if (errno == ENOENT) printf("there is no key store %s (keystorectl "
			"create makes one, -K uses another file)\n", ks.path);
	else printf("unable to read the key store %s: %s\n", ks.path,
			errno == EINVAL?"it has the wrong size":strerror(errno));
	exit_status = EXIT_FAILURE;

	return 0;
}

/* 64 hexadecimal digits to a key of 32 bytes, 0 on success */
static int ks_parse_key(const char *hex, size_t len, unsigned char *key) {
	int i, j, d;

	if (len != 64) return -1;
	for (i = 0; i < 64; i++) {
		j = hex[i];
		if (j >= '0' && j <= '9') d = j - '0';
		else if (j >= 'a' && j <= 'f') d = j - 'a' + 10;
		else if (j >= 'A' && j <= 'F') d = j - 'A' + 10;
		else return -1;
		if (i&1) key[i/2] |= d;
		else key[i/2] = d<<4;
	}

	return 0;
}

/* the key of the key store for a passphrase (the suffix makes it another
 * key than that of a partition with the same passphrase); with -k the
 * input is the key itself (see keystorectl derive) */
int ks_ask_key(const char *prompt, int verify, unsigned char *key) {
	char *pw;
	size_t len;

	if (!(pw = ask_passphrase(prompt, verify, &len))) return -1;
	if (!len) {
		free_passphrase(pw, len);
		return 0;
	}
	if (key_input) {
		if (ks_parse_key(pw, len, key)) {
			free_passphrase(pw, len);
			wipememory(key, 32);
			printf("not a key (-k: 64 hexadecimal digits, see "
					"keystorectl derive)\n");
			exit_status = EXIT_FAILURE;
			return -1;
		}
	} else derive_key(pw, len, "/keystore", key);
	free_passphrase(pw, len);

	return 1;
}

int ks_count(int view) {
	int i, n = 0;

	for (i = 0; i < KS_ENTRIES; i++)
		if (ks.views[view]->e[i].flags&KS_USED) n++;

	return n;
}

int ks_valid_name(const char *name) {
	const char *p;

	if (strlen(name) >= KS_NAME) {
		printf("the name is too long, at most %d characters\n",
				KS_NAME - 1);
		return 0;
	}
	for (p = name; *p; p++) if (!isalnum((unsigned char)*p) && *p != '_') {
		printf("a name may only contain letters, digits and the "
				"underscore\n");
		return 0;
	}

	return 1;
}

void ks_list(int v) {
	char def[8];
	int i, other;

	printf("view %s:%s", vname(v), ks_count(v)?"":" (empty)");
	/* a name of its own: the slot (its default name) too */
	snprintf(def, sizeof(def), "i%02d", ks.views[v]->slot);
	if (strcmp(vname(v), def)) printf(" [slot %s]", def);
	printf("\n");
	for (i = 0; i < KS_ENTRIES; i++) {
		ks_entry_t *e = &ks.views[v]->e[i];
		if (!(e->flags&KS_USED)) continue;
		printf("  %s%s", e->name,
				(e->flags&KS_HAS_LUKS)?" [LUKS]":"");
		if ((other = ks_conflict(&ks, v, e->name)) >= 0)
			printf(" (other keys in view %s: lock one of them to "
					"use it)", vname(other));
		printf("\n");
	}
}

/* a line from the user, without spaces at both ends, NULL at the end */
char *ask_line(const char *prompt) {
	char *l = readline(prompt), *p, *q;

	if (!l) return NULL;
	for (p = l; isspace((unsigned char)*p); p++);
	for (q = p + strlen(p); q > p && isspace((unsigned char)q[-1]); q--);
	*q = '\0';
	memmove(l, p, q - p + 1);

	return l;
}

int ask_yn(const char *prompt) {
	char *a = ask_line(prompt);
	int ret = a && (*a == 'y' || *a == 'Y');

	free(a);

	return ret;
}

int ctl_call(ctl_priv_t *priv, char *command) {
	char *tmp;
	ctl_command_t *cmnd;

	/* strip leading spaces */
	while (*command == ' ' ) command++;

	/* check if the command is local */
	if ((tmp = strchr(command, ' '))) *tmp = '\0'; // temporary terminator
	cmnd = hashtbl_find_element_bykey(&priv->c, command);
	if (tmp) *tmp = ' '; // replace space
	else tmp="";

	/* execute */
	if (cmnd) return do_local_command(priv, cmnd, "%s", tmp);
	if (priv->forward) return do_server_command(priv->s, 1, "%s", command);
	if (tmp && *tmp) *tmp = '\0';
	printf("unknown command \"%s\" (help shows the commands)\n", command);
	exit_status = EXIT_FAILURE;

	return 0;
}

/* strip whitespace at both ends of a string, in place */
static char *strip(char *str) {
	char *end;

	while (isspace((unsigned char)*str)) str++;
	end = str + strlen(str);
	while (end > str && isspace((unsigned char)end[-1])) *--end = '\0';

	return str;
}

/* read the KDF parameters from the configuration file, it has lines
 * KEY=VALUE, empty lines and lines starting with # are ignored
 *
 * kdf=PBKDF2-SHA256   the key derivation function (the only one supported)
 * salt=TEXT           the salt (whitespace at both ends is ignored)
 * iterations=NUMBER   the number of iterations
 * paranoia=LEVEL      the recommended paranoia level (0..3, default 2)
 * luks_prefix=TEXT    the start of the names of the LUKS volumes
 *                     (/dev/mapper/TEXTNAME), default scubed3-
 *
 * values that are given on the command line take precedence; returns 0
 * if the file does not exist and must_exist is 0, 1 if it was read */
static int read_config(const char *path, int must_exist, int salt_set,
		int iterations_set) {
	FILE *fp;
	char line[1024], *key, *value, *endptr;
	int lineno = 0, have_kdf = 0, have_salt = 0, have_iterations = 0;
	int have_paranoia = 0;
	unsigned long iterations;

	if (!(fp = fopen(path, "r"))) {
		if (errno == ENOENT && !must_exist) return 0;
		FATAL("unable to open configuration file %s: %s", path,
				strerror(errno));
	}

	while (fgets(line, sizeof(line), fp)) {
		lineno++;
		if (!strchr(line, '\n') && !feof(fp))
			FATAL("%s:%d: line too long", path, lineno);

		key = strip(line);
		if (!*key || *key == '#') continue;

		if (!(value = strchr(key, '=')))
			FATAL("%s:%d: expected KEY=VALUE", path, lineno);
		*value++ = '\0';
		key = strip(key);
		value = strip(value);

		if (!*value) FATAL("%s:%d: empty value for %s", path, lineno,
				key);

		if (!strcmp(key, "kdf")) {
			if (have_kdf++) FATAL("%s:%d: kdf specified twice",
					path, lineno);
			if (strcasecmp(value, DEFAULT_KDF_FUNCTION "-"
						DEFAULT_KDF_HASH))
				FATAL("%s:%d: unsupported kdf \"%s\", only "
						DEFAULT_KDF_FUNCTION "-"
						DEFAULT_KDF_HASH " is "
						"supported", path, lineno,
						value);
		} else if (!strcmp(key, "salt")) {
			if (have_salt++) FATAL("%s:%d: salt specified twice",
					path, lineno);
			if (!salt_set) kdf_salt = estrdup(value);
		} else if (!strcmp(key, "iterations")) {
			if (have_iterations++) FATAL("%s:%d: iterations "
					"specified twice", path, lineno);
			errno = 0;
			iterations = strtoul(value, &endptr, 10);
			if (*endptr || errno || !iterations ||
					iterations > UINT32_MAX)
				FATAL("%s:%d: invalid number of iterations "
						"\"%s\"", path, lineno, value);
			if (!iterations_set) kdf_iterations = iterations;
		} else if (!strcmp(key, "paranoia")) {
			if (have_paranoia++) FATAL("%s:%d: paranoia specified "
					"twice", path, lineno);
			if (strlen(value) != 1 || *value < '0' || *value > '3')
				FATAL("%s:%d: invalid paranoia level \"%s\", it "
						"must be 0, 1, 2 or 3", path,
						lineno, value);
			recommended_paranoia = *value - '0';
		} else if (!strcmp(key, "luks_prefix")) {
			const char *c;
			if (strlen(value) >= sizeof(luks_prefix))
				FATAL("%s:%d: luks_prefix is too long", path,
						lineno);
			for (c = value; *c; c++)
				if (!isalnum((unsigned char)*c) && *c != '-' &&
						*c != '_' && *c != '.')
					FATAL("%s:%d: luks_prefix may only have "
							"letters, digits, '-', '_' "
							"and '.'", path, lineno);
			strcpy(luks_prefix, value);
		} else FATAL("%s:%d: unknown key \"%s\"", path, lineno, key);
	}

	if (ferror(fp)) FATAL("error reading configuration file %s: %s",
			path, strerror(errno));

	fclose(fp);

	return 1;
}

/* the options, the configuration file (the KDF parameters: command line,
 * configuration file, defaults), mlockall, libgcrypt, the signals */
void ctl_init(int argc, char **argv, void (*help)(void)) {
	char *endptr;
	int opt, salt_set = 0, iterations_set = 0;

	/* since we have options to enable verbose and debug, it
	 * makes no sense to enable them by default */
	verbose = 0;
	debug = 0;
	verbose_init(argv[0]);

	opterr = 0;
	while ((opt = getopt(argc, argv, "+C:s:i:a:hYc:vdqK:k")) != -1) {
		switch (opt) {
			case 'q':
				quiet = 1;
				break;
			case 'v':
				verbose = 1;
				break;
			case 'd':
				debug = 1;
				break;
			case 'h':
				help();
				exit(0);
			case 'c':
				if (no_commands == MAX_COMMANDS) FATAL("at most %d -c options", MAX_COMMANDS);
				if (strlen(optarg) == 0) FATAL("command may not be empty");
				command_options[no_commands++] = optarg;
				break;
			case 'K':
				if (strlen(optarg) == 0) FATAL("key store file name must not be empty");
				ks.path = optarg;
				break;
			case 'k':
				key_input = 1;
				break;
			case 'a':
				if (strlen(optarg) == 0) FATAL("socket address may not be empty");
				control_socket = optarg;
				break;
			case 'C':
				if (strlen(optarg) == 0) FATAL("config file name must not be empty");
				config_file = optarg;
				break;
			case 's':
				if (strlen(optarg) == 0) FATAL("salt must not be empty");
				kdf_salt = optarg;
				salt_set = 1;
				break;
			case 'i':
				kdf_iterations = strtoul(optarg, &endptr, 10);
				if (*endptr != '\0') FATAL("error in converting %s to unsigned long", optarg);
				iterations_set = 1;
				break;
			case 'Y':
				VERBOSE("assuming yes");
				assume_yes++;
				break;
			case '?':
				FATAL("unrecognized option %c, use -h for help", optopt);
			default:
				assert(0);
		}
	}
	if (optind != argc) FATAL("unrecognized argument(s), use -h for help");
	interactive = !no_commands;

	assert(!strcmp("SHA256", DEFAULT_KDF_HASH));
	assert(!strcmp("PBKDF2", DEFAULT_KDF_FUNCTION));

	if (read_config(config_file?config_file:DEFAULT_CONFIG_FILE,
				config_file != NULL, salt_set, iterations_set))
		VERBOSE("KDF parameters read from %s, keep a backup of it, "
				"without it your partitions cannot be opened",
				config_file?config_file:DEFAULT_CONFIG_FILE);
	else VERBOSE("no configuration file %s, using the default KDF "
			"parameters", DEFAULT_CONFIG_FILE);

	if (salt_set && strcmp(DEFAULT_KDF_SALT, kdf_salt))
		WARNING("using custom value for salt, if you forget your salt, "
				"you may lose access to your devices");
	if (iterations_set && kdf_iterations != DEFAULT_KDF_ITERATIONS)
		WARNING("using custom value for iterations, if you forget this "
				"value, you may lose access to your devices");
	if (kdf_iterations < 1000000) {
		WARNING("low number of KDF iterations < 1000000, "
				"this is bad for security");
	}
	assert(DEFAULT_KDF_ITERATIONS == 16*1024*1024);
	assert(!strcmp("CBC_ESSIV(AES256)", DEFAULT_CIPHER_STRING));

	/* lock me into memory; don't leak info to swap */
	if (mlockall(MCL_CURRENT|MCL_FUTURE) < 0)
		WARNING("failed locking process in RAM (not root?): %s",
				strerror(errno));

	gcry_global_init();

	signal(SIGPIPE, SIG_IGN);
	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	signal(SIGHUP, on_signal);
	signal(SIGQUIT, on_signal);
}

/* load all command descriptors in the hash table of priv */
void ctl_add_commands(ctl_priv_t *priv, ctl_command_t *commands, int n) {
	int i;

	hashtbl_init_default(&priv->c, -1, 4, 0, 1, NULL);
	for (i = 0; i < n; i++) {
		if (hashtbl_add_element(&priv->c, &commands[i]) == NULL)
			FATAL("duplicate command");
	}
}

/* connect to the control socket of the daemon; it serves one scubed3ctl
 * (or keystorectl) at a time, the first command waits with a timeout (see
 * do_server_command) */
int ctl_connect(ctl_priv_t *priv, int fatal) {
	struct sockaddr_un remote;
	struct timeval tv = { 2, 0 };
	socklen_t len;

	if ((priv->s = socket(AF_UNIX, SOCK_STREAM, 0)) == -1)
		FATAL("socket: %s", strerror(errno));

	remote.sun_family = AF_UNIX;
	if (strlen(control_socket) >= sizeof(remote.sun_path))
		FATAL("the path of the control socket is too long (at most "
				"%zu characters)", sizeof(remote.sun_path) - 1);
	strcpy(remote.sun_path, control_socket);
	len = strlen(remote.sun_path) + sizeof(remote.sun_family);
	if (connect(priv->s, (struct sockaddr*)&remote, len) == -1) {
		if (fatal) FATAL("connecting to socket %s: %s",
				remote.sun_path, strerror(errno));
		close(priv->s);
		priv->s = -1;
		return -1;
	}

	if (setsockopt(priv->s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)))
		FATAL("setsockopt: %s", strerror(errno));
	wait_note = 1;
	if (do_server_command(priv->s, 0, "static-info")) FATAL("unable to "
			"request mountpoint");
	if (wait_note) {
		tv.tv_sec = 0;
		wait_note = 0;
		if (setsockopt(priv->s, SOL_SOCKET, SO_RCVTIMEO, &tv,
					sizeof(tv)))
			FATAL("setsockopt: %s", strerror(errno));
	}
	if (result.argc != 3 || result.status == -1)
		FATAL("unexpected reply from server");

	priv->mountpoint = estrdup(result.argv[0]);
	if (parse_int(&priv->no_macroblocks, result.argv[1]))
		FATAL("unable to read the number of macroblocks "
				"from the server");
	VERBOSE("connected to scubed3-%s", result.argv[2]);
	VERBOSE("cipher: %s, KDF: %s(%s/%ld)", DEFAULT_CIPHER_STRING,
			DEFAULT_KDF_FUNCTION, DEFAULT_KDF_HASH, kdf_iterations);

	return 0;
}

void ctl_disconnect(ctl_priv_t *priv) {
	if (priv->s == -1) return;
	do_server_command(priv->s, 1, "exit");
	close(priv->s);
	priv->s = -1;
	free(priv->mountpoint);
	priv->mountpoint = NULL;
}
