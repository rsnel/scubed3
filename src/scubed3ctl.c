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
#undef NDEBUG /* we need sanity checking */
#include <readline/readline.h>
#include <readline/history.h>
#include <termios.h>

#include <assert.h>
#include "config.h"
#include "verbose.h"
#include "control.h"
#include "gcry.h"
#include "util.h"

#define BUF_SIZE 1024
#define CONV_SIZE 512

#define DEFAULT_KDF_HASH		"SHA256"
#define	DEFAULT_KDF_SALT		"scubed3_prod"
#define DEFAULT_KDF_ITERATIONS		16777216
#define DEFAULT_KDF_FUNCTION		"PBKDF2"
#define DEFAULT_CIPHER_STRING		"CBC_ESSIV(AES256)"
#define KEY_LENGTH	32

#define MAX_RESULT_LINES 128

// PBKDF2 requires salt
const char *command_option = NULL;
const char *kdf_salt = DEFAULT_KDF_SALT;
const char *control_socket = CONTROL_SOCKET;
unsigned long kdf_iterations = DEFAULT_KDF_ITERATIONS;
int assume_yes = 0;

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
						ret = -1;
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
	if (argc != cmnd->argc ) {
		printf("usage: %s%s\n", cmnd->head.key, cmnd->usage);
		free(args);
		return 0;
	}

	ret = (*cmnd->command)(priv, argv);
	free(args);
	return ret;
}

int my_getpass(char **lineptr, size_t *n, FILE *stream) {
	struct termios old, new;

	/* Turn echoing off and fail if we can't. */
	if (tcgetattr(fileno(stream), &old) != 0) return -1;

	new = old;
	new.c_lflag &= ~ECHO;
	if (tcsetattr(fileno(stream), TCSAFLUSH, &new) != 0)
		return -1;

	/* Read the password. */
	*n = getline (lineptr, n, stream);

	/* Restore terminal. */
	(void) tcsetattr (fileno (stream), TCSAFLUSH, &old);

	printf("\n");

	return 0;
}

static int ctl_open_create_common(ctl_priv_t *priv, char *argv[], int create) {
	uint8_t hash[32];
	char *pw = NULL, *pw2 = NULL;
	size_t pw_len, pw2_len;
	int i, ret;
	int algo, subalgo;

	if (do_server_command(priv->s, 1, "check-available %s", argv[0]))
		return -1;
	if (result.status == -1) return 0;

	if (!strcmp(DEFAULT_KDF_FUNCTION, "PBKDF2"))
		algo = GCRY_KDF_PBKDF2;
	else {
		ERROR("unknown key derivation function %s",
				DEFAULT_KDF_FUNCTION);
		return -1;
	}

	subalgo = gcry_md_map_name(DEFAULT_KDF_HASH);
	assert(subalgo);

	printf("Enter passphrase: ");
	if (my_getpass(&pw, &pw_len, stdin) == -1) {
		ERROR("unable to get password");
		return -1;
	}
	if (create) {
		printf("Verify passphrase: ");
		if (my_getpass(&pw2, &pw2_len, stdin) == -1) {
			wipememory(pw, pw_len);
			free(pw);
			ERROR("unable to get password for verification");
			return -1;
		}
		if (pw_len != pw2_len || strcmp(pw, pw2)) {
			wipememory(pw, pw_len);
			wipememory(pw2, pw2_len);
			free(pw);
			free(pw2);
			printf("passphrases do not match\n");
			exit_status = EXIT_FAILURE;
			return 0;
		}

		wipememory(pw2, pw2_len);
		free(pw2);
	}

	assert(pw_len > 0);
	if (pw_len == 1) WARNING("empty passphrase used, "
			"this is not very secure");

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

	gcry_call(kdf_derive, pw, pw_len - 1,
			algo, subalgo, kdf_salt, strlen(kdf_salt),
			kdf_iterations, 32, hash);

	wipememory(pw, pw_len);
	free(pw);

	char hash_text[64], *ptr = hash_text;

	for (i = 0; i < gcry_md_get_algo_dlen(subalgo); i++)
		ptr += snprintf(ptr, 3, "%02x", hash[i]);

	// WARNING: sizeof(hash) must be cast to int... see below
	wipememory(hash, (int)sizeof(hash));
	//gcry_md_close(hd);

	// WARNING: sizeof(hash_text) must be cast to int... see below
	ret = do_server_command(priv->s, 1, "%s-internal %s %s %.*s",
			create?"create":"open", argv[0],
			DEFAULT_CIPHER_STRING, (int)sizeof(hash_text),
			hash_text);

	// HERE BE DRAGONS! if sizeof(hash_text) is NOT cast to int, then
	// the compiler 'optimizes' it to 0, that will lead to wipememory
	// attempting to wipe all memory, which will lead to a segfault
	wipememory(hash_text, (int)sizeof(hash_text));

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
	printf("mount NAME MOUNTPOINT\n");
	printf("umount NAME\n");
	return 0;
}

static int ctl_create(ctl_priv_t *priv, char *argv[]) {
	return ctl_open_create_common(priv, argv, 1);
}

int yesno(const char *prompt) {
	//char answer[4];
	char *answer;
	if (assume_yes >= 3) {
		printf("%s Yes (assumed)\n", prompt);
		return 1;
	}
label:
	answer = readline(prompt);
	while (*answer == ' ') answer++;

	//fgets(answer, sizeof(answer), stdin);
	if (answer[0] == '\0' || !strcmp("No", answer)) {
		free(answer);
		return 0; /* No */
	}
	if (strcmp("Yes", answer)) {
		prompt = "Please answer Yes or No. [No] ";
		free(answer);
		goto label;
	}
	free(answer);
	return 1; /* Yes */
}

void warning(void) {
	printf("---WARNING---WARNING---WARNING---WARNING"
			"---WARNING---WARNING---WARNING---\n");
}


static int parse_int(int *r, const char *in) {
	char *end;
	int ret = strtol(in, &end, 10);

	if (errno && (ret == LONG_MIN || ret == LONG_MAX)) {
		printf("integer out of range\n");
		exit_status = EXIT_FAILURE;
		return -1;
	}
	if (*end != '\0') {
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

static int ctl_mke2fs(ctl_priv_t *priv, char *argv[]) {
	if (do_server_command(priv->s, 0, "info %s", argv[0])) return -1;
	if (result.status) {
		printf("%s\n", result.argv[0]);
		return 0;
	}
	if (do_server_command(priv->s, 0, "get-aux %s mountpoint",
					argv[0])) return -1;
	if (!result.status) {
		printf("partition \"%s\" is mounted on %s\n",
				argv[0], result.argv[0]);
		return 0;
	}
	var_system("mke2fs -F %s/%s", priv->mountpoint, argv[0]);
	return 0;
}

static int ctl_mount(ctl_priv_t *priv, char *argv[]) {
	if (*argv[1] != '/') {
		printf("mountpoint must start with a slash");
		return 0;
	}
	if (do_server_command(priv->s, 0, "info %s", argv[0])) return -1;
	if (result.status) {
		exit_status = EXIT_SUCCESS; // previous error is handled
					    // here so do not exit with
					    // EXIT_FAILURE yet

		// we try to open the partition
		do_local_command(priv, hashtbl_find_element_bykey(&priv->c,
					"open"), "%s", argv[0]);

		if (result.status) return 0;
		if (do_server_command(priv->s, 0, "set-close-on-release %s 1",
				argv[0])) return -1;
		if (result.status) return 0;
	}

	if (!var_system("mount -o loop %s/%s %s", priv->mountpoint,
				argv[0], argv[1])) { // ok
		if (do_server_command(priv->s, 0, "set-aux %s mountpoint %s",
					argv[0], argv[1])) return -1;
	}

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

static int ctl_umount(ctl_priv_t *priv, char *argv[]) {
	if (*argv[0] != '/') { // figure out mountpoint
		if (do_server_command(priv->s, 0, "get-aux %s mountpoint",
					argv[0])) return -1;
		if (result.status) {
			printf("%s\n", result.argv[0]);
			return 0;
		}
		var_system("umount %s", result.argv[0]);

	} else var_system("umount %s", argv[0]);
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

void show_help() {
	printf("%s is a program to connect with the scubed3 process to\n", exec_name);
	printf("manage scubed3 partitions (also known as hidden volumes)\n");
	printf("\n");
	printf("Usage:\n\n$ %s [-s KDF_SALT] [-i KDF_ITERATIONS] [-a SOCKET_ADDRESS] \\\n", exec_name); //argv[0]);
	printf("                [-YYY] [-c COMMAND] [-v] [-q] [-d]\n");
	printf("\nOptions (defaults shown in parentheses):\n\n");
	printf("-s KDF_SALT       salt used for KDF (%s)\n", DEFAULT_KDF_SALT);
	printf("-i KDF_ITERATIONS iterations done by KDF (%u)\n", DEFAULT_KDF_ITERATIONS);
	printf("-a SOCKET_ADDRESS addres of scubed3 socket (%s)\n", CONTROL_SOCKET);
	printf("-Y                assume Yes to questions, this option is DANGEROUS\n");
	printf("                  and must be specified 3 times to take effect\n");
	printf("-c COMMAND        non interactive mode, run COMMAND and exit,\n");
	printf("                  if the command has spaces, it must be quoted\n");
	printf("-v                show verbose output\n");
	printf("-d                show debug output\n");
	printf("-q                do not show warnings, if \"assume Yes\" is active\n");
	printf("                  the warnings when shrinking/enlarging scubed3\n");
	printf("                  partitions is also not shown\n\n");
	printf("Using options -s or -i is not recommended, because you can lose access\n");
	printf("to your scubed3 partitions if you lose the values you used when you\n");
	printf("created the devices. In addition using a low\n");
	printf("number of KDF interations is also not recommended.\n");
	printf("\nMore information:\n\nType \"help\" at the s3> prompt. You only get the prompt\nif the connection to the scubed3 process succeeds\n");
}

#define NO_COMMANDS (sizeof(ctl_commands)/sizeof(ctl_commands[0]))

int main(int argc, char **argv) {
	char *endptr;
	ctl_priv_t priv;
	socklen_t len;
	int ret, opt, i, connections = 0;
	char *line = NULL;
	struct sockaddr_un remote;

	/* since we have options to enable verbose and debug, it
	 * makes no sense to enable them by default */
	verbose = 0;
	debug = 0;
	verbose_init(argv[0]);

	opterr = 0;
	while ((opt = getopt(argc, argv, "+s:i:a:hYc:vdq")) != -1) {
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
				if (command_option) FATAL("only one -c option may be specified");
				if (strlen(optarg) == 0) FATAL("command may not be empty");
				command_option = optarg;
				break;
			case 'a':
				if (strlen(optarg) == 0) FATAL("socket address may not be empty");
				control_socket = optarg;
				break;
			case 's':
				if (strlen(optarg) == 0) FATAL("salt must not be empty");
				kdf_salt = optarg;
				break;
			case 'i':
				kdf_iterations = strtoul(optarg, &endptr, 10);
				if (*endptr != '\0') FATAL("error in converting %s to unsigned long", optarg);
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
	if (strcmp(DEFAULT_KDF_SALT, kdf_salt))
		WARNING("using a custom salt is not recommended");
	if (kdf_iterations != DEFAULT_KDF_ITERATIONS)
		WARNING("a custom iteration count is not recommended");
	if (kdf_iterations < 1000000) {
		WARNING("low number of KDF iterations, not recommended");
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

	if ((priv.s = socket(AF_UNIX, SOCK_STREAM, 0)) == -1)
		FATAL("socket: %s", strerror(errno));

	remote.sun_family = AF_UNIX;
	strcpy(remote.sun_path, control_socket);
	len = strlen(remote.sun_path) + sizeof(remote.sun_family);
	if (connect(priv.s, (struct sockaddr*)&remote, len) == -1)
		FATAL("connecting to socket %s: %s",
				remote.sun_path, strerror(errno));


	if (do_server_command(priv.s, 0, "static-info")) FATAL("unable to "
			"request mountpoint");
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

	do {
		if (line) free(line);
		if (command_option) {
			line = estrdup(command_option);
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

		if (command_option) {
			do_server_command(priv.s, 1, "exit");
			break;
		}

	} while (!ret);

	free(line);

	free(priv.mountpoint);

	close(priv.s);

	exit(command_option?exit_status:0);
}
