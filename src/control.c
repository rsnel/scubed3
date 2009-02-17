/* control.c - control socket i/o
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
#include <string.h>
#include <errno.h>
#include <assert.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <signal.h>
#include "verbose.h"
#include "util.h"
#include "pthd.h"
#include "hashtbl.h"
#include "control.h"

#define BUF_SIZE 8192
#define MAX_ARGC 10

int control_write(int s, const char *format, ...) {
	char *string;
	ssize_t sent = 0, len, n;
	va_list ap;

	va_start(ap, format);
	if ((len = vasprintf(&string, format, ap)) == -1) {
		ERROR("vasprintf: %s", strerror(errno));
		return -1;
	}
	va_end(ap);

	do {
		n = send(s, string + sent, len - sent, 0);
		if (n == 0) {
			ERROR("send: connection reset by peer");
			return -1;
		}
		if (n < 0) {
			ERROR("send: %s", strerror(errno));
			return -1;
		}
		sent += n;
	} while (sent < len);

	free(string);

	return 0;
}

int control_status(int s) {
	return control_write(s, "OK\nno info available\n.\n");
}

int control_call(int s, char *command) {
	int argc = 0;
	char *argv[MAX_ARGC+1];

	argv[argc] = command;

	do {
		while (*argv[argc] == ' ') argv[argc]++;

		argc++;

		if (argc > MAX_ARGC)
			return control_write(s, "ERR\ntoo many arguments\n.\n");

		argv[argc] = argv[argc-1];

		while (*argv[argc] != '\0' && *argv[argc] != ' ') argv[argc]++;

		if (*argv[argc] == ' ') *argv[argc]++ = '\0';

	} while (*argv[argc] != '\0');

	VERBOSE("control_call: command ->%s<- %d arguments", argv[0], argc);

	if (!strcmp(argv[0], "status") && argc == 1) return control_status(s);
	else return
		control_write(s, "ERR\nunknown command ->%s<-\n.\n", argv[0]);
}

void *control_thread(void *arg) {
	int s, s2;
	socklen_t t, len;
	char buf[BUF_SIZE];
	int buf_len = 0;
	struct sockaddr_un local, remote;

	if ((s = socket(AF_UNIX, SOCK_STREAM, 0)) == -1)
		FATAL("socket: %s", strerror(errno));


	local.sun_family = AF_UNIX;
	strcpy(local.sun_path, CONTROL_SOCKET);
	unlink(local.sun_path);
	len = strlen(local.sun_path) + sizeof(local.sun_family);
	if (bind(s, (struct sockaddr*)&local, len) == -1)
		FATAL("bind: %s", strerror(errno));

	if (chmod(CONTROL_SOCKET, S_IRUSR|S_IWUSR) == -1)
		FATAL("chmod: %s", strerror(errno));

	if (listen(s, 1) == -1)
		FATAL("listen: %s", strerror(errno));

	while (1) {
		int n, i, start, done = 0;
		VERBOSE("waiting for connection on " CONTROL_SOCKET);
		t = sizeof(remote);
		if ((s2 = accept(s, (struct sockaddr*)&remote, &t)) == -1)
			FATAL("accept: %s", strerror(errno));

		do {
			n = recv(s2, buf + buf_len, BUF_SIZE - buf_len, 0);
			if (n == 0) {
				ERROR("recv: connection reset by peer");
				break;
			}
			if (n < 0) {
				ERROR("recv: %s", strerror(errno));
				break;
			}

			//VERBOSE("got %d bytes", n);
			start = 0;
			for (i = buf_len; i < buf_len + n; i++) {
				if (buf[i] == '\n') {
					buf[i] = '\0';
					if (control_call(s2, start + buf)) {
						done = 1;
						break;
					}
					start = i + 1;
				}
			}

			buf_len += n;

			memmove(buf, buf + start, buf_len - start);
			buf_len -= start;

			if (buf_len == BUF_SIZE) {
				ERROR("command too long");
				break;
			}

		} while(!done);

		close(s2);
	}

	pthread_exit(NULL);
}

#if 0
#define CONTROL_MAXARGS 10

static char *control_argv[FUSE_IO_MAXARGS];
static int control_argc;


static void control_response(const char *fmt, ...) {
	int bla;
	va_list ap;
	va_start(ap, fmt);

	bla = vsnprintf(control_read_buf + control_read_buf_len,
			FUSE_IO_READ_BUF_SIZE - control_read_buf_len,
			fmt, ap);
	if (bla >= FUSE_IO_READ_BUF_SIZE - control_read_buf_len)
		control_read_buf_len = FUSE_IO_READ_BUF_SIZE;
	else control_read_buf_len += bla;

	va_end(ap);
}

static void control_call(void) {
	if (control_argc == 0) return;

	if (!strcmp(control_argv[0], "status") && control_argc == 1)
		control_status();
	else if (!strcmp(control_argv[0], "open") && control_argc == 4)
		control_open(control_argv[1], control_argv[2], control_argv[3]);
	else control_response("ERR\nunknown command %s or wrong number"
			" of arguments\n.\n", control_argv[0]);

	VERBOSE("handled command %s", control_argv[0]);
}

static int control_write(const char *buf, size_t size, off_t offset) {
	pthd_mutex_lock(&control_mutex);
	while (size--) {
		if (control_write_buf_len >= FUSE_IO_WRITE_BUF_SIZE)
			return -ENOSPC;

		control_write_buf[control_write_buf_len] = *buf;
		if (*buf++ == '\n') {
			control_argc = 0;
			control_write_buf[control_write_buf_len] = '\0';
			control_argv[control_argc] = control_write_buf;

			do {
				while (*control_argv[control_argc] == ' ')
					control_argv[control_argc]++;

				control_argc++;

				if (control_argc > FUSE_IO_MAXARGS)
					return -ENOSPC;

				control_argv[control_argc] =
					control_argv[control_argc - 1];

				while (*control_argv[control_argc] != '\0' &&
						*control_argv[control_argc]
						!= ' ')
					control_argv[control_argc]++;

				if (*control_argv[control_argc] == ' ')
					*control_argv[control_argc]++ = '\0';

			} while (*control_argv[control_argc] != '\0');

			control_call();

			control_write_buf_len = 0;
		} else control_write_buf_len++;
	}
	pthd_mutex_unlock(&control_mutex);

	return size;
}
#endif
