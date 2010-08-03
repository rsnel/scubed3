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
#include <limits.h>
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
#define DEFAULT_RESERVED_MACROBLOCKS 10
#define DEFAULT_KEEP_REVISIONS 3

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

int control_write_status(int s, int err) {
	if (!err) {
		if (control_write_string(s, "OK\n", 3)) return -1;
	} else {
		if (control_write_string(s, "ERR\n", 4)) return -1;
	}

	return 0;
}

int control_vwrite_line(int s, const char *format, va_list ap) {
	int ret, len;
	char *string;
	if ((len = vasprintf(&string, format, ap)) == -1) {
		ERROR("vasprintf: %s", strerror(errno));
		return -1;
	}

	ret = control_write_string(s, string, len);

	free(string);

	return ret;
}

int control_write_line(int s, const char *format, ...) {
	int ret;
	va_list ap;

	va_start(ap, format);
	ret = control_vwrite_line(s, format, ap);
	va_end(ap);

	return ret;
}

int control_write_terminate(s) {
	return control_write_string(s, ".\n", 2);
}

int control_write_complete(int s, int err, const char *format, ...) {
	va_list ap;
	int ret;

	if (control_write_status(s, err)) return -1;

	va_start(ap, format);
	ret = control_vwrite_line(s, format, ap);
	va_end(ap);

	if (ret) return -1;

	return control_write_string(s, "\n.\n", 3);
}

typedef struct control_command {
	hashtbl_elt_t head;
	int (*command)(int, control_thread_priv_t*, char**);
	int argc;
	char *usage;
} control_command_t;

	//if (control_write_line(s, "consists of %u macroblocks of %u bytes\n", priv->b->no_macroblocks, priv->b->macroblock_size)) return -1;
	//if (control_write_line(s, "each macroblock has %d mesoblocks of %u bytes\n", 1<<(priv->b->macroblock_log - priv->b->mesoblk_log), 1<<priv->b->mesoblk_log)) return -1;

typedef struct control_status_priv_s {
	int s; /* socket */
	uint32_t macroblocks_left;
} control_status_priv_t;

static int control_status(int s, control_thread_priv_t *priv, char *argv[]) {
	control_status_priv_t status_priv = {
		.s = s,
		.macroblocks_left = priv->b->no_macroblocks
	};
        int rep(control_status_priv_t *priv, fuse_io_entry_t *entry) {
		priv->macroblocks_left -= entry->d.no_macroblocks;
                return control_write_line(priv->s, "%07u blocks in %s\n", entry->d.no_macroblocks, entry->head.key);
        }

	if (control_write_status(s, 0)) return -1;

	if (hashtbl_ts_traverse(priv->h, (int (*)(void*, hashtbl_elt_t*))rep, &status_priv)) return -1;

	if (control_write_line(s, "%07u blocks unclaimed\n", status_priv.macroblocks_left)) return -1;
	if (control_write_line(s, "%07u blocks total\n", priv->b->no_macroblocks)) return -1;
	return control_write_terminate(s);
}

static int control_open_add_common(int s, control_thread_priv_t *priv, char *argv[]) {
	fuse_io_entry_t *entry;
	char *allocname;

	allocname = estrdup(argv[0]);
	entry = hashtbl_allocate_and_add_element(priv->h,
			allocname, sizeof(*entry));

	if (!entry) {
		free(allocname);
		return control_write_complete(s, 1,
				"unable to add partition \"%s\", duplicate name?", argv[0]);
	}

	entry->to_be_deleted = 0;
	entry->inuse = 0;

	ecch_try {
		/* if any of this fails, we cannot add the partition */
		size_t key_len;
		char buf[1<<priv->b->mesoblk_log];

		memset(buf, 0, 1<<priv->b->mesoblk_log);

		key_len = strlen(argv[2]);
		if (key_len%2) ecch_throw(ECCH_DEFAULT, "cipher key not valid "
				"base16 (uneven number of chars)");

		if (unbase16(argv[2], key_len)) ecch_throw(ECCH_DEFAULT, "cipher "
				"key not valid base16 (invalid chars)");

		cipher_init(&entry->c, argv[1], 1024,
				(unsigned char*)argv[2], key_len/2);
		
		// encrypt zeroed buffer and hash the result
		// the output of the hash is used to ID ciphermode + key
		cipher_enc(&entry->c, (unsigned char*)buf, (unsigned char*)buf, 0, 0);
		gcry_md_hash_buffer(GCRY_MD_SHA256, entry->unique_id.id, buf, sizeof(buf));
		entry->unique_id.head.key = entry->unique_id.id;
		entry->unique_id.name = allocname;
		entry->d.name = estrdup(allocname);
		if (!hashtbl_add_element(priv->ids, &entry->unique_id))
			ecch_throw(ECCH_DEFAULT, "cipher(mode)/key combination already in use");
		hashtbl_unlock_element_byptr(&entry->unique_id);	

		blockio_dev_init(&entry->d, priv->b, &entry->c, argv[0]);
		entry->size = 0;
		entry->ids = priv->ids;
		//scubed3_init(&entry->l, &entry->d);
		//entry->size = ((entry->l.dev->no_macroblocks-entry->
		//			l.dev->reserved)*entry->l.dev->mmpm)
		//	<<entry->l.dev->b->mesoblk_log;
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

	return control_write_complete(s, 0, "partition \"%s\" added", argv[0]);
}

static int control_open(int s, control_thread_priv_t *priv, char *argv[]) {
	return control_open_add_common(s, priv, argv);
}

static int control_add(int s, control_thread_priv_t *priv, char *argv[]) {
	return control_open_add_common(s, priv, argv);
}

static int control_close(int s, control_thread_priv_t *priv, char *argv[]) {
	fuse_io_entry_t *entry = hashtbl_find_element_bykey(priv->h, argv[0]);
	if (!entry) return control_write_complete(s, 1,
			"partition \"%s\" not found", argv[0]);

	if (entry->inuse) {
		hashtbl_unlock_element_byptr(entry);
		return control_write_complete(s, 1,
				"partition \"%s\" is busy", argv[0]);
	}

	entry->to_be_deleted = 1;
	hashtbl_unlock_element_byptr(entry);

	hashtbl_delete_element_byptr(priv->h, entry);

	return control_write_complete(s, 0, "partition \"%s\" closed", argv[0]);
}

static int control_resize(int s, control_thread_priv_t *priv, char *argv[]) {
	long int size;
	char *end = NULL;
	fuse_io_entry_t *entry = hashtbl_find_element_bykey(priv->h, argv[0]);
	blockio_dev_t *dev;

	if (!entry) return control_write_complete(s, 1,
			"partition \"%s\" not found", argv[0]);

	if (entry->inuse) {
		hashtbl_unlock_element_byptr(entry);
		return control_write_complete(s, 1,
				"partition \"%s\" is busy", argv[0]);
	}
	dev = &entry->d;
	size = strtol(argv[1], &end, 10);
	if (errno && (size == LONG_MIN || size == LONG_MAX)) {
		hashtbl_unlock_element_byptr(entry);
		return control_write_complete(s, 1, "unable to parse ->%s<- to a number: %s", argv[1], strerror(errno));
	}
	if (*end != '\0') {
		hashtbl_unlock_element_byptr(entry);
		return control_write_complete(s, 1, "unable to parse ->%s<- to a number", argv[1]);
	}
	
	if (size == 0) {
		hashtbl_unlock_element_byptr(entry);
		return control_write_complete(s, 1, "we cannot resize to zero");
	}

	if (dev->no_macroblocks) {
		hashtbl_unlock_element_byptr(entry);
		return control_write_complete(s, 1, "unable to resize a device with allocated blocks");
	}

	if (size < dev->no_macroblocks) {
		hashtbl_unlock_element_byptr(entry);
		return control_write_complete(s, 1, "unable to shrink device");
	}

	if (size > dev->b->no_macroblocks) {
		hashtbl_unlock_element_byptr(entry);
		return control_write_complete(s, 1, "not enough blocks available, base device has only %d blocks", dev->b->no_macroblocks);
	}

	{ /* build an array of free blocks */
		int i, no_freeb = 0;
		uint16_t freeb[dev->b->no_macroblocks];
		uint16_t no, select = 0;
		void *tmp;
		assert(dev->b->no_macroblocks <= 65536);
		for (i = 0; i < dev->b->no_macroblocks; i++)
			if (!dev->b->blockio_infos[i].dev) freeb[no_freeb++] = i;

		assert(dev->no_macroblocks == 0);
		VERBOSE("we have %d free blocks to chose from", no_freeb);
		tmp = realloc(dev->our_macroblocks, sizeof(dev->our_macroblocks[0])*size);
		if (!tmp) {
			hashtbl_unlock_element_byptr(entry);
			return control_write_complete(s, 1, "out of memory error");
		}
		dev->our_macroblocks = tmp;

		while (size) {
			blockio_info_t *bi;
			no = random_custom(&dev->b->r, no_freeb);
			select = freeb[no];
			VERBOSE("select nr %d, %d", no, select);
			bi = &dev->b->blockio_infos[select];
			bi->dev = dev;
			
			// mark as allocated but free
			bitmap_setbit(&dev->status, select<<1); 
			assert(!bitmap_getbit(&dev->status, (select<<1) + 1));

			dev->our_macroblocks[dev->no_macroblocks++] = bi;
			no_freeb--;
			memmove(&freeb[no], &freeb[no+1], sizeof(freeb[0])*(no_freeb - no));
			size--;
		}
		random_init(&dev->r, dev->no_macroblocks);
		dev->updated = 1;
	}


	hashtbl_unlock_element_byptr(entry);
	
	return control_write_complete(s, 0, "succesful resized");
}

static control_command_t control_commands[] = {
	{
		.head.key = "p",
		.command = control_status,
		.argc = 0,
		.usage = ""
	},
	{
		.head.key = "open-internal",
		.command = control_open,
		.argc = 3,
		.usage = " NAME CIPHER_SPEC KEY"
	},
	{
		.head.key = "add-internal",
		.command = control_add,
		.argc = 3,
		.usage = " NAME CIPHER_SPEC KEY"
	},
	{
		.head.key = "close",
		.command = control_close,
		.argc = 1,
		.usage = " NAME"
	},
	{
		.head.key = "resize-force",
		.command = control_resize,
		.argc = 2,
		.usage = " NAME BLOCKS"
	}
};

#define NO_COMMANDS (sizeof(control_commands)/sizeof(control_commands[0]))

int control_call(int s, control_thread_priv_t *priv, char *command) {
	int argc = 0;
	char *argv[MAX_ARGC+1];
	control_command_t *cmnd;
	argv[argc] = command;

	do {
		while (*argv[argc] == ' ') argv[argc]++;

		argc++;

		if (argc > MAX_ARGC) return control_write_complete(s, 1,
				"too many arguments");

		argv[argc] = argv[argc-1];

		while (*argv[argc] != '\0' && *argv[argc] != ' ') argv[argc]++;

		if (*argv[argc] == ' ') *argv[argc]++ = '\0';

	} while (*argv[argc] != '\0');

	if (!(cmnd = hashtbl_find_element_bykey(&priv->c, argv[0])))
		return control_write_complete(s, 1, "unknown command \"%s\"", argv[0]);

	if (argc - 1 != cmnd->argc) 
		return control_write_complete(s, 1, "usage: %s%s", argv[0], cmnd->usage);

	return (*cmnd->command)(s, priv, argv + 1);
}

void *control_thread(void *arg) {
	control_thread_priv_t *priv = arg;
	int s, s2, i;
	socklen_t t, len;
	char buf[BUF_SIZE];
	int buf_len = 0;
	struct sockaddr_un local, remote;

	/* load all command descriptors in hash table */
	hashtbl_init_default(&priv->c, -1, 4, 0, 1, NULL);
	for (i = 0; i < NO_COMMANDS; i++) {
		//DEBUG("%d %s", i, control_commands[i].head.key);
		if (hashtbl_add_element(&priv->c, &control_commands[i]) == NULL)
			FATAL("duplicate command");
	}

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
		int n, i, start, done = 0, ret;
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
					ret = control_call(s2, priv, buf + start);

					/* clear command from memory */
					wipememory(buf + start, i);

					if (ret) {
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

