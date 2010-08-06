/* scubed3ctl.c - scubed3 control program
 *
 * Copyright (C) 2009  Rik Snel <rik@snel.it>
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
#undef NDEBUG /* we need sanity checking */
#include <assert.h>
#include <readline/readline.h>
#include <readline/history.h>
#include <termios.h>

#include "config.h"
#include "verbose.h"
#include "control.h"
#include "gcry.h"
//#include "cipher.h"

#define BUF_SIZE 1024
#define CONV_SIZE 512

#define PASSPHRASE_HASH GCRY_MD_SHA256
#define DEFAULT_CIPHER_STRING "CBC_LARGE(AES256)"
#define KEY_LENGTH	32

#define MAX_RESULT_LINES 128

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

	if ((len = vasprintf(&string, format, ap)) == -1)
		FATAL("vasprintf: %s", strerror(errno));

	ret = system(string);

	wipememory(string, len);

	free(string);
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
	if ((len = vasprintf(&string, format, ap)) == -1) 
		FATAL("vasprintf: %s", strerror(errno));

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

#if 0
int write_line(int s, const char *format, ...) {
	int ret;
	va_list ap;

	va_start(ap, format);
	ret = vwrite_line(s, format, ap);
	va_end(ap);

	return ret;
}
#endif

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
					if (!status_known) WARNING("message terminates without known status");
					break;
				}
				if (status_known) {
					if (echo) printf("%s\n",
							result.buf + start);
					if (result.argc == MAX_RESULT_LINES) FATAL("too many lines in response");
					result.argv[result.argc++] = result.buf + start;
				} else if (!strcmp(result.buf + start, "ERR") ||
						!strcmp(result.buf + start, "OK")) {
					status_known = 1;
					if (!strcmp(result.buf + start, "ERR")) {
						ret = -1;
						result.status = -1;
					}
				} else FATAL("malformed response, "
						"expected OK or ERR");

				start = i + 1;
			}
		}

		buf_len += n;
	} while (!done);

	return ret;
}

int my_getpass(char **lineptr, size_t *n, FILE *stream) {
	struct termios old, new;

	/* Turn echoing off and fail if we can't. */
	if (tcgetattr (fileno(stream), &old) != 0) return -1;

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

#define MAX_ARGC 10

typedef struct ctl_priv_s {
	int s;
	hashtbl_t c;
} ctl_priv_t;

static int ctl_open(ctl_priv_t *priv, char *argv[]) {
	return 0;
}

static int ctl_create(ctl_priv_t *priv, char *argv[]) {
	return 0;
}

static int ctl_close(ctl_priv_t *priv, char *argv[]) {
	return 0;
}

static int ctl_resize(ctl_priv_t *priv, char *argv[]) {
	return 0;
}

static int ctl_mount(ctl_priv_t *priv, char *argv[]) {
	return 0;
}

static int ctl_umount(ctl_priv_t *priv, char *argv[]) {
	return 0;
}

static int ctl_exit(ctl_priv_t *priv, char *argv[]) {
	return 0;
}

typedef struct ctl_command_s {
	hashtbl_elt_t head;
	int (*command)(ctl_priv_t*, char**);
	int argc;
	char *usage;
} ctl_command_t;

static ctl_command_t ctl_commands[] = {
	{
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
		.head.key = "close",
		.command = ctl_close,
		.argc = 1,
		.usage = " NAME"
	}, {
		.head.key = "resize",
		.command = ctl_resize,
		.argc = 2,
		.usage = " NAME"
	}, {
		.head.key = "mount",
		.command = ctl_mount,
		.argc = 2,
		.usage = " NAME MOUNTPOINT"
	}, {
		.head.key = "umount",
		.command = ctl_umount,
		.argc = 1,
		.usage = " NAME"
	}, {
		.head.key = "exit",
		.command = ctl_exit,
		.argc = 0,
		.usage = ""
	}
};

int control_call(ctl_priv_t *priv, char *command) {
	//int argc = 0;
	//char *argv[MAX_ARGC+1];
	//ctl_command_t *cmnd;

	VERBOSE("got command ->%s<-", command);
	return 0;
}

#define NO_COMMANDS (sizeof(ctl_commands)/sizeof(ctl_commands[0]))

int main(int argc, char **argv) {
	ctl_priv_t priv;
	socklen_t len;
	int i;
	char *line = NULL;
	char *mountpoint;
	struct sockaddr_un remote;
	assert(PASSPHRASE_HASH == GCRY_MD_SHA256);
	assert(!strcmp("CBC_LARGE(AES256)", DEFAULT_CIPHER_STRING));

	verbose_init(argv[0]);

	/* lock me into memory; don't leak info to swap */
	if (mlockall(MCL_CURRENT|MCL_FUTURE)<0)
		WARNING("failed locking process in RAM (not root?): %s",
				strerror(errno));

	gcry_global_init();

	/* load all command descriptors in hash table */
	hashtbl_init_default(&priv.c, -1, 4, 0, 1, NULL);
	for (i = 0; i < NO_COMMANDS; i++) {
		if (hashtbl_add_element(&priv.c, &ctl_commands[i]) == NULL)
			FATAL("duplicate command");
	}

	if ((priv.s = socket(AF_UNIX, SOCK_STREAM, 0)) == -1)
		FATAL("socket: %s", strerror(errno));

	remote.sun_family = AF_UNIX;
	strcpy(remote.sun_path, CONTROL_SOCKET);
	len = strlen(remote.sun_path) + sizeof(remote.sun_family);
	if (connect(priv.s, (struct sockaddr*)&remote, len) == -1)
		FATAL("connect: %s", strerror(errno));

	do_server_command(priv.s, 0, "version");
	printf("scubed3ctl-" VERSION ", connected to scubed3-%s\n", result.argv[0]);

	do_server_command(priv.s, 0, "mountpoint");
	if (result.argc != 1 || result.status == -1)
		FATAL("unexpected reply from server");
	
	mountpoint = strdup(result.argv[0]);
	if (!mountpoint) FATAL("out of memory");

	do {
		if (line) free(line);
		line = readline("s3> ");

		if (!line) {
			printf("\nEOF\n");
			exit(1);
		}

		if (!strcmp(line, "exit") || !strcmp(line, "quit") ||
				!strcmp(line, "q") || !strcmp(line, "x") ||
				!strcmp(line, "bye") || !strcmp(line, "kthxbye") ||
				!strcmp(line, "thanks")) {
			do_server_command(priv.s, 1, "exit");
			break;
		} else if (!strcmp(line, "help")) {
			printf("helper functions in scubed3ctl:\n\n");
			printf("exit (and common synonyms)\n");
			printf("create NAME (asks twice for passphrase, expects 0 allocated blocks)\n");
			printf("open NAME (asks once for passphrase, expects >0 allocated blocks)\n");
			printf("resize NAME BLOCKS\n");
			printf("\n");
			printf("internal commands of scubed3:\n\n");
			do_server_command(priv.s, 1, "help-internal");
		} else if (!strncmp(line, "create ", 5) || !strncmp(line, "open ", 5) || !strcmp(line, "create") || !strcmp(line,"open")) {
			/* tokenize, and build custom command */
			int argc = 0;
			char *argv[MAX_ARGC+1];
			char conv[CONV_SIZE], *convp = conv;
			unsigned char key[KEY_LENGTH];
			char *pw = NULL, *pw2 = NULL;
			size_t pw_len, pw2_len;

			argv[argc] = line; 
			
			do {
				while (*argv[argc] == ' ') argv[argc]++;

				argc++;

				if (argc > MAX_ARGC) {
					printf("too many arguments\n");
					continue;
				}

                		argv[argc] = argv[argc-1];

                		while (*argv[argc] != '\0' && *argv[argc] != ' ') argv[argc]++;

                		if (*argv[argc] == ' ') *argv[argc]++ = '\0';

			} while (*argv[argc] != '\0');

			if (argc != 2) {
				printf("usage: %s NAME\n", argv[0]);
				continue;
			}

			convp += snprintf(convp, CONV_SIZE, "%s-internal %s " DEFAULT_CIPHER_STRING " ", argv[0], argv[1]);
			if (convp > conv + CONV_SIZE - 2*KEY_LENGTH - 1) {
				printf("large buffer not large enough\n");
				continue;
			}

			printf("Enter passphrase: ");
			if (my_getpass(&pw, &pw_len, stdin) == -1)
				FATAL("unable to get password");
			if (!strcmp(argv[0], "create")) {
				printf("Verify passphrase: ");
				if (my_getpass(&pw2, &pw2_len, stdin) == -1) {
					wipememory(pw, pw_len);
					free(pw);
					FATAL("unable to get password for verification");
				}
				if (pw_len != pw2_len || strcmp(pw, pw2)) {
					wipememory(pw, pw_len);
					wipememory(pw2, pw2_len);
					free(pw);
					free(pw2);
					printf("passphrases do not match\n");
					continue;
				}
				wipememory(pw2, pw2_len);
				free(pw2);
			}
			
			gcry_md_hash_buffer(PASSPHRASE_HASH, key, pw, pw_len - 1);
			for (i = 0; i < 32; i++) convp += sprintf(convp, "%02x", key[i]);
			wipememory(pw, pw_len);
			free(pw);
			
			do_server_command(priv.s, 1, "%s", conv);
			wipememory(conv, sizeof(conv));

		} else if (!strncmp(line, "umount ", 7) || !strcmp(line, "umount")) {
			/* tokenize, and build custom command */
			int argc = 0;
			char *argv[MAX_ARGC+1];

			argv[argc] = line; 
			
			do {
				while (*argv[argc] == ' ') argv[argc]++;

				argc++;

				if (argc > MAX_ARGC) {
					printf("too many arguments\n");
					continue;
				}

                		argv[argc] = argv[argc-1];

                		while (*argv[argc] != '\0' && *argv[argc] != ' ') argv[argc]++;

                		if (*argv[argc] == ' ') *argv[argc]++ = '\0';

			} while (*argv[argc] != '\0');

			if (argc != 2) {
				printf("usage: %s MOUNTPOINT\n", argv[0]);
				continue;
			}

			var_system("umount %s", argv[1]);

		} else if (!strncmp(line, "mount ", 6) || !strcmp(line, "mount")) {
			/* tokenize, and build custom command */
			int argc = 0;
			char *argv[MAX_ARGC+1];

			argv[argc] = line; 
			
			do {
				while (*argv[argc] == ' ') argv[argc]++;

				argc++;

				if (argc > MAX_ARGC) {
					printf("too many arguments\n");
					continue;
				}

                		argv[argc] = argv[argc-1];

                		while (*argv[argc] != '\0' && *argv[argc] != ' ') argv[argc]++;

                		if (*argv[argc] == ' ') *argv[argc]++ = '\0';

			} while (*argv[argc] != '\0');

			if (argc != 3) {
				printf("usage: %s NAME MOUNTPOINT\n", argv[0]);
				continue;
			}

			//VERBOSE("we should mount -o loop %s/%s %s", mountpoint, argv[1], argv[2]);
			do_server_command(priv.s, 0, "stats %s", argv[1]);
			if (result.status) {
				printf("partition %s is not open\n", argv[1]);
			} else {
				var_system("mount -o loop %s/%s %s", mountpoint, argv[1], argv[2]);
			}


		} else if (!strncmp(line, "resize ", 7) || !strcmp(line, "resize")) {
			printf("unimplemented, use low-level command resize-force\n");
		} else {
			do_server_command(priv.s, 1, "%s", line);
			wipememory(line, strlen(line));
		}

	} while (1);

	free(line);

	close(priv.s);

	exit(0);
}
