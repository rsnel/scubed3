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

int do_command(int s, char *format, ...) {
	char buf[BUF_SIZE];
	int ret = 0, buf_len = 0, start = 0, i, done = 0;
	int status_known = 0;
	ssize_t sent = 0, len, n;

	len = strlen(format);

	format[len] = '\n';
	len++;

	do {
		n = send(s, format + sent, len - sent, 0);
		if (n == 0) FATAL("send: connection reset by peer");
		if (n < 0) FATAL("send: %s", strerror(errno));
		sent += n;
	} while (sent < len);

	len--;
	format[len] = '\0';

	do {
		if (buf_len == BUF_SIZE) FATAL("response too long");
		n = recv(s, buf + buf_len, BUF_SIZE - buf_len, 0);
		if (n == 0) FATAL("recv: connection reset by peer");
		if (n < 0) FATAL("recv: %s", strerror(errno));

		for (i = buf_len; i < buf_len + n; i++) {
			if (buf[i] == '\n') {
				buf[i] = '\0';
				if (*(buf + start) == '.' && i - start == 1) {
					done = 1;
					if (!status_known) WARNING("message terminates without known status");
					break;
				}
				if (status_known) {
					printf("%s\n", buf + start);
				} else if (!strcmp(buf + start, "ERR") ||
						!strcmp(buf + start, "OK")) {
					status_known = 1;
					if (!strcmp(buf + start, "ERR"))
						ret = -1;
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

int main(int argc, char **argv) {
	int s;
	socklen_t len;
	int i;
	char *line = NULL;
	struct sockaddr_un remote;
	assert(PASSPHRASE_HASH == GCRY_MD_SHA256);
	assert(!strcmp("CBC_LARGE(AES256)", DEFAULT_CIPHER_STRING));

	verbose_init(argv[0]);

	/* lock me into memory; don't leak info to swap */
	if (mlockall(MCL_CURRENT|MCL_FUTURE)<0)
		WARNING("failed locking process in RAM (not root?): %s",
				strerror(errno));

	gcry_global_init();

	if ((s = socket(AF_UNIX, SOCK_STREAM, 0)) == -1)
		FATAL("socket: %s", strerror(errno));

	remote.sun_family = AF_UNIX;
	strcpy(remote.sun_path, CONTROL_SOCKET);
	len = strlen(remote.sun_path) + sizeof(remote.sun_family);
	if (connect(s, (struct sockaddr*)&remote, len) == -1)
		FATAL("connect: %s", strerror(errno));

	printf("scubed3ctl-" VERSION ", connected to scubed3\n");

	do {
		if (line) free(line);
		line = readline("> ");

		if (!line) {
			printf("\nEOF\n");
			exit(1);
		}

		if (!strcmp(line, "exit") || !strcmp(line, "quit") ||
				!strcmp(line, "q") || !strcmp(line, "x") ||
				!strcmp(line, "bye") || !strcmp(line, "kthxbye") ||
				!strcmp(line, "thanks")) {
			char bla[5] = { 'e', 'x', 'i', 't', '\0' };
			do_command(s, bla);
			break;
		} else if (!strcmp(line, "help")) {
			char bla[5] = { 'h', 'e', 'l', 'p', '\0' };
			printf("helper functions in scubed3ctl:\n\n");
			printf("exit (and common synonyms)\n");
			printf("create NAME (asks twice for passphrase, expects 0 allocated blocks)\n");
			printf("open NAME (asks once for passphrase, expects >0 allocated blocks)\n");
			printf("resize NAME BLOCKS\n");
			printf("\n");
			printf("internal commands of scubed3:\n\n");
			do_command(s, bla);
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
			
			do_command(s, conv);
			wipememory(conv, sizeof(conv));

		} else if (!strncmp(line, "resize ", 7) || !strcmp(line, "resize")) {
			printf("unimplemented, use low-level command resize-force\n");
		} else {
			do_command(s, line);
			wipememory(line, strlen(line));
		}

	} while (1);

	free(line);

	close(s);

	exit(0);
}
