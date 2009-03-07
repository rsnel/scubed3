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
#include <fuse.h>
#include "verbose.h"
#include "util.h"
#include "pthd.h"
#include "hashtbl.h"
#include "control.h"
#include "fuse_io.h"
#include "ecch.h"

#define BUF_SIZE 8192
#define MAX_ARGC 10

int control_write_string(int s, const char *string, ssize_t len) {
	ssize_t sent = 0, n;
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

	return 0;
}

int control_write_complete(int s, int err, const char *format, ...) {
	char *string;
	//ssize_t sent = 0, len, n;
	ssize_t len;
	va_list ap;

	if (!err) {
		if (control_write_string(s, "OK\n", 3)) return -1;
	} else {
		if (control_write_string(s, "ERR\n", 4)) return -1;
	}

	va_start(ap, format);
	if ((len = vasprintf(&string, format, ap)) == -1) {
		ERROR("vasprintf: %s", strerror(errno));
		return -1;
	}
	va_end(ap);

	//VERBOSE("->%s<-", string);

	if (control_write_string(s, string, len)) {
		free(string);
		return -1;
	}

	free(string);

	return control_write_string(s, "\n.\n", 3);
}

int control_status(int s, control_thread_priv_t *priv) {
	return control_write_complete(s, 0, "no info available");
}

void some_bullshit() {
	ecch_throw(ECCH_DEFAULT, "some error");
}

int control_open(int s, control_thread_priv_t *priv, char *name,
		char *cipher_spec, char *key) {
	fuse_io_entry_t *entry;
	char *allocname;
	VERBOSE("got request to open \"%s\", %s, %s", name,
			cipher_spec, key);

	allocname = estrdup(name);
	entry = hashtbl_allocate_and_add_element(priv->h,
			allocname, sizeof(*entry));

	if (!entry) {
		free(allocname);
		return control_write_complete(s, 1,
				"unable to add partition \"%s\"", name);
	}

	entry->to_be_deleted = 0;
	entry->inuse = 0;

	ecch_try {
		/* if any of this fails, we cannot add the partition */
		size_t key_len;
		key_len = strlen(key);
		if (key_len%2) ecch_throw(ECCH_DEFAULT, "cipher key not valid "
				"base16 (uneven number of chars)");

		if (unbase16(key, key_len)) ecch_throw(ECCH_DEFAULT, "cipher "
				"key not valid base16 (invalid chars)");

		cipher_init(&entry->c, cipher_spec, 1024,
				(unsigned char*)key, key_len/2);
		blockio_dev_init(&entry->d, priv->b, &entry->c, name);
		scubed3_init(&entry->l, &entry->d);
		entry->size = ((entry->l.dev->no_macroblocks-entry->
					l.dev->reserved)*entry->l.dev->mmpm)
			<<entry->l.dev->b->mesoblk_log;
	}
	ecch_catch_all {
		entry->to_be_deleted = 1;
		hashtbl_unlock_element_byptr(entry);
		hashtbl_delete_element_byptr(priv->h, entry);
		return control_write_complete(s, 1, "%s",
				ecch_context.ecch.msg);
	}
	ecch_endtry;

	hashtbl_unlock_element_byptr(entry);

	return control_write_complete(s, 0, "partition \"%s\" added", name);
}

int control_close(int s, control_thread_priv_t *priv, char *name) {
	fuse_io_entry_t *entry = hashtbl_find_element_bykey(priv->h, name);
	if (!entry) return control_write_complete(s, 1,
			"partition \"%s\" not found", name);

	if (entry->inuse) {
		hashtbl_unlock_element_byptr(entry);
		return control_write_complete(s, 1,
				"partition \"%s\" is busy", name);
	}

	entry->to_be_deleted = 1;
	hashtbl_unlock_element_byptr(entry);

	hashtbl_delete_element_byptr(priv->h, entry);

	return control_write_complete(s, 0, "partition \"%s\" closed", name);
}

int control_call(int s, control_thread_priv_t *priv, char *command) {
	int argc = 0, len;
	char *argv[MAX_ARGC+1];

	argv[argc] = command;
	len = strlen(command);

	//VERBOSE("recv: %s", command);

	do {
		while (*argv[argc] == ' ') argv[argc]++;

		argc++;

		if (argc > MAX_ARGC) return control_write_complete(s, 1,
				"too many arguments");

		argv[argc] = argv[argc-1];

		while (*argv[argc] != '\0' && *argv[argc] != ' ') argv[argc]++;

		if (*argv[argc] == ' ') *argv[argc]++ = '\0';

	} while (*argv[argc] != '\0');

	if (!strcmp(argv[0],"status") && argc == 1)
		return control_status(s, priv);
	if (!strcmp(argv[0], "close") && argc == 2)
		return control_close(s, priv, argv[1]);
	if (!strcmp(argv[0], "open") && argc == 4)
		return control_open(s, priv, argv[1], argv[2], argv[3]);
	else return control_write_complete(s, 1, "unknown command \"%s\" or "
			"wrong number of arguments", argv[0]);

	memset(command, 0, len);
}

void *control_thread(void *arg) {
	control_thread_priv_t *priv = arg;
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
					if (control_call(s2, priv, start + buf))
					{
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

