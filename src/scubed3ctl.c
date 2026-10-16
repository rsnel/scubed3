/* scubed3ctl.c - scubed3 control program
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
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/user.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <signal.h>
#include <limits.h>
#include <ctype.h>
#include <strings.h>
#undef NDEBUG /* we need sanity checking */
#include <termios.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <sys/sysmacros.h>
#include <linux/loop.h>

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

#define BUF_SIZE 1024
#define CONV_SIZE 512

#define DEFAULT_KDF_HASH		"SHA256"
#define	DEFAULT_KDF_SALT		"scubed3_prod"
#define DEFAULT_KDF_ITERATIONS		16777216
#define DEFAULT_KDF_FUNCTION		"PBKDF2"
#define DEFAULT_CIPHER_STRING		"CBC_ESSIV(AES256)"
#define DEFAULT_CONFIG_FILE		"/etc/scubed3.conf"
#define DEFAULT_KEYSTORE_FILE		"/etc/scubed3.keys"
#define KEY_LENGTH	32
/* the key of a partition in the key store is a key of a partition */
_Static_assert(KS_KEY == KEY_LENGTH, "the keys of the key store have KEY_LENGTH bytes");

#define MAX_RESULT_LINES 128

// PBKDF2 requires salt
/* -c COMMAND, more than one: they are done in this order (the key store
 * stays unlocked between them) */
#define MAX_COMMANDS 16
const char *command_options[MAX_COMMANDS];
int no_commands = 0;
const char *kdf_salt = DEFAULT_KDF_SALT;
const char *control_socket = CONTROL_SOCKET;
unsigned long kdf_iterations = DEFAULT_KDF_ITERATIONS;

/* the paranoia level that is required before a partition is written to by
 * mount, mke2fs or luksformat (paranoia=LEVEL in the configuration file) */
#define DEFAULT_PARANOIA		2
int recommended_paranoia = DEFAULT_PARANOIA;
const char *config_file = NULL; /* NULL: DEFAULT_CONFIG_FILE, if it exists */

/* the key store (see keystore.h), read when it is used */
keystore_t ks = { .path = DEFAULT_KEYSTORE_FILE };
int assume_yes = 0;
/* -k: the passphrases of the key store are given as their keys (64
 * hexadecimal digits, from keys-derive): a script (the debian-installer)
 * computes the KDF of a passphrase once, not every time it runs scubed3ctl */
static int key_input = 0;

/* in non-interactive mode, the exit status of
 * scubed3ctl must match the exit status of the
 * command that was requested using -c */
int exit_status = EXIT_SUCCESS;

typedef struct result_s {
	char buf[BUF_SIZE];
	char *argv[MAX_RESULT_LINES];
	int argc;
	int status;
} result_t;

result_t result;

int vvar_system(const char *format, va_list ap) {
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

int vwrite_line(int s, const char *format, va_list ap) {
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

			fprintf(stderr, "scubed3ctl: waiting: the scubed3 "
					"daemon serves another scubed3ctl (an "
					"s3> prompt?),\nthis one goes on when "
					"that one quits\n");
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

#define MAX_ARGC 10

typedef struct ctl_priv_s {
	int s;
	hashtbl_t c;
	char *mountpoint;
	int no_macroblocks;
} ctl_priv_t;

typedef struct ctl_command_s {
	hashtbl_elt_t head;
	int (*command)(ctl_priv_t*, char**);
	int argc;
	int optargc; /* optional arguments after argc, argv ends with NULL */
	char *usage;
} ctl_command_t;

int do_local_command(ctl_priv_t *priv,
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

/* read a passphrase (a line) from stream; on a terminal without echo, from a
 * pipe as it is (the debian-installer gives the passphrases on a pipe) */
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
int my_getpass(char **lineptr, size_t *n, FILE *stream) {
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
static char *ask_passphrase(const char *prompt, int verify, size_t *len) {
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

static void free_passphrase(char *pw, size_t len) {
	if (!pw) return;
	wipememory(pw, len);
	free(pw);
}

/* the key of a passphrase: PBKDF2-SHA256 with the salt of the
 * configuration file, and suffix (the key store uses another key than a
 * partition with the same passphrase) */
static void derive_key(const char *pw, size_t len, const char *suffix,
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

/* open or create partition name with key */
static int send_key(ctl_priv_t *priv, const char *name,
		const unsigned char *key, int create) {
	char hash_text[2*KEY_LENGTH + 1], *ptr = hash_text;
	int i, ret;

	for (i = 0; i < KEY_LENGTH; i++)
		ptr += snprintf(ptr, 3, "%02x", key[i]);

	ret = do_server_command(priv->s, 1, "%s-internal %s %s %.*s",
			create?"create":"open", name,
			DEFAULT_CIPHER_STRING, 2*KEY_LENGTH,
			hash_text);

	// HERE BE DRAGONS! if sizeof(hash_text) is NOT cast to int, then
	// the compiler 'optimizes' it to 0, that will lead to wipememory
	// attempting to wipe all memory, which will lead to a segfault
	wipememory(hash_text, (int)sizeof(hash_text));

	return ret;
}

/* the entry of partition name in the unlocked views of the key store,
 * NULL if it is in none; if two views have other keys for it (two
 * partitions with the same name, for example a root partition you can
 * admit and a hidden one), the name is ambiguous: an error, NULL and
 * *ambiguous = 1, one of the views must be locked first */
static ks_entry_t *ks_lookup(const char *name, int *ambiguous) {
	ks_entry_t *e;
	int view, other;

	*ambiguous = 0;
	if (!(e = ks_find(&ks, -1, name, &view))) return NULL;
	if ((other = ks_conflict(&ks, view, name)) >= 0) {
		printf("\"%s\" is in view %d and in view %d of the key store, "
				"with other keys: two\npartitions with the same "
				"name; lock one of the views first (lock VIEW)\n",
				name, view + 1, other + 1);
		exit_status = EXIT_FAILURE;
		*ambiguous = 1;
		return NULL;
	}

	return e;
}

/* the key of a partition comes from the key store, if it is in an unlocked
 * view, otherwise from its passphrase */
static int ctl_open_create_common(ctl_priv_t *priv, char *argv[], int create) {
	unsigned char key[KEY_LENGTH];
	ks_entry_t *e;
	char *pw;
	size_t len;
	int ret, ambiguous;

	if (do_server_command(priv->s, 1, "check-available %s", argv[0]))
		return -1;
	if (result.status == -1) return 0;

	assert(!strcmp(DEFAULT_KDF_FUNCTION, "PBKDF2"));

	e = ks_lookup(argv[0], &ambiguous);
	if (ambiguous) return 0;
	if (e) {
		VERBOSE("the key of \"%s\" is in the key store", argv[0]);
		return send_key(priv, argv[0], e->key, create);
	}

	if (!(pw = ask_passphrase("Enter passphrase: ", create, &len))) {
		exit_status = EXIT_FAILURE;
		return 0;
	}
	if (!len) WARNING("empty passphrase used, "
			"this is not very secure");

	derive_key(pw, len, "", key);
	free_passphrase(pw, len);

	ret = send_key(priv, argv[0], key, create);

	// WARNING: sizeof(key) must be cast to int... see below
	wipememory(key, (int)sizeof(key));

	return ret;
}

static int ctl_open(ctl_priv_t *priv, char *argv[]) {
	return ctl_open_create_common(priv, argv, 0);
}

static int ctl_help(ctl_priv_t *priv, char *argv[]) {
	printf("Internal commands:\n\n");
	printf("create NAME\n");
	printf("open NAME\n");
	printf("close NAME\n");
	printf("resize NAME MACROBLOCKS\n");
	printf("luksformat NAME\n");
	printf("luksopen NAME\n");
	printf("luksstatus [NAME] (LUKS header, key store, loop device, /dev/mapper, mount)\n");
	printf("\nKey store (%s):\n\n", ks.path);
	printf("keys-setup        (guided: creates, unlocks, adds passphrases and partitions)\n");
	printf("keys-create\n");
	printf("unlock\n");
	printf("unlock-all\n");
	printf("unlock-until NAME (passphrases until a view has NAME, for a boot)\n");
	printf("lock VIEW         (forget an unlocked view, for example when two views have\n");
	printf("                   a partition with the same name and other keys)\n");
	printf("keys\n");
	printf("keys-passphrase-add\n");
	printf("keys-derive       (the key of a passphrase, for -k)\n");
	printf("keys-passphrase-change [VIEW]\n");
	printf("keys-passphrase-remove [VIEW]\n");
	printf("keys-add NAME [VIEW]\n");
	printf("keys-copy NAME1 NAME2 [VIEW] (NAME2 gets the keys of NAME1)\n");
	printf("keys-import NAME [VIEW]\n");
	printf("keys-remove NAME [VIEW]\n");
	printf("open-all\n");
	printf("luksopen-all [NAME] (of the view of NAME, or of all unlocked views)\n\n");
	printf("mke2fs NAME\n");
	printf("mount NAME MOUNTPOINT\n");
	printf("umount NAME|MOUNTPOINT\n");
	printf("chown OWNER.GROUP NAME|MOUNTPOINT\n");
	printf("paranoia LEVEL [KIBPS]\n");
	return 0;
}

static int ctl_create(ctl_priv_t *priv, char *argv[]) {
	return ctl_open_create_common(priv, argv, 1);
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
static int parse_int(int *r, const char *in) {
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

static int parse_info(ctl_priv_t *priv, int no, ...) {
	int done[no], *ptrs[no], i, j;
	char *names[no], *is;
	va_list ap;

	va_start(ap, no);
	for (i = 0; i < no; i++) {
		done[i] = 0;
		names[i] = va_arg(ap, char*);
		ptrs[i] = va_arg(ap, int*);
	}
	va_end(ap);

	for (j = 0; j < result.argc; j++) {
		if (!(is = strchr(result.argv[j], '='))) {
			printf("malformed response from server\n");
			exit_status = EXIT_FAILURE;
			return -1;
		}

		*(is++) = '\0';

		for (i = 0; i < no; i++) {
			if (!strcmp(result.argv[j], names[i])) {
				if (done[i]) {
					printf("double response "
							"from the server");
					exit_status = EXIT_FAILURE;
					return -1;
				}

				if (parse_int(ptrs[i], is)) return -1;

				done[i] = 1;

				continue;
			}
		}
	}

	return 0;
}

#define DEFAULT_KEEP_REVISIONS 3
#define DEFAULT_INCREMENT 4
#define DEFAULT_RESERVED_MACROBLOCKS \
	(DEFAULT_KEEP_REVISIONS + DEFAULT_INCREMENT)

static int ctl_resize(ctl_priv_t *priv, char *argv[]) {
	int no_macroblocks, new, reserved_macroblocks;

	if (do_server_command(priv->s, 0, "info %s", argv[0])) return -1;
	if (result.status) {
		printf("%s\n", result.argv[0]);
		return 0;
	}

	if (parse_info(priv, 1, "no_macroblocks", &no_macroblocks)) return 0;

	if (parse_int(&new, argv[1])) return 0;

	if (new > 0) reserved_macroblocks = (new - 1)/4 + 1; 
	else reserved_macroblocks = 0;

	if (new == no_macroblocks) {
		printf("size already is %d\n", new);
		return 0;
	}

	if (new < 0) {
		printf("you cannot resize below 0\n");
		return 0;
	}

	if (new > priv->no_macroblocks) {
		printf("can't resize to %d macroblocks because there are only "
				"%d macroblocks in total\n",
				new, priv->no_macroblocks);
		return 0;
	}

	/* only show this warning if quiet is not active or assume_yes
	 * is not active */
	if (!quiet || assume_yes < 3) {
		if (new > no_macroblocks) {
			char *prompt = "\
only safe if ALL your scubed3 partitions are open, continue? [No] ";
			warning();
			printf("allocating %d blocks for %s from the unclaimed pool, "
					"this is\n", new - no_macroblocks, argv[0]);

			if (!yesno(prompt)) return 0;
		} else {
			assert(no_macroblocks > new);
			warning();
			if (!new) {
				printf("\
removing %s: all its data is lost.\n\
\n\
Removing a partition is a sensitive operation: its blocks are wiped, in\n\
paranoia level 2 and 3 in random order, that takes long (%s cannot be\n\
closed until it is done). To protect its data, and the existence of its\n\
passphrase: if scubed3 stops before \"p\" shows %s as [removed, wiped],\n\
OPEN %s FIRST every time you start scubed3, before you switch to level 2\n\
or 3 and before you write to the other partitions, until \"p\" shows\n\
[removed, wiped]. Do not use its passphrase again before that.\n\
\n", argv[0], argv[0], argv[0], argv[0]);
				if (!typed_yes("type yes if you understand this "
							"and want to remove it: ")) {
					printf("%s is not removed\n", argv[0]);
					return 0;
				}
				return do_server_command(priv->s, 1,
						"resize-internal %s 0 0", argv[0]);
			}
			printf("removing %d blocks from the end of %s, those blocks\n",
					no_macroblocks - new, argv[0]);
			printf("will be added to the unclaimed pool, if you have a "
					"filesystem on it\n");

			if (!yesno("you must resize it before typing Yes, "
						"continue? [No] ")) return 0;
		}
	}

	return do_server_command(priv->s, 1, "resize-internal %s %d %d",
			argv[0], new, reserved_macroblocks);
}

/* writing to a partition (mount, mke2fs, luksformat) below the recommended
 * paranoia level shows which blocks belong to which partition (see
 * Paranoia levels in the README), the user must type yes; returns 1 if it
 * may go on, 0 if not, -1 if the connection failed */
static int check_paranoia(ctl_priv_t *priv, const char *what) {
	int i, current = -1;
	char prompt[128];

	if (do_server_command(priv->s, 0, "p")) return -1;
	for (i = 0; i < result.argc; i++)
		sscanf(result.argv[i], "paranoia level %d", &current);
	if (current >= recommended_paranoia) return 1;

	warning();
	printf("the paranoia level is %d, the recommended level is %d (see "
			"the configuration\nfile): what is written to a "
			"partition now shows which blocks belong to\nwhich "
			"partition. Open ALL your partitions first, then "
			"switch with\n\"paranoia %d\", then %s.\n\n", current,
			recommended_paranoia, recommended_paranoia, what);
	snprintf(prompt, sizeof(prompt), "type yes to %s anyway: ", what);
	if (typed_yes(prompt)) return 1;
	printf("cancelled\n");

	return 0;
}

/* the name of the device mapper device of a partition with LUKS on it:
 * the prefix (luks_prefix= in the configuration file, the debian-installer
 * makes it HOSTNAME-, as LVM names the volumes of HOSTNAME-vg) and the name
 * of the partition */
#define LUKS_NAME	"%s%s"
static char luks_prefix[65] = "scubed3-";

/* does the partition have a LUKS header? (it is not counted as an error if
 * it has not) */
static int is_luks(ctl_priv_t *priv, const char *name) {
	char *cmd;
	int ret;

	if (asprintf(&cmd, "cryptsetup isLuks %s/%s 2>/dev/null",
				priv->mountpoint, name) == -1)
		FATAL("out of memory");
	ret = system(cmd);
	free(cmd);

	return !ret;
}

/* the partition must exist and must not be mounted, returns 1 if so (a
 * message is printed if not), -1 if the connection failed */
static int check_unmounted(ctl_priv_t *priv, const char *name) {
	if (do_server_command(priv->s, 0, "info %s", name)) return -1;
	if (result.status) {
		printf("%s\n", result.argv[0]);
		return 0;
	}
	if (do_server_command(priv->s, 0, "get-aux %s mountpoint",
					name)) return -1;
	if (!result.status) {
		printf("partition \"%s\" is mounted on %s\n",
				name, result.argv[0]);
		return 0;
	}

	return 1;
}

/* LUKS on top of a partition (see README): its data stays encrypted while
 * the partition is open (in paranoia level 2 and 3 all partitions are
 * open) but the LUKS volume is not, the LUKS volume is opened by mke2fs
 * and mount, umount closes it */
/* run a cryptsetup command with the key file of the LUKS volume of e on its
 * standard input (--key-file=-), the key is never written to a file */
static int cryptsetup_with_key(ks_entry_t *e, const char *format, ...) {
	char *cmd;
	va_list ap;
	FILE *fp;
	int ret;

	va_start(ap, format);
	ret = vasprintf(&cmd, format, ap);
	va_end(ap);
	if (ret == -1) FATAL("out of memory");

	if (!(fp = popen(cmd, "w"))) FATAL("popen: %s", strerror(errno));
	free(cmd);
	if (fwrite(e->luks, KS_LUKS, 1, fp) != 1)
		WARNING("writing the key to cryptsetup: %s", strerror(errno));
	if ((ret = pclose(fp))) exit_status = EXIT_FAILURE;

	return ret;
}

/* the key of the LUKS volume on partition name in the key store, NULL if
 * it is not there (cryptsetup asks the passphrase then); *ambiguous: see
 * ks_lookup */
static ks_entry_t *luks_key(const char *name, int *ambiguous) {
	ks_entry_t *e = ks_lookup(name, ambiguous);

	return (e && (e->flags&KS_HAS_LUKS))?e:NULL;
}

/* open the LUKS volume on partition name as /dev/mapper/PREFIXNAME */
static int luks_open(ctl_priv_t *priv, const char *name) {
	int ambiguous;
	ks_entry_t *e = luks_key(name, &ambiguous);

	if (ambiguous) return 1;
	if (e) return cryptsetup_with_key(e, "cryptsetup open --key-file=- "
			"%s/%s " LUKS_NAME, priv->mountpoint, name,
			luks_prefix, name);

	return var_system("cryptsetup open %s/%s " LUKS_NAME,
			priv->mountpoint, name, luks_prefix, name);
}

/* the block device whose major:minor is in /sys/block/NAME/dev (or the
 * sysfs file at path), 0 if there is none */
static dev_t sysfs_dev(const char *path) {
	unsigned int ma, mi;
	FILE *f;
	int ok;

	if (!(f = fopen(path, "r"))) return 0;
	ok = fscanf(f, "%u:%u", &ma, &mi) == 2;
	fclose(f);

	return ok?makedev(ma, mi):0;
}

/* where the block device dev is mounted (the first mount point in mnt),
 * 1 if it is */
static int mounted_at(dev_t dev, char *mnt, size_t size) {
	char src[PATH_MAX], dir[PATH_MAX];
	struct stat st;
	FILE *f;
	int ret = 0;

	if (!(f = fopen("/proc/self/mounts", "r"))) return 0;
	while (fscanf(f, "%4095s %4095s %*[^\n]", src, dir) == 2) {
		if (src[0] != '/' || stat(src, &st) || !S_ISBLK(st.st_mode) ||
				st.st_rdev != dev) continue;
		snprintf(mnt, size, "%s", dir);
		ret = 1;
		break;
	}
	fclose(f);

	return ret;
}

/* the device mapper device on loop device dev (a device in /dev/mapper with
 * it as slave; or a node of the loop device itself, as fake-cryptsetup of
 * the tests makes it), its name in name, its device in *mdev */
static int mapper_on(const char *loop, dev_t dev, char *name, size_t size,
		dev_t *mdev) {
	char path[PATH_MAX];
	struct dirent *de;
	struct stat st;
	DIR *d;
	int ret = 0;

	if (!(d = opendir("/dev/mapper"))) return 0;
	while (!ret && (de = readdir(d))) {
		if (de->d_name[0] == '.' || !strcmp(de->d_name, "control"))
			continue;
		snprintf(path, sizeof(path), "/dev/mapper/%s", de->d_name);
		if (stat(path, &st) || !S_ISBLK(st.st_mode)) continue;
		snprintf(path, sizeof(path), "/sys/dev/block/%u:%u/slaves/%s",
				major(st.st_rdev), minor(st.st_rdev), loop);
		if (st.st_rdev != dev && access(path, F_OK)) continue;
		snprintf(name, size, "%s", de->d_name);
		*mdev = st.st_rdev;
		ret = 1;
	}
	closedir(d);

	return ret;
}

/* the LUKS status of one partition: its LUKS header, the key store, the
 * loop devices on its file, the device mapper device on them (LUKS open),
 * where it is mounted */
static void luks_status(ctl_priv_t *priv, const char *name) {
	char path[PATH_MAX], loop[NAME_MAX + 1], mapper[NAME_MAX + 1],
	     mnt[PATH_MAX];
	unsigned char hdr[8];
	struct loop_info64 li;
	struct dirent *de;
	struct stat st;
	ks_entry_t *e;
	dev_t ldev, mdev;
	int fd, luks = 0, version = 0, v, loops = 0;
	DIR *d;

	snprintf(path, sizeof(path), "%s/%s", priv->mountpoint, name);
	if (stat(path, &st)) {
		printf("%s: not open\n", name);
		exit_status = EXIT_FAILURE;
		return;
	}
	if ((fd = open(path, O_RDONLY|O_CLOEXEC)) != -1) {
		if (pread(fd, hdr, sizeof(hdr), 0) == sizeof(hdr) &&
				!memcmp(hdr, "LUKS\272\276", 6)) {
			luks = 1;
			version = hdr[6]<<8|hdr[7];
		}
		close(fd);
	}
	printf("%s: ", name);
	if (luks && (version == 1 || version == 2))
		printf("LUKS%d header", version);
	else if (luks) printf("LUKS header (version %d)", version);
	else printf("no LUKS header");
	if (ks.data && (e = ks_find(&ks, -1, name, &v))) {
		if (e->flags&KS_HAS_LUKS) printf(", its LUKS key is in view "
				"%d of the key store", v + 1);
		else printf(", in view %d of the key store (no LUKS key)",
				v + 1);
	}

	/* the loop devices on the file of the partition */
	if ((d = opendir("/sys/block"))) {
		while ((de = readdir(d))) {
			if (strncmp(de->d_name, "loop", 4)) continue;
			snprintf(path, sizeof(path), "/dev/%s", de->d_name);
			if ((fd = open(path, O_RDONLY|O_CLOEXEC)) == -1) continue;
			if (ioctl(fd, LOOP_GET_STATUS64, &li) ||
					li.lo_device != st.st_dev ||
					li.lo_inode != st.st_ino) {
				close(fd);
				continue;
			}
			close(fd);
			snprintf(loop, sizeof(loop), "%s", de->d_name);
			snprintf(path, sizeof(path), "/sys/block/%s/dev", loop);
			ldev = sysfs_dev(path);
			loops++;
			printf(", /dev/%s", loop);
			if (mapper_on(loop, ldev, mapper, sizeof(mapper), &mdev)) {
				printf(" open as /dev/mapper/%s", mapper);
				if (mounted_at(mdev, mnt, sizeof(mnt)))
					printf(" (mounted on %s)", mnt);
			} else if (mounted_at(ldev, mnt, sizeof(mnt)))
				printf(" mounted on %s", mnt);
			else if (luks) printf(" (not opened)");
		}
		closedir(d);
	}
	if (!loops) printf(luks?", not open (no loop device)":
			", no loop device");
	printf("\n");
}

static int ctl_luksstatus(ctl_priv_t *priv, char *argv[]) {
	struct dirent *de;
	DIR *d;
	int n = 0;

	if (argv[0]) {
		luks_status(priv, argv[0]);
		return 0;
	}
	if (!(d = opendir(priv->mountpoint))) {
		printf("unable to read %s: %s\n", priv->mountpoint,
				strerror(errno));
		exit_status = EXIT_FAILURE;
		return 0;
	}
	while ((de = readdir(d))) {
		if (de->d_name[0] == '.') continue;
		luks_status(priv, de->d_name);
		n++;
	}
	closedir(d);
	if (!n) printf("no partitions are open\n");

	return 0;
}

static int ctl_luksformat(ctl_priv_t *priv, char *argv[]) {
	int ret, ambiguous;
	ks_entry_t *e = luks_key(argv[0], &ambiguous);
	char prompt[128];

	if (ambiguous) return 0;

	if ((ret = check_unmounted(priv, argv[0])) != 1) return ret;
	if ((ret = check_paranoia(priv, "format it")) != 1) return ret;
	if (is_luks(priv, argv[0])) printf("partition \"%s\" already has a "
			"LUKS header, it is formatted again\n", argv[0]);
	if (!e) {
		var_system("cryptsetup luksFormat --type luks2 %s/%s",
				priv->mountpoint, argv[0]);
		return 0;
	}

	/* the key from the key store, cryptsetup does not ask */
	snprintf(prompt, sizeof(prompt), "the data on \"%s\" is lost, type "
			"yes to format it (with its LUKS key from the key "
			"store): ", argv[0]);
	if (!typed_yes(prompt)) {
		printf("cancelled\n");
		return 0;
	}
	if (!cryptsetup_with_key(e, "cryptsetup -q luksFormat --type luks2 "
				"--key-file=- %s/%s", priv->mountpoint, argv[0]))
		printf("\"%s\" is formatted, its LUKS key is in the key "
				"store\n", argv[0]);

	return 0;
}

/* open the LUKS volume of a partition (for a boot: no filesystem is
 * mounted, nothing is written) */
static int ctl_luksopen(ctl_priv_t *priv, char *argv[]) {
	if (do_server_command(priv->s, 0, "info %s", argv[0])) return -1;
	if (result.status) {
		printf("%s\n", result.argv[0]);
		exit_status = EXIT_FAILURE;
		return 0;
	}
	if (!is_luks(priv, argv[0])) {
		printf("partition \"%s\" has no LUKS header\n", argv[0]);
		exit_status = EXIT_FAILURE;
		return 0;
	}
	luks_open(priv, argv[0]);

	return 0;
}

static int ctl_mke2fs(ctl_priv_t *priv, char *argv[]) {
	int ret;

	if ((ret = check_unmounted(priv, argv[0])) != 1) return ret;
	if ((ret = check_paranoia(priv, "make the filesystem")) != 1)
		return ret;
	if (is_luks(priv, argv[0])) {
		if (luks_open(priv, argv[0])) return 0;
		var_system("mke2fs -F /dev/mapper/" LUKS_NAME, luks_prefix,
				argv[0]);
		var_system("cryptsetup close " LUKS_NAME, luks_prefix, argv[0]);
	} else var_system("mke2fs -F %s/%s", priv->mountpoint, argv[0]);
	return 0;
}

static int ctl_mount(ctl_priv_t *priv, char *argv[]) {
	int ret;

	if (*argv[1] != '/') {
		printf("mountpoint must start with a slash\n");
		return 0;
	}
	if (do_server_command(priv->s, 0, "info %s", argv[0])) return -1;
	if (result.status) {
		if ((ret = check_paranoia(priv, "mount it")) != 1) return ret;
		// we try to open the partition
		do_local_command(priv, hashtbl_find_element_bykey(&priv->c,
					"open"), "%s", argv[0]);

		if (result.status) return 0;
		if (do_server_command(priv->s, 0, "set-close-on-release %s 1",
				argv[0])) return -1;
		if (result.status) return 0;
	} else {
		/* the partition can be opened more than once, but it
		 * should not be mounted twice */
		if (do_server_command(priv->s, 0, "get-aux %s mountpoint",
					argv[0])) return -1;
		if (!result.status) {
			printf("partition \"%s\" is already mounted on %s\n",
					argv[0], result.argv[0]);
			return 0;
		}
		if ((ret = check_paranoia(priv, "mount it")) != 1) return ret;
	}

	if (is_luks(priv, argv[0])) {
		if (luks_open(priv, argv[0])) return 0;
		if (var_system("mount /dev/mapper/" LUKS_NAME " %s",
					luks_prefix, argv[0], argv[1])) {
			var_system("cryptsetup close " LUKS_NAME, luks_prefix,
					argv[0]);
			return 0;
		}
	} else if (var_system("mount -o loop %s/%s %s", priv->mountpoint,
				argv[0], argv[1])) return 0;

	if (do_server_command(priv->s, 0, "set-aux %s mountpoint %s",
				argv[0], argv[1])) return -1;

	return 0;
}

static int ctl_chown(ctl_priv_t *priv, char *argv[]) {
	if (*argv[1] == '/') {
		printf("only partition name is allowed\n");
		return 0;
	}
	if (do_server_command(priv->s, 0, "get-aux %s mountpoint",
				argv[1])) return -1;
	if (result.status) {
		printf("no mountpount known for \"%s\"\n", argv[1]);
		return 0;
	}
	var_system("chown %s %s", argv[0], result.argv[0]);

	return 0;
}

/* paranoia level 2 and 3 (see README) overwrite every block that does not
 * belong to an open partition, so all partitions must be open (we ask, but
 * not when switching between level 2 and 3: closing a partition switches
 * back to level 1, so they are still open); level 3 limits the speed of
 * every partition, it is shown */
static int ctl_paranoia(ctl_priv_t *priv, char *argv[]) {
	int i, level, current = -1;
	unsigned long total = 0, kibps = 0, blocks;
	char name[256];

	if (strcmp(argv[0], "0") && strcmp(argv[0], "1") &&
			strcmp(argv[0], "2") && strcmp(argv[0], "3")) {
		printf("paranoia level must be 0, 1, 2 or 3\n");
		exit_status = EXIT_FAILURE;
		return 0;
	}
	level = atoi(argv[0]);
	if (argv[1] && (sscanf(argv[1], "%lu", &kibps) != 1 || !kibps)) {
		printf("the rate must be a positive number of KiB/s\n");
		exit_status = EXIT_FAILURE;
		return 0;
	}

	if (level >= 2 && (!quiet || assume_yes < 3)) {
		if (do_server_command(priv->s, 0, "p")) return -1;
		for (i = 0; i < result.argc; i++)
			sscanf(result.argv[i], "paranoia level %d", &current);
		if (current < 2) {
			warning();
			printf("paranoia level %d writes random data to every "
					"block that does not belong\nto an open "
					"partition: partitions that are NOT OPEN NOW "
					"WILL BE DESTROYED\n(and closing a partition "
					"switches back to level 1), the open "
					"partitions are:\n", level);
			for (i = 0; i < result.argc; i++)
				if (strstr(result.argv[i], " blocks in "))
					printf("%s\n", result.argv[i]);
		}

		if (level == 3) {
			for (i = 0; i < result.argc; i++) {
				sscanf(result.argv[i], "%lu blocks total",
						&total);
				if (!argv[1]) sscanf(result.argv[i],
						"level 3 rate %lu KiB/s",
						&kibps);
			}
			printf("\nparanoia level 3 writes on average %lu KiB/s "
					"(%.1f GB per day), also if\nnothing "
					"is written; a partition gets a share of "
					"that proportional to its\nsize, so it "
					"can write at most (the garbage collector "
					"takes a part of it):\n", kibps,
					kibps*1024.*86400/1e9);
			for (i = 0; total && i < result.argc; i++)
				if (sscanf(result.argv[i], "%lu blocks in "
						"%255s", &blocks, name) == 2)
					printf("%s: %.1f KiB/s\n", name,
						(double)kibps*blocks/total);
		}
		if (current < 2 && !yesno("are ALL your scubed3 partitions "
					"open, continue? [No] ")) return 0;
	}

	if (argv[1]) return do_server_command(priv->s, 1, "paranoia %s %s",
			argv[0], argv[1]);
	return do_server_command(priv->s, 1, "paranoia %s", argv[0]);
}

/* unmount, and close the LUKS volume if it was mounted from one */
static void umount_close(const char *mountpoint) {
	var_system("src=$(findmnt -n -o SOURCE %s); umount %s && "
			"case \"$src\" in /dev/mapper/" LUKS_NAME ") "
			"cryptsetup close \"$src\";; esac", mountpoint,
			mountpoint, luks_prefix, "*");
}

static int ctl_umount(ctl_priv_t *priv, char *argv[]) {
	if (*argv[0] != '/') { // figure out mountpoint
		if (do_server_command(priv->s, 0, "get-aux %s mountpoint",
					argv[0])) return -1;
		if (result.status) {
			printf("%s\n", result.argv[0]);
			return 0;
		}
		umount_close(result.argv[0]);
	} else umount_close(argv[0]);
	return 0;
}

/* the key store (see keystore.h): it is read when it is first used */
static int ks_open(void) {
	if (!ks_read(&ks)) return 1;
	if (errno == ENOENT) printf("there is no key store %s (keys-create "
			"makes one, -K uses another file)\n", ks.path);
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
 * input is the key itself (see keys-derive) */
static int ks_ask_key(const char *prompt, int verify, unsigned char *key) {
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
					"keys-derive)\n");
			exit_status = EXIT_FAILURE;
			return -1;
		}
	} else derive_key(pw, len, "/keystore", key);
	free_passphrase(pw, len);

	return 1;
}

static int ks_count(int view) {
	int i, n = 0;

	for (i = 0; i < KS_ENTRIES; i++)
		if (ks.views[view]->e[i].flags&KS_USED) n++;

	return n;
}

/* view VIEW (1, 2, ...), or the only unlocked view if there is no
 * argument; -1 (with a message) if there is no such view */
static int ks_view_arg(const char *arg) {
	int view;

	if (!arg) {
		if (ks.no_views == 1) return 0;
		printf(ks.no_views?"%d views are unlocked, give the number of "
				"the view\n":"no view is unlocked (unlock)\n",
				ks.no_views);
		return -1;
	}
	if (parse_int(&view, arg) || view < 1 || view > ks.no_views) {
		printf("there is no view %s (keys shows the unlocked views)\n",
				arg);
		return -1;
	}

	return view - 1;
}

static int ks_valid_name(const char *name) {
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

/* every change writes the key store again completely, the views that are
 * not unlocked are lost: the user must type yes */
static int ks_commit(void) {
	int i;

	if (!quiet || assume_yes < 3) {
		warning();
		printf("the key store is written again, completely (new IVs, new "
				"random data):\nthe views of passphrases that "
				"are NOT UNLOCKED NOW ARE LOST, it\ndoes not "
				"know them; unlocked now:");
		if (!ks.no_views) printf(" no views");
		for (i = 0; i < ks.no_views; i++) printf("%s view %d (%d "
				"entries)", i?",":"", i + 1, ks_count(i));
		printf("\n\n");
	}
	if (!typed_yes("type yes if ALL passphrases of the key store are "
				"unlocked: ")) {
		printf("cancelled, the key store is not changed (but the "
				"change is in memory, until you quit)\n");
		return 0;
	}
	if (ks_write(&ks)) {
		if (errno == ESTALE) printf("the key store %s is NOT "
				"written: another scubed3ctl changed it\nafter it "
				"was read here (this write would lose that "
				"change); quit,\nunlock again and redo the "
				"change\n", ks.path);
		else printf("unable to write the key store %s: %s\n",
				ks.path, strerror(errno));
		exit_status = EXIT_FAILURE;
		return 0;
	}
	printf("the key store is written\n");

	return 1;
}

static int ctl_keys_create(ctl_priv_t *priv, char *argv[]) {
	if (ks_create(ks.path)) {
		printf("unable to create the key store %s: %s\n", ks.path,
				strerror(errno));
		exit_status = EXIT_FAILURE;
		return 0;
	}
	printf("the key store %s is created (%d slots, random data, no "
			"views): keys-passphrase-add\nadds a passphrase; keep "
			"a backup of it (and of the configuration file)\n",
			ks.path, KS_SLOTS);

	return 0;
}

/* the partitions of view that are in another unlocked view with other
 * keys (the same name, another partition): they cannot be used while both
 * views are unlocked */
static void ks_warn_conflicts(int view) {
	int i, other;

	for (i = 0; i < KS_ENTRIES; i++) {
		ks_entry_t *e = &ks.views[view]->e[i];
		if (!(e->flags&KS_USED)) continue;
		if ((other = ks_conflict(&ks, view, e->name)) < 0) continue;
		printf("WARNING: \"%s\" is in view %d and in view %d with other "
				"keys (two partitions\nwith the same name), it "
				"cannot be used while both views are unlocked:\n"
				"lock one of them (lock VIEW)\n", e->name, view + 1,
				other + 1);
	}
}

static int ks_unlock_one(const char *prompt) {
	unsigned char key[32];
	int ret, view;

	if ((ret = ks_ask_key(prompt, 0, key)) != 1) return ret;
	view = ks_unlock(&ks, key);
	wipememory(key, 32);
	if (view < 0) {
		printf("no view for this passphrase\n");
		exit_status = EXIT_FAILURE;
	} else {
		printf("view %d: %d entries\n", view + 1, ks_count(view));
		ks_warn_conflicts(view);
	}

	return 1;
}

static int ctl_unlock(ctl_priv_t *priv, char *argv[]) {
	if (!ks_open()) return 0;
	if (ks_unlock_one("Key store passphrase: ") == 0)
		printf("no passphrase\n");

	return 0;
}

/* passphrases are asked until an empty one */
static int ctl_unlock_all(ctl_priv_t *priv, char *argv[]) {
	if (!ks_open()) return 0;
	while (ks_unlock_one("Key store passphrase (empty when done): ") == 1);

	return 0;
}

/* for a boot (see contrib/initramfs-tools): passphrases are read until a
 * view with partition NAME is unlocked (the views of the other passphrases
 * stay unlocked); after every passphrase one line, "NAME" in quotes:
 *
 *	"NAME" is in view N		found, the next command is done
 *	view N: M entries, without "NAME"
 *	no view for this passphrase	(also an empty one)
 *
 * at the end of the input (or if NAME is in two views with other keys) the
 * commands after it are not done; without a terminal no prompt is shown */
static int ctl_unlock_until(ctl_priv_t *priv, char *argv[]) {
	unsigned char key[32];
	int ret, view, other;
	const char *prompt = isatty(fileno(stdin))?"Key store passphrase: ":"";

	if (!ks_open()) return 1;
	while (1) {
		if ((ret = ks_ask_key(prompt, 0, key)) == -1) {
			printf("no view with \"%s\" is unlocked\n", argv[0]);
			exit_status = EXIT_FAILURE;
			fflush(stdout);
			return 1;
		}
		view = -1;
		if (ret == 1) {
			view = ks_unlock(&ks, key);
			wipememory(key, 32);
		}
		if (view < 0) printf("no view for this passphrase\n");
		else if (!ks_find(&ks, view, argv[0], NULL))
			printf("view %d: %d entries, without \"%s\"\n",
					view + 1, ks_count(view), argv[0]);
		else if ((other = ks_conflict(&ks, view, argv[0])) >= 0) {
			printf("\"%s\" is in view %d and in view %d with other "
					"keys\n", argv[0], view + 1, other + 1);
			exit_status = EXIT_FAILURE;
			fflush(stdout);
			return 1;
		} else {
			printf("\"%s\" is in view %d\n", argv[0], view + 1);
			fflush(stdout);
			return 0;
		}
		/* the answer must arrive now (the caller waits for it) */
		fflush(stdout);
	}
}

static void ks_list(int v) {
	int i, other;

	printf("view %d:%s [slot %d of %d]\n", v + 1, ks_count(v)?"":" (empty)",
			ks.views[v]->slot + 1, KS_SLOTS);
	for (i = 0; i < KS_ENTRIES; i++) {
		ks_entry_t *e = &ks.views[v]->e[i];
		if (!(e->flags&KS_USED)) continue;
		printf("  %s%s", e->name,
				(e->flags&KS_HAS_LUKS)?" [LUKS]":"");
		if ((other = ks_conflict(&ks, v, e->name)) >= 0)
			printf(" (other keys in view %d: lock one of them to "
					"use it)", other + 1);
		printf("\n");
	}
}

/* forget an unlocked view (in memory, the key store is not changed); the
 * views after it get a lower number; a write of the key store loses it,
 * as every view that is not unlocked */
static int ctl_lock(ctl_priv_t *priv, char *argv[]) {
	int view;

	if ((view = ks_view_arg(argv[0])) < 0) return 0;
	ks_remove_view(&ks, view);
	printf("view %d is locked%s\n", view + 1, (view < ks.no_views)?
			" (keys shows the new numbers of the other views)":"");

	return 0;
}

static int ctl_keys(ctl_priv_t *priv, char *argv[]) {
	int v;

	if (!ks.no_views) printf("no view is unlocked\n");
	for (v = 0; v < ks.no_views; v++) ks_list(v);

	return 0;
}

/* the key of a passphrase of the key store, for -k: the KDF is computed
 * once (it takes seconds), a script gives the key to the next runs of
 * scubed3ctl; the key unlocks the view as the passphrase does, keep it as
 * secret */
static int ctl_keys_derive(ctl_priv_t *priv, char *argv[]) {
	unsigned char key[32];
	char *pw;
	size_t len;
	int i;

	if (!(pw = ask_passphrase("Key store passphrase: ", 0, &len)))
		return 0;
	if (!len) {
		free_passphrase(pw, len);
		printf("no passphrase\n");
		exit_status = EXIT_FAILURE;
		return 0;
	}
	derive_key(pw, len, "/keystore", key);
	free_passphrase(pw, len);
	printf("key: ");
	for (i = 0; i < 32; i++) printf("%02x", key[i]);
	printf("\n");
	fflush(stdout);
	wipememory(key, sizeof(key));

	return 0;
}

static int ctl_keys_passphrase_add(ctl_priv_t *priv, char *argv[]) {
	unsigned char key[32];
	int view;

	if (!ks_open()) return 0;
	if (ks_ask_key("New key store passphrase: ", 1, key) != 1) {
		printf("no passphrase\n");
		return 0;
	}
	if (ks_unlock(&ks, key) >= 0) {
		printf("this passphrase has a view already\n");
	} else if ((view = ks_add_view(&ks, key)) < 0) {
		printf("all %d slots are used by unlocked views\n", KS_SLOTS);
	} else {
		printf("view %d is added (empty, in slot %d of %d, chosen at "
				"random)\n", view + 1, ks.views[view]->slot + 1,
				KS_SLOTS);
		ks_commit();
	}
	wipememory(key, 32);

	return 0;
}

static int ctl_keys_passphrase_change(ctl_priv_t *priv, char *argv[]) {
	unsigned char key[32];
	int view, other;

	if ((view = ks_view_arg(argv[0])) < 0) return 0;
	if (ks_ask_key("New key store passphrase: ", 1, key) != 1) {
		printf("no passphrase\n");
		return 0;
	}
	if ((other = ks_unlock(&ks, key)) >= 0) {
		printf("this passphrase has a view already (view %d)\n",
				other + 1);
	} else {
		memcpy(ks.views[view]->key, key, 32);
		printf("view %d has the new passphrase\n", view + 1);
		ks_commit();
	}
	wipememory(key, 32);

	return 0;
}

static int ctl_keys_passphrase_remove(ctl_priv_t *priv, char *argv[]) {
	int view;

	if ((view = ks_view_arg(argv[0])) < 0) return 0;
	printf("view %d (%d entries) is removed: the keys in it are lost, "
			"unless they are in\nanother view too\n", view + 1,
			ks_count(view));
	ks_remove_view(&ks, view);
	ks_commit();

	return 0;
}

/* NAME gets new random keys (of the partition and of its LUKS volume), or
 * the keys of NAME in another unlocked view (the same partition); 1 if the
 * view is changed (in memory) */
static int ks_add(int view, const char *name) {
	ks_entry_t *e, *from;
	int other;

	if (!ks_valid_name(name)) return 0;
	if (ks_find(&ks, view, name, NULL)) {
		printf("\"%s\" is in view %d already\n", name, view + 1);
		return 0;
	}
	from = ks_find(&ks, -1, name, &other);
	if (from && ks_conflict(&ks, other, name) >= 0) {
		printf("\"%s\" has other keys in view %d and view %d: two "
				"partitions with the same\nname, it is not clear "
				"which one to copy; lock one of the views first "
				"(lock VIEW)\n", name, other + 1,
				ks_conflict(&ks, other, name) + 1);
		return 0;
	}
	if (!(e = ks_new_entry(&ks, view, name))) {
		printf("view %d is full (%d entries)\n", view + 1, KS_ENTRIES);
		return 0;
	}
	if (from) {
		memcpy(e, from, sizeof(*e));
		printf("the keys of \"%s\" are copied from view %d to view %d\n",
				name, other + 1, view + 1);
	} else {
		ks_random(e->key, KS_KEY);
		ks_random(e->luks, KS_LUKS);
		e->flags |= KS_HAS_LUKS;
		printf("\"%s\" has new random keys in view %d: create it, and "
				"luksformat it\n", name, view + 1);
	}

	return 1;
}

static int ctl_keys_add(ctl_priv_t *priv, char *argv[]) {
	int view;

	if ((view = ks_view_arg(argv[1])) < 0) return 0;
	if (ks_add(view, argv[0])) ks_commit();

	return 0;
}

/* NAME2 gets the keys of NAME1 (an unlocked view has it): the same
 * partition under another name, for example "root" for the initramfs; in
 * VIEW, or in the view of NAME1 */
static int ctl_keys_copy(ctl_priv_t *priv, char *argv[]) {
	ks_entry_t *from, *e;
	int view, ambiguous, other;

	if (!ks_valid_name(argv[1])) return 0;
	if (!(from = ks_lookup(argv[0], &ambiguous))) {
		if (!ambiguous) printf("\"%s\" is not in an unlocked view\n",
				argv[0]);
		return 0;
	}
	if (argv[2]) {
		if ((view = ks_view_arg(argv[2])) < 0) return 0;
	} else ks_find(&ks, -1, argv[0], &view);
	if (ks_find(&ks, view, argv[1], NULL)) {
		printf("\"%s\" is in view %d already (keys-remove it first)\n",
				argv[1], view + 1);
		return 0;
	}
	if (!(e = ks_new_entry(&ks, view, argv[1]))) {
		printf("view %d is full (%d entries)\n", view + 1, KS_ENTRIES);
		return 0;
	}
	e->flags = from->flags;
	memcpy(e->key, from->key, KS_KEY);
	memcpy(e->luks, from->luks, KS_LUKS);
	printf("\"%s\" has the keys of \"%s\" in view %d: the same partition "
			"(it is opened as\none of them at a time; keys-remove "
			"%s removes the old name)\n", argv[1], argv[0],
			view + 1, argv[0]);
	if ((other = ks_conflict(&ks, view, argv[1])) >= 0)
		printf("NOTE: view %d has another partition \"%s\" (other "
				"keys); while both views\nare unlocked, \"%s\" "
				"cannot be used: lock one of them first (lock "
				"VIEW)\n", other + 1, argv[1], argv[1]);
	ks_commit();

	return 0;
}

/* overwrite the key file path (fd, len bytes) with zeroes, remove it */
static void ks_wipe_file(int fd, size_t len) {
	char zero[256] = { };
	size_t n;

	if (fd == -1) return;
	if (lseek(fd, 0, SEEK_SET) == 0) {
		while (len) {
			n = len < sizeof(zero)?len:sizeof(zero);
			if (write(fd, zero, n) != (ssize_t)n) break;
			len -= n;
		}
	}
	close(fd);
}

/* an existing partition: its key from its passphrase; if it is open and
 * has LUKS on it, a new key of the LUKS volume is added to it (with a
 * passphrase of the LUKS volume, it is asked) */
static int ks_import(ctl_priv_t *priv, int view, const char *name) {
	ks_entry_t *e;
	char *pw, tmp[32], old[32];
	size_t len, old_len = 0;
	int fd, old_fd = -1, other;
	char *argv[] = { (char*)name }, *old_pw = NULL;

	if (!ks_valid_name(argv[0])) return 0;
	if (ks_find(&ks, view, argv[0], NULL)) {
		printf("\"%s\" is in view %d already\n", argv[0], view + 1);
		return 0;
	}
	if (!(e = ks_new_entry(&ks, view, argv[0]))) {
		printf("view %d is full (%d entries)\n", view + 1, KS_ENTRIES);
		return 0;
	}
	if (!(pw = ask_passphrase("Passphrase of the partition: ", 0, &len))) {
		memset(e, 0, sizeof(*e));
		return 0;
	}
	derive_key(pw, len, "", e->key);
	free_passphrase(pw, len);

	/* the LUKS volume gets a new key; the new key and the existing
	 * passphrase (asked here, so that all input comes from the same
	 * place, also from a pipe) are in memory files (memfd_create, given
	 * to cryptsetup as /dev/fd/N): they are wiped afterwards, and they
	 * vanish with the processes, also when scubed3ctl is killed */
	if (!do_server_command(priv->s, 0, "info %s", argv[0]) &&
			!result.status && is_luks(priv, argv[0])) {
		ks_random(e->luks, KS_LUKS);
		fd = -1;
		if (!(old_pw = ask_passphrase("Passphrase of its LUKS volume: ",
						0, &old_len))) {
			printf("the LUKS volume on \"%s\" gets no key in the key "
					"store\n", argv[0]);
		} else if ((fd = memfd_create("scubed3ctl-key", 0)) == -1 ||
				write(fd, e->luks, KS_LUKS) != KS_LUKS ||
				(old_fd = memfd_create("scubed3ctl-old", 0)) ==
				-1 || write(old_fd, old_pw, old_len) !=
				(ssize_t)old_len) {
			WARNING("unable to make a key file in memory: %s",
					strerror(errno));
		} else if (snprintf(tmp, sizeof(tmp), "/dev/fd/%d", fd),
				snprintf(old, sizeof(old), "/dev/fd/%d", old_fd),
				!var_system("cryptsetup luksAddKey --key-file %s "
					"%s/%s %s", old, priv->mountpoint,
					argv[0], tmp)) {
			e->flags |= KS_HAS_LUKS;
			printf("the LUKS volume on \"%s\" has a new key, it is in "
					"the key store\n", argv[0]);
		}
		free_passphrase(old_pw, old_len);
		ks_wipe_file(fd, KS_LUKS);
		ks_wipe_file(old_fd, old_len);
		if (!(e->flags&KS_HAS_LUKS)) memset(e->luks, 0, KS_LUKS);
	}
	printf("the key of \"%s\" is in view %d (it is not checked: close and "
			"open it to see\nthat it is right)\n", argv[0], view + 1);
	if ((other = ks_conflict(&ks, view, argv[0])) >= 0)
		printf("NOTE: view %d has another partition \"%s\" (other "
				"keys); while both views\nare unlocked, \"%s\" "
				"cannot be used: lock one of them first (lock "
				"VIEW)\n", other + 1, argv[0], argv[0]);

	return 1;
}

static int ctl_keys_import(ctl_priv_t *priv, char *argv[]) {
	int view;

	if ((view = ks_view_arg(argv[1])) < 0) return 0;
	if (ks_import(priv, view, argv[0])) ks_commit();

	return 0;
}

static int ctl_keys_remove(ctl_priv_t *priv, char *argv[]) {
	ks_entry_t *e;
	int view;

	if ((view = ks_view_arg(argv[1])) < 0) return 0;
	if (!(e = ks_find(&ks, view, argv[0], NULL))) {
		printf("\"%s\" is not in view %d\n", argv[0], view + 1);
		return 0;
	}
	wipememory(e, sizeof(*e));
	if (!ks_find(&ks, -1, argv[0], NULL)) printf("the keys of \"%s\" are "
			"not in another unlocked view: without them\nit cannot "
			"be opened (unless with its passphrase, if it has "
			"one)\n", argv[0]);
	ks_commit();

	return 0;
}

/* a line from the user, without spaces at both ends, NULL at the end */
static char *ask_line(const char *prompt) {
	char *l = readline(prompt), *p, *q;

	if (!l) return NULL;
	for (p = l; isspace((unsigned char)*p); p++);
	for (q = p + strlen(p); q > p && isspace((unsigned char)q[-1]); q--);
	*q = '\0';
	memmove(l, p, q - p + 1);

	return l;
}

static int ask_yn(const char *prompt) {
	char *a = ask_line(prompt);
	int ret = a && (*a == 'y' || *a == 'Y');

	free(a);

	return ret;
}

/* a guided setup of the key store: it is created if it does not exist, all
 * its passphrases are unlocked, new passphrases are added, the partitions
 * of every view are chosen; it is written once, at the end */
static int ctl_keys_setup(ctl_priv_t *priv, char *argv[]) {
	unsigned char key[32];
	char prompt[96], *name;
	int v, changed = 0, created = 0, step = 1;

	if (access(ks.path, F_OK)) {
		if (ks_create(ks.path)) {
			printf("unable to create the key store %s: %s\n",
					ks.path, strerror(errno));
			exit_status = EXIT_FAILURE;
			return 0;
		}
		printf("the key store %s is created (keep a backup of it, and "
				"of the configuration\nfile)\n", ks.path);
		created = 1;
	}
	if (!ks_open()) return 0;

	if (!created) {
		printf("\n%d. unlock ALL passphrases of the key store: the views "
				"of the passphrases\n   that are not unlocked are "
				"lost when it is written\n", step++);
		while (ks_unlock_one("Key store passphrase (empty when "
					"done): ") == 1);
	}

	printf("\n%d. new passphrases: each one gets its own view (for "
			"example one you can\n   reveal, and one for the hidden "
			"partitions)\n", step++);
	while (ask_yn("add a new passphrase? [y/N] ")) {
		if (ks_ask_key("New key store passphrase: ", 1, key) != 1)
			continue;
		if (ks_unlock(&ks, key) >= 0)
			printf("this passphrase has a view already\n");
		else if ((v = ks_add_view(&ks, key)) < 0)
			printf("all %d slots are used\n", KS_SLOTS);
		else {
			printf("view %d is added (in slot %d of %d, chosen at "
					"random)\n", v + 1, ks.views[v]->slot + 1,
					KS_SLOTS);
			changed = 1;
		}
		wipememory(key, 32);
	}

	printf("\n%d. the partitions of every view: a name that is in another "
			"view gets the same\n   keys, an existing partition is "
			"imported (with its passphrase), a new one\n   gets new "
			"keys (then: create, resize, luksformat and mke2fs "
			"it)\n", step++);
	for (v = 0; v < ks.no_views; v++) {
		printf("\n");
		ks_list(v);
		for (;;) {
			snprintf(prompt, sizeof(prompt), "partition to add to "
					"view %d (empty when done): ", v + 1);
			if (!(name = ask_line(prompt)) || !*name) {
				free(name);
				break;
			}
			snprintf(prompt, sizeof(prompt), "is \"%.30s\" an "
					"existing partition with its own "
					"passphrase? [y/N] ", name);
			if (!ks_find(&ks, -1, name, NULL) && ks_valid_name(name)
					&& ask_yn(prompt))
				changed |= ks_import(priv, v, name);
			else changed |= ks_add(v, name);
			free(name);
		}
	}

	if (!changed) {
		printf("\nnothing is changed\n");
		return 0;
	}
	printf("\n");
	if (ks_commit()) printf("\nwith the key store unlocked (unlock, or "
			"unlock-all), open, open-all,\ncreate, luksformat, "
			"mke2fs, mount and luksopen use its keys\n");

	return 0;
}

/* open every partition of the unlocked views that is not open */
static int ctl_open_all(ctl_priv_t *priv, char *argv[]) {
	int v, i, n = 0, other, conflicts = 0;

	/* a name with other keys in two views: it is not clear which one to
	 * open, nothing is opened */
	for (v = 0; v < ks.no_views; v++) for (i = 0; i < KS_ENTRIES; i++) {
		ks_entry_t *e = &ks.views[v]->e[i];
		if (!(e->flags&KS_USED)) continue;
		if ((other = ks_conflict(&ks, v, e->name)) <= v) continue;
		printf("\"%s\" is in view %d and in view %d with other keys "
				"(two partitions with\nthe same name)\n",
				e->name, v + 1, other + 1);
		conflicts++;
	}
	if (conflicts) {
		printf("nothing is opened: lock one of the conflicting views "
				"first (lock VIEW)\n");
		exit_status = EXIT_FAILURE;
		return 0;
	}

	for (v = 0; v < ks.no_views; v++) for (i = 0; i < KS_ENTRIES; i++) {
		ks_entry_t *e = &ks.views[v]->e[i];
		if (!(e->flags&KS_USED)) continue;
		if (do_server_command(priv->s, 0, "info %s", e->name))
			return -1;
		if (!result.status) continue; /* open already */
		printf("opening \"%s\"\n", e->name);
		if (send_key(priv, e->name, e->key, 0)) return -1;
		if (!result.status) n++;
	}
	printf("%d partitions are opened\n", n);

	return 0;
}

/* open the LUKS volumes (with their keys from the key store) of the open
 * partitions of the unlocked views, of the view of NAME only if it is
 * given (at boot: the view of the root partition; the partitions of the
 * other views are open, their LUKS volumes are not) */
static int ctl_luksopen_all(ctl_priv_t *priv, char *argv[]) {
	int v, from = 0, to = ks.no_views, i, n = 0;
	char dev[128];

	if (argv[0]) {
		if (!ks_find(&ks, -1, argv[0], &from)) {
			printf("\"%s\" is not in an unlocked view\n", argv[0]);
			exit_status = EXIT_FAILURE;
			return 0;
		}
		to = from + 1;
	}
	for (v = from; v < to; v++) for (i = 0; i < KS_ENTRIES; i++) {
		ks_entry_t *e = &ks.views[v]->e[i];
		if (!(e->flags&KS_USED) || !(e->flags&KS_HAS_LUKS)) continue;
		snprintf(dev, sizeof(dev), "/dev/mapper/" LUKS_NAME,
				luks_prefix, e->name);
		if (!access(dev, F_OK)) continue; /* open already */
		if (do_server_command(priv->s, 0, "info %s", e->name))
			return -1;
		if (result.status) continue; /* the partition is not open */
		if (!is_luks(priv, e->name)) continue; /* no LUKS (yet) */
		if (!luks_open(priv, e->name)) n++;
	}
	printf("%d LUKS volumes are opened\n", n);

	return 0;
}

static ctl_command_t ctl_commands[] = {
	{
		.head.key = "help",
		.command = ctl_help,
		.argc = 0,
		.usage = ""
	}, {
		.head.key = "create",
		.command = ctl_create,
		.argc = 1,
		.usage = " NAME"
	}, {
		.head.key = "open",
		.command = ctl_open,
		.argc = 1,
		.usage = " NAME"
	}, {
		.head.key = "resize",
		.command = ctl_resize,
		.argc = 2,
		.usage = " NAME MACROBLOCKS"
	}, {
		.head.key = "luksformat",
		.command = ctl_luksformat,
		.argc = 1,
		.usage = " NAME"
	}, {
		.head.key = "luksopen",
		.command = ctl_luksopen,
		.argc = 1,
		.usage = " NAME"
	}, {
		.head.key = "luksstatus",
		.command = ctl_luksstatus,
		.argc = 0,
		.optargc = 1,
		.usage = " [NAME]"
	}, {
		.head.key = "keys-create",
		.command = ctl_keys_create,
		.argc = 0,
		.usage = ""
	}, {
		.head.key = "unlock",
		.command = ctl_unlock,
		.argc = 0,
		.usage = ""
	}, {
		.head.key = "unlock-all",
		.command = ctl_unlock_all,
		.argc = 0,
		.usage = ""
	}, {
		.head.key = "unlock-until",
		.command = ctl_unlock_until,
		.argc = 1,
		.usage = " NAME"
	}, {
		.head.key = "lock",
		.command = ctl_lock,
		.argc = 1,
		.usage = " VIEW"
	}, {
		.head.key = "keys",
		.command = ctl_keys,
		.argc = 0,
		.usage = ""
	}, {
		.head.key = "keys-derive",
		.command = ctl_keys_derive,
		.argc = 0,
		.usage = ""
	}, {
		.head.key = "keys-passphrase-add",
		.command = ctl_keys_passphrase_add,
		.argc = 0,
		.usage = ""
	}, {
		.head.key = "keys-passphrase-change",
		.command = ctl_keys_passphrase_change,
		.argc = 0,
		.optargc = 1,
		.usage = " [VIEW]"
	}, {
		.head.key = "keys-passphrase-remove",
		.command = ctl_keys_passphrase_remove,
		.argc = 0,
		.optargc = 1,
		.usage = " [VIEW]"
	}, {
		.head.key = "keys-add",
		.command = ctl_keys_add,
		.argc = 1,
		.optargc = 1,
		.usage = " NAME [VIEW]"
	}, {
		.head.key = "keys-copy",
		.command = ctl_keys_copy,
		.argc = 2,
		.optargc = 1,
		.usage = " NAME1 NAME2 [VIEW]"
	}, {
		.head.key = "keys-import",
		.command = ctl_keys_import,
		.argc = 1,
		.optargc = 1,
		.usage = " NAME [VIEW]"
	}, {
		.head.key = "keys-remove",
		.command = ctl_keys_remove,
		.argc = 1,
		.optargc = 1,
		.usage = " NAME [VIEW]"
	}, {
		.head.key = "keys-setup",
		.command = ctl_keys_setup,
		.argc = 0,
		.usage = ""
	}, {
		.head.key = "open-all",
		.command = ctl_open_all,
		.argc = 0,
		.usage = ""
	}, {
		.head.key = "luksopen-all",
		.command = ctl_luksopen_all,
		.argc = 0,
		.optargc = 1,
		.usage = " [NAME]"
	}, {
		.head.key = "mke2fs",
		.command = ctl_mke2fs,
		.argc = 1,
		.usage = " NAME"
	}, {
		.head.key = "mount",
		.command = ctl_mount,
		.argc = 2,
		.usage = " NAME MOUNTPOINT"
	}, {
		.head.key = "chown",
		.command = ctl_chown,
		.argc = 2,
		.usage = " OWNER.GROUP NAME|MOUNTPOINT"
	}, {
		.head.key = "paranoia",
		.command = ctl_paranoia,
		.argc = 1,
		.optargc = 1,
		.usage = " LEVEL [KIBPS]"
	}, {
		.head.key = "umount",
		.command = ctl_umount,
		.argc = 1,
		.usage = " NAME|MOUNTPOINT"
	}
};

int ctl_call(ctl_priv_t *priv, char *command) {
	char *tmp;
	ctl_command_t *cmnd;

	/* strip leading spaces */
	while (*command == ' ' ) command++;

	/* check if the command is local */
	if ((tmp = strchr(command, ' '))) *tmp = '\0'; // temprary terminator
	cmnd = hashtbl_find_element_bykey(&priv->c, command);
	if (tmp) *tmp = ' '; // replace space
	else tmp="";

	/* execute */
	if (cmnd) return do_local_command(priv, cmnd, "%s", tmp);
	else return do_server_command(priv->s, 1, "%s", command);
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

void show_help() {
	printf("%s is a program to connect with the scubed3 process to\n", exec_name);
	printf("manage scubed3 partitions (also known as hidden volumes)\n");
	printf("\n");
	printf("Usage:\n\n$ %s [-C CONFIG_FILE] [-s KDF_SALT] [-i KDF_ITERATIONS] \\\n", exec_name); //argv[0]);
	printf("                [-a SOCKET_ADDRESS] [-K KEY_STORE] [-k] [-YYY] [-c COMMAND]... \\\n");
	printf("                [-v] [-q] [-d]\n");
	printf("\nOptions (defaults shown in parentheses):\n\n");
	printf("-C CONFIG_FILE    file with the KDF parameters (%s, if it\n", DEFAULT_CONFIG_FILE);
	printf("                  exists), lines kdf=%s-%s, salt=TEXT and\n", DEFAULT_KDF_FUNCTION, DEFAULT_KDF_HASH);
	printf("                  iterations=NUMBER, keep a backup of it! (and\n");
	printf("                  paranoia=LEVEL, the level that mount, mke2fs and\n");
	printf("                  luksformat require, default %d)\n", DEFAULT_PARANOIA);
	printf("-s KDF_SALT       salt used for KDF, overrides the config file (%s)\n", DEFAULT_KDF_SALT);
	printf("-i KDF_ITERATIONS iterations done by KDF, overrides the config file (%u)\n", DEFAULT_KDF_ITERATIONS);
	printf("-a SOCKET_ADDRESS address of scubed3 socket (%s)\n", CONTROL_SOCKET);
	printf("-Y                assume Yes to questions, this option is DANGEROUS\n");
	printf("                  and must be specified 3 times to take effect\n");
	printf("-c COMMAND        non interactive mode, run COMMAND and exit,\n");
	printf("                  if the command has spaces, it must be quoted;\n");
	printf("                  more -c options: the commands one after the other\n");
	printf("-K KEY_STORE      the key store (%s)\n", DEFAULT_KEYSTORE_FILE);
	printf("-k                the passphrases of the key store are given as their\n");
	printf("                  keys (64 hexadecimal digits, from keys-derive), for\n");
	printf("                  scripts: the KDF is computed once\n");
	printf("-v                show verbose output\n");
	printf("-d                show debug output\n");
	printf("-q                do not show warnings, if \"assume Yes\" is active\n");
	printf("                  the warnings when shrinking/enlarging scubed3\n");
	printf("                  partitions is also not shown\n");
	printf("\nMore information:\n\nType \"help\" at the s3> prompt. You only get the prompt\nif the connection to the scubed3 process succeeds\n");
}

#define NO_COMMANDS (sizeof(ctl_commands)/sizeof(ctl_commands[0]))

int main(int argc, char **argv) {
	char *endptr;
	ctl_priv_t priv;
	socklen_t len;
	int ret, opt, i, connections = 0, salt_set = 0, iterations_set = 0;
	char *line = NULL;
	struct sockaddr_un remote;

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
				show_help();
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

	assert(!strcmp("SHA256", DEFAULT_KDF_HASH));
	assert(!strcmp("PBKDF2", DEFAULT_KDF_FUNCTION));

	/* the KDF parameters: command line, configuration file, defaults */
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

	/* load all command descriptors in hash table */
	hashtbl_init_default(&priv.c, -1, 4, 0, 1, NULL);
	for (i = 0; i < NO_COMMANDS; i++) {
		if (hashtbl_add_element(&priv.c, &ctl_commands[i]) == NULL)
			FATAL("duplicate command");
	}

	signal(SIGPIPE, SIG_IGN);
	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	signal(SIGHUP, on_signal);
	signal(SIGQUIT, on_signal);

	if ((priv.s = socket(AF_UNIX, SOCK_STREAM, 0)) == -1)
		FATAL("socket: %s", strerror(errno));

	remote.sun_family = AF_UNIX;
	if (strlen(control_socket) >= sizeof(remote.sun_path))
		FATAL("the path of the control socket is too long (at most "
				"%zu characters)", sizeof(remote.sun_path) - 1);
	strcpy(remote.sun_path, control_socket);
	len = strlen(remote.sun_path) + sizeof(remote.sun_family);
	if (connect(priv.s, (struct sockaddr*)&remote, len) == -1)
		FATAL("connecting to socket %s: %s",
				remote.sun_path, strerror(errno));


	{
		struct timeval tv = { 2, 0 };

		if (setsockopt(priv.s, SOL_SOCKET, SO_RCVTIMEO, &tv,
					sizeof(tv)))
			FATAL("setsockopt: %s", strerror(errno));
		wait_note = 1;
	}
	if (do_server_command(priv.s, 0, "static-info")) FATAL("unable to "
			"request mountpoint");
	if (wait_note) {
		struct timeval tv = { 0, 0 };

		wait_note = 0;
		if (setsockopt(priv.s, SOL_SOCKET, SO_RCVTIMEO, &tv,
					sizeof(tv)))
			FATAL("setsockopt: %s", strerror(errno));
	}
	if (result.argc != 3 || result.status == -1)
		FATAL("unexpected reply from server");

	priv.mountpoint = strdup(result.argv[0]);
	if (!priv.mountpoint) FATAL("out of memory");

	if (parse_int(&priv.no_macroblocks, result.argv[1]))
		FATAL("unable to read the number of macroblocks "
				"from the server");

	if (!connections) {
		VERBOSE("scubed3ctl-" VERSION ", connected to scubed3-%s",
				result.argv[2]);
		VERBOSE("cipher: %s, KDF: %s(%s/%ld)",
				DEFAULT_CIPHER_STRING, DEFAULT_KDF_FUNCTION,
				DEFAULT_KDF_HASH, kdf_iterations);
	} else {
		printf("re-establised connection\n");
	}

	connections++;

	i = 0;
	do {
		if (line) free(line);
		if (no_commands) {
			line = estrdup(command_options[i++]);
		} else line = readline("s3> ");

		// do not store history!
		//if (line && *line) add_history(line);

		/* exit is a special case */
		if (!line || !strcmp(line, "exit") || !strcmp(line, "quit") ||
				!strcmp(line, "q") || !strcmp(line, "x") ||
				!strcmp(line, "bye") ||
				!strcmp(line, "kthxbye") ||
				!strcmp(line, "thanks")) {
			if (!line) printf("^D\n");
			do_server_command(priv.s, 1, "exit");
			break;
		}

		ret = ctl_call(&priv, line);

		if (no_commands && (i == no_commands || ret)) {
			do_server_command(priv.s, 1, "exit");
			break;
		}

	} while (!ret);

	free(line);
	ks_free(&ks);

	free(priv.mountpoint);

	close(priv.s);

	exit(no_commands?exit_status:0);
}
