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
#include <unistd.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <signal.h>
#include <fuse.h>
#include <assert.h>
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

int control_write_silent_success(int s) {
	return control_write_string(s, "OK\n.\n", 5);
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

static int parse_int(int s, int *r, const char *in) {
        char *end;
        int ret = strtol(in, &end, 10);

        if (errno && (ret == LONG_MIN || ret == LONG_MAX))
		return control_write_complete(s, 1, "integer out of range\n");

        if (*end != '\0') 
                return control_write_complete(s, 1, "unable to parse ->%s<-\n", in);

        *r = ret;

	return 0;
}

static int control_cycle(int s, control_thread_priv_t *priv, char *argv[]) {
	fuse_io_entry_t *entry = hashtbl_find_element_bykey(priv->h, argv[0]);
	int times;
	if (!entry) return control_write_complete(s, 1,
			"partition \"%s\" not found", argv[0]);

	if (parse_int(s, &times, argv[1])) return -1;

	if (times < 0) {
		hashtbl_unlock_element_byptr(entry);
		return control_write_complete(s, 1,
				"integer must be positive");
	}

	while (times--) scubed3_cycle(&entry->l);

	hashtbl_unlock_element_byptr(entry);

	return control_write_complete(s, 0, "see debug output");
}

static int control_verbose_ordered(int s, control_thread_priv_t *priv, char *argv[]) {
	fuse_io_entry_t *entry = hashtbl_find_element_bykey(priv->h, argv[0]);
	if (!entry) return control_write_complete(s, 1,
			"partition \"%s\" not found", argv[0]);

	blockio_verbose_ordered(&entry->d);

	hashtbl_unlock_element_byptr(entry);

	return control_write_complete(s, 0, "see debug output");
}

static int control_check_data_integrity(int s, control_thread_priv_t *priv, char *argv[]) {
	void *check_block(blockio_info_t *bi) {
		if (blockio_check_data_hash(bi)) {
			VERBOSE("%d %lld OK", bi - bi->dev->b->blockio_infos,
					bi->seqno);
		} else {
			VERBOSE("%d %lld FAIL", bi - bi->dev->b->blockio_infos,
					bi->seqno);
		}
		return NULL;
	}
	fuse_io_entry_t *entry = hashtbl_find_element_bykey(priv->h, argv[0]);
	if (!entry) return control_write_complete(s, 1,
			"partition \"%s\" not found", argv[0]);

	dllarr_iterate(&entry->d.used_blocks, (dllarr_iterator_t)check_block, NULL);

	hashtbl_unlock_element_byptr(entry);

	return control_write_complete(s, 0, "see debug output");
}

static int control_static_info(int s, control_thread_priv_t *priv, char *argv[]) {
	return control_write_complete(s, 0, "%s\n%d\n%s",
			priv->mountpoint,
			priv->b->no_macroblocks,
			VERSION);
}

static int control_exit(int s, control_thread_priv_t *priv, char *argv[]) {
	control_write_complete(s, 0, "kthxbye!");
	//VERBOSE("closing connection");
	return -1; // close connection
}

typedef struct control_help_priv_s {
	int s; /* socket */
} control_help_priv_t;

static int control_help(int s, control_thread_priv_t *priv, char *argv[]) {
	control_help_priv_t help_priv = {
		.s = s
	};
	int rep(control_help_priv_t *priv, control_command_t *c) {
		return control_write_line(priv->s, "%s%s\n",
				c->head.key, c->usage);
	}

	if (control_write_status(s, 0)) return -1;

	if (hashtbl_ts_traverse(&priv->c,
				(int (*)(void*, hashtbl_elt_t*))rep,
				&help_priv)) return -1;

	return control_write_terminate(s);
}

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
		size_t size = ((entry->d.rev[0].no_macroblocks - entry->d.reserved_macroblocks)*entry->d.mmpm);
		priv->macroblocks_left -= entry->d.rev[0].no_macroblocks;
                return control_write_line(priv->s, "%07u blocks in %s (%.1fMiB)%s%s%s%s%s\n", entry->d.rev[0].no_macroblocks, entry->head.key, ((entry->d.rev[0].no_macroblocks >= entry->d.reserved_macroblocks)?size<<entry->d.b->mesoblk_log:0)/(1024.*1024.), entry->inuse?" [U]":"", entry->close_on_release?" [C]":"", entry->mountpoint?" [MNT ":"", entry->mountpoint?entry->mountpoint:"", entry->mountpoint?"]":"");
        }

	if (control_write_status(s, 0)) return -1;

//	if (control_write_line(s, "consists of %u macroblocks of %u bytes\n", priv->b->no_macroblocks, priv->b->macroblock_size)) return -1;
//	if (control_write_line(s, "each macroblock has %d mesoblocks of %u bytes\n", 1<<(priv->b->macroblock_log - priv->b->mesoblk_log), 1<<priv->b->mesoblk_log)) return -1;

	if (hashtbl_ts_traverse(priv->h, (int (*)(void*, hashtbl_elt_t*))rep, &status_priv)) return -1;

	if (control_write_line(s, "%07u blocks unclaimed\n", status_priv.macroblocks_left)) return -1;
	if (control_write_line(s, "%07u blocks total\n", priv->b->no_macroblocks)) return -1;
	return control_write_terminate(s);
}

static int check_name(const char *ptr) {
	while (*ptr != '\0') {
		if (!(*ptr >= 'a' && *ptr <= 'z') &&
				!(*ptr >= 'A' && *ptr <= 'Z') &&
				!(*ptr >= '0' && *ptr <= '9') &&
				!(*ptr == '_'))
			return -1;
		ptr++;
	}

	return 0;
}

static int control_open_create_common(int s, control_thread_priv_t *priv, char *argv[], int add) {
	fuse_io_entry_t *entry;
	char *allocname;

	if (check_name(argv[0])) return control_write_complete(s, 1,
				"illegal name specified for partition, "
				"it may only contain letters, digits and "
				"the underscore");

	allocname = estrdup(argv[0]);
	entry = hashtbl_allocate_and_add_element(priv->h,
			allocname, sizeof(*entry));

	if (!entry) {
		free(allocname);
		return control_write_complete(s, 1,
				"unable to %s partition \"%s\", duplicate "
				"name?", add?"create":"open", argv[0]);
	}

	entry->to_be_deleted = 0;
	assert(!entry->close_on_release);
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
		cipher_enc(&entry->c, (unsigned char*)buf, (unsigned char*)buf, 0, 0, 0);
		gcry_md_hash_buffer(GCRY_MD_SHA256, entry->unique_id.id, buf, sizeof(buf));
		entry->unique_id.head.key = entry->unique_id.id;
		entry->unique_id.name = allocname;
		entry->d.name = estrdup(allocname);
		if (!hashtbl_add_element(priv->ids, &entry->unique_id))
			ecch_throw(ECCH_DEFAULT, "cipher(mode)/key combination already in use");
		hashtbl_unlock_element_byptr(&entry->unique_id);	
		entry->ids = priv->ids;

		blockio_dev_init(&entry->d, priv->b, &entry->c, argv[0]);
		if (add && entry->d.rev[0].no_macroblocks) 
			ecch_throw(ECCH_DEFAULT, "unable to create device: it already exists, use `open' instead");
		if (!add & !entry->d.rev[0].no_macroblocks)
			ecch_throw(ECCH_DEFAULT, "unable to open device: passphrase wrong?");
		entry->size = 0;
		if (entry->d.rev[0].no_macroblocks > entry->d.reserved_macroblocks) entry->size = ((entry->d.rev[0].no_macroblocks-entry->d.reserved_macroblocks)<<entry->d.b->mesoblk_log)*entry->d.mmpm;
		scubed3_init(&entry->l, &entry->d);
		//ecch_throw(ECCH_DEFAULT, "break off, we're testing");
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

	return control_write_silent_success(s);
	//control_write_complete(s, 0, "partition \"%s\" %s", argv[0], add?"created":"opened");
}

static int control_open(int s, control_thread_priv_t *priv, char *argv[]) {
	return control_open_create_common(s, priv, argv, 0);
}

static int control_create(int s, control_thread_priv_t *priv, char *argv[]) {
	return control_open_create_common(s, priv, argv, 1);
}

static int control_check_available(int s,
		control_thread_priv_t *priv, char *argv[]) {
	fuse_io_entry_t *entry;

	if (check_name(argv[0])) return control_write_complete(s, 1,
				"illegal name specified for partition, "
				"it may only contain letters, digits and "
				"the underscore");

	entry = hashtbl_find_element_bykey(priv->h, argv[0]);

	if (!entry) return control_write_silent_success(s);

	hashtbl_unlock_element_byptr(entry);

	return control_write_complete(s, 1,
			"partition \"%s\" is in use", argv[0]);
}

static int control_set_aux(int s, control_thread_priv_t *priv, char *argv[]) {
	fuse_io_entry_t *entry = hashtbl_find_element_bykey(priv->h, argv[0]);
	int err = 0;

	if (!entry) return control_write_complete(s, 1,
			"partition \"%s\" not found", argv[0]);
	if (!strcmp(argv[1], "mountpoint")) {
		if (entry->mountpoint) free(entry->mountpoint);
		entry->mountpoint = strdup(argv[2]);
		if (!entry->mountpoint) {
			hashtbl_unlock_element_byptr(entry);
			return control_write_complete(s, 1, "out of memory");
		}
	} else err = 1;

	hashtbl_unlock_element_byptr(entry);

	if (err) return control_write_complete(s, 1,
			"key \"%s\" is impossible", argv[1]);

	return control_write_silent_success(s);
}

static int control_get_aux(int s, control_thread_priv_t *priv, char *argv[]) {
	fuse_io_entry_t *entry = hashtbl_find_element_bykey(priv->h, argv[0]);
	char *ans = NULL;

	if (!entry) return control_write_complete(s, 1,
			"partition \"%s\" not found", argv[0]);

	if (!strcmp(argv[1], "mountpoint")) {
		ans = entry->mountpoint;
	}

	hashtbl_unlock_element_byptr(entry);

	if (!ans) return control_write_complete(s, 1,
			"key \"%s\" not found", argv[1]);

	return control_write_complete(s, 0, "%s", ans);
}

static int control_set_close_on_release(int s,
		control_thread_priv_t *priv, char *argv[]) {
	fuse_io_entry_t *entry = hashtbl_find_element_bykey(priv->h, argv[0]);
	int err = 0;

	if (!entry) return control_write_complete(s, 1,
			"partition \"%s\" not found", argv[0]);

	if (!strcmp(argv[1], "0") || !strcasecmp(argv[1], "false")) {
		entry->close_on_release = 0;
	} else if (!strcmp(argv[1], "1") || !strcasecmp(argv[1], "true")) {
		entry->close_on_release = 1;
	} else err = 1;

	hashtbl_unlock_element_byptr(entry);

	if (err) return control_write_complete(s, 1,
			"illegal argument; expected boolean");

	return control_write_silent_success(s);
}

static int control_info(int s, control_thread_priv_t *priv, char *argv[]) {
	fuse_io_entry_t *entry = hashtbl_find_element_bykey(priv->h, argv[0]);
	if (!entry) return control_write_complete(s, 1,
			"partition \"%s\" not found", argv[0]);

		// UNLOCK ELEMENTS NEEDED
	if (control_write_status(s, 0)) return -1;

	if (control_write_line(s, "no_macroblocks=%d\n",
				entry->d.rev[0].no_macroblocks)) return -1;

	if (control_write_line(s, "keep_revisions=%d\n",
				entry->d.keep_revisions)) return -1;

	if (control_write_line(s, "reserved_macroblocks=%d\n",
				entry->d.reserved_macroblocks)) return -1;

	if (control_write_line(s, "used=%d\n",
				dllarr_count(&entry->d.used_blocks)))
		return -1;

	if (control_write_line(s, "selected=%d\n",
				dllarr_count(&entry->d.selected_blocks)))
		return -1;

	if (control_write_line(s, "free=%d\n",
				dllarr_count(&entry->d.free_blocks)))
		return -1;

	if (control_write_line(s, "writes=%d\n",entry->d.writes))
		return -1;

	if (control_write_line(s, "useful=%d\n",entry->d.useful))
		return -1;

	if (control_write_line(s, "wasted_keep=%d\n",entry->d.wasted_keep))
		return -1;

	if (control_write_line(s, "wasted_gc=%d\n",entry->d.wasted_gc))
		return -1;

	if (control_write_line(s, "pre_emptive_gc=%d\n",entry->d.pre_emptive_gc))
		return -1;

	if (control_write_line(s, "wasted_empty=%d\n",entry->d.wasted_empty))
		return -1;

	if (control_write_line(s, "layout_revision=%d\n",entry->d.layout_revision))
		return -1;

	if (entry->d.bi) {
		if (control_write_line(s, "no_indices=%d\n",
					entry->d.bi->no_indices)) return -1;
	}

	if (control_write_line(s, "updated=%d\n", entry->d.updated)) return -1;

	hashtbl_unlock_element_byptr(entry);

	return control_write_terminate(s);
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

	//return control_write_complete(s, 0, "partition \"%s\" closed", argv[0]);
	return control_write_silent_success(s);
}

static int control_resize(int s, control_thread_priv_t *priv, char *argv[]) {
	int size = 0, reserved = 0, keep = 0; // shut compiler up
	int err;
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

	if (parse_int(s, &size, argv[1])) return -1;
	if (parse_int(s, &reserved, argv[2])) return -1;
	if (parse_int(s, &keep, argv[3])) return -1;

	if (!(reserved <= size) || !(keep <= reserved) || !(size >= 0)) {
		hashtbl_unlock_element_byptr(entry);
		return control_write_complete(s, 1,
			"something wrong with caller");
	}

	if (reserved != dev->reserved_macroblocks && dev->bi) {
		hashtbl_unlock_element_byptr(entry);
		return control_write_complete(s, 1, "unable to change amount "
				"of reserved blocks");
	}

	if (reserved <= 4 && keep != 0) {
		hashtbl_unlock_element_byptr(entry);
		return control_write_complete(s, 1, "if reserved < 4, then "
				"keep revisions must be 0");
	}

	if (reserved > 4 && keep > reserved - 4) {
		hashtbl_unlock_element_byptr(entry);
		return control_write_complete(s, 1, "if reserved > 0, then "
				"keep revisions must be smaller than "
				"reserved - 4");
	}

#if 1
	if (size < dev->rev[0].no_macroblocks) {
		blockio_dev_free_macroblocks(dev, dev->rev[0].no_macroblocks - size);
		hashtbl_unlock_element_byptr(entry);
		return control_write_complete(s, 1, "very experimental; "
				"not yet supported");
	}
#endif

	if (size > dev->b->no_macroblocks) {
		hashtbl_unlock_element_byptr(entry);
		return control_write_complete(s, 1,
				"not enough blocks available, base device "
				"has only %d blocks", dev->b->no_macroblocks);
	}

	size -= dev->rev[0].no_macroblocks;

	if (size == 0) {
		hashtbl_unlock_element_byptr(entry);
		return control_write_complete(s, 1, "nothing to do");
	}

	VERBOSE("need to allocate %d additional blocks", size);

	if ((err = blockio_dev_allocate_macroblocks(dev, size))) {
		hashtbl_unlock_element_byptr(entry);
		return control_write_complete(s, 1,
				(err == -1)?"not enough unclaimed blocks "
				"available for resize":"out of memory error");
	}

	dev->keep_revisions = keep;
	dev->reserved_macroblocks = reserved;

	if (!dev->bi) blockio_dev_select_next_macroblock(dev, 1);

	dev->updated = 1;

	if (entry->d.rev[0].no_macroblocks > entry->d.reserved_macroblocks)
		entry->size = ((entry->d.rev[0].no_macroblocks - 
					entry->d.reserved_macroblocks)<<
				entry->d.b->mesoblk_log)*entry->d.mmpm;
	scubed3_reinit(&entry->l);

	hashtbl_unlock_element_byptr(entry);

	return control_write_silent_success(s);
}

static control_command_t control_commands[] = {
	{
		.head.key = "static-info",
		.command = control_static_info,
		.argc = 0,
		.usage = ""
	}, {
		.head.key = "exit",
		.command = control_exit,
		.argc = 0,
		.usage = ""
	}, {
		.head.key = "p",
		.command = control_status,
		.argc = 0,
		.usage = ""
	}, {
		.head.key = "set-close-on-release",
		.command = control_set_close_on_release,
		.argc = 2,
		.usage = " NAME BOOL"
	}, {
		.head.key = "check-available",
		.command = control_check_available,
		.argc = 1,
		.usage = " NAME"
	}, {
		.head.key = "cycle",
		.command = control_cycle,
		.argc = 2,
		.usage = " NAME COUNT"
	}, {
		.head.key = "verbose-ordered",
		.command = control_verbose_ordered,
		.argc = 1,
		.usage = " NAME"
	}, {
		.head.key = "check-data-integrity",
		.command = control_check_data_integrity,
		.argc = 1,
		.usage = " NAME"
	}, {
		.head.key = "open-internal",
		.command = control_open,
		.argc = 3,
		.usage = " NAME CIPHER_SPEC KEY"
	}, {
		.head.key = "info",
		.command = control_info,
		.argc = 1,
		.usage = " NAME"
	}, {
		.head.key = "get-aux",
		.command = control_get_aux,
		.argc = 2,
		.usage = " NAME KEY"
	}, {
		.head.key = "set-aux",
		.command = control_set_aux,
		.argc = 3,
		.usage = " NAME KEY VALUE"
	}, {
		.head.key = "create-internal",
		.command = control_create,
		.argc = 3,
		.usage = " NAME CIPHER_SPEC KEY"
	}, {
		.head.key = "help-internal",
		.command = control_help,
		.argc = 0,
		.usage = ""
	}, {
		.head.key = "close",
		.command = control_close,
		.argc = 1,
		.usage = " NAME"
	}, {
		.head.key = "resize-internal",
		.command = control_resize,
		.argc = 4,
		.usage = " NAME BLOCKS RESERVED KEEP"
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

		while (*argv[argc] == ' ') *argv[argc]++ = '\0';

	} while (*argv[argc] != '\0');

	if (argc == 1 && *argv[0] == '\0') return control_write_silent_success(s);

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

	VERBOSE("listening for connection with scubed2ctl on " CONTROL_SOCKET);

	while (1) {
		int n, i, start, done = 0, ret;
		t = sizeof(remote);
		if ((s2 = accept(s, (struct sockaddr*)&remote, &t)) == -1)
			FATAL("accept: %s", strerror(errno));

		buf_len = 0;
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


#if 0
		fprintf(stderr, "all macroblocks: ");
		for (i = 0; i < dev->b->no_macroblocks; i++) {
			fprintf(stderr, "%d",
					bitmap_getbits(&dev->status, i<<1, 2));
		}
		fprintf(stderr, "\n");

		fprintf(stderr, "our macroblocks: ");
		for (i = 0; i < dev->no_macroblocks; i++) {
			fprintf(stderr, "%d",
					blockio_dev_get_macroblock_status_old(dev, i));
		}
		fprintf(stderr, "\n");
#endif
