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
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#undef NDEBUG /* we need sanity checking */
#include <assert.h>
#include <readline/readline.h>
#include <readline/history.h>

#include "config.h"
#include "verbose.h"
#include "control.h"

#define BUF_SIZE 1024

int do_command(int s, const char *format, ...) {
	char buf[BUF_SIZE];
	int ret = 0, buf_len = 0, start, i, done = 0;
	char *string;
	ssize_t sent = 0, len, n;
	va_list ap;

	va_start(ap, format);
	if ((len = vasprintf(&string, format, ap)) == -1)
		FATAL("vasprintf: %s", strerror(errno));

	do {
		n = send(s, string + sent, len - sent, 0);
		if (n == 0) FATAL("send: connection reset by peer");
		if (n < 0) FATAL("send: %s", strerror(errno));
		sent += n;
	} while (sent < len);

	free(string);

	do {
		n = recv(s, buf + buf_len, BUF_SIZE - buf_len, 0);
		if (n == 0) FATAL("recv: connection reset by peer");
		if (n < 0) FATAL("recv: %s", strerror(errno));

		start = 0;

		for (i = buf_len; i < buf_len + n; i++) {
			if (buf[i] == '\n') {
				buf[i] = '\0';
				if (*(buf + start) == '.' && i - start == 1) {
					done = 1;
					break;
				}
				if (strcmp(buf + start, "OK") &&
						strcmp(buf + start, "ERR")) {
					printf("%s\n", buf + start);
				} else if (!strcmp(buf + start, "ERR"))
					ret = -1;

				start = i + 1;
			}
		}
	} while (!done);

	return ret;
}


int main(int argc, char **argv) {
	int s;
	socklen_t len;
	char *line;
	struct sockaddr_un remote;

	verbose_init(argv[0]);

	if ((s = socket(AF_UNIX, SOCK_STREAM, 0)) == -1)
		FATAL("socket: %s", strerror(errno));

	remote.sun_family = AF_UNIX;
	strcpy(remote.sun_path, CONTROL_SOCKET);
	len = strlen(remote.sun_path) + sizeof(remote.sun_family);
	if (connect(s, (struct sockaddr*)&remote, len) == -1)
		FATAL("connect: %s", strerror(errno));

	//VERBOSE("connected!");

	printf("scubed3ctl-" VERSION ", connected to scubed3\n");

	do {
		line = readline("> ");

		if (!strcmp(line, "exit")) break;
		if (!strcmp(line, "quit")) break;

		do_command(s, "%s\n", line);

		free(line);
	} while (1);

	free(line);

	close(s);

	exit(0);
}
