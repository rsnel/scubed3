/* fuse_io.c - interaction with fuse
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

#define FUSE_USE_VERSION 34
#include <fuse3/fuse.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include "verbose.h"
#include "fuse_io.h"
#include "hashtbl.h"
#include "util.h"
#include "blockio.h"
#include "pthd.h"
#include "plmgr.h"
#include "control.h"
#include "fuse_io.h"

typedef struct fuse_io_priv_s {
	hashtbl_t entries, ids;
	pthread_t control_thread;
	control_thread_priv_t control_thread_priv;
	pthread_t plmgr_thread;
	plmgr_thread_priv_t plmgr_thread_priv;
} fuse_io_priv_t;

static int fuse_io_getattr(const char *path, struct stat *stbuf, struct fuse_file_info *fi) {
	fuse_io_entry_t *entry;
	assert(path && *path == '/');

	memset(stbuf, 0, sizeof(*stbuf));

	if (!strcmp(path, "/")) {
		stbuf->st_mode = S_IFDIR|0755;
		stbuf->st_nlink = 2;
		return 0;
	}

	entry = hashtbl_find_element_bykey(
			&((fuse_io_priv_t*)fuse_get_context()->private_data)->
			entries, path + 1);
	if (!entry) return -ENOENT;

	if (!entry->readonly) stbuf->st_mode = S_IFREG|0600;
	else stbuf->st_mode = S_IFREG|0400;

	stbuf->st_nlink = 1;
	stbuf->st_size = entry->size;

	/* no cancellation point between acquisition of entry
	 * and this unlock function */
	hashtbl_unlock_element_byptr(entry);

	return 0;
}

typedef struct fuse_io_readdir_priv_s {
	fuse_fill_dir_t filler;
	void *buf;
} fuse_io_readdir_priv_t;

static int fuse_io_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
		off_t offset, struct fuse_file_info *fi, enum fuse_readdir_flags flags) {
	fuse_io_readdir_priv_t priv = {
		.filler = filler,
		.buf = buf
	};
	int rep(fuse_io_readdir_priv_t *priv, fuse_io_entry_t *entry) {
		priv->filler(priv->buf, entry->head.key, NULL, 0, 0);
		return 0;
	}
	if (strcmp(path, "/") != 0) return -ENOENT;

	filler(buf, ".", NULL, 0, 0);
	filler(buf, "..", NULL, 0, 0);

	hashtbl_ts_traverse(
			&((fuse_io_priv_t*)fuse_get_context()->private_data)->
			entries, (int (*)(void*, hashtbl_elt_t*))rep, &priv);

	return 0;
}

static int fuse_io_open(const char *path, struct fuse_file_info *fi) {
	fuse_io_entry_t *entry = hashtbl_find_element_bykey(
			&((fuse_io_priv_t*)fuse_get_context()->private_data)->
			entries, path + 1);
	if (!entry) return -ENOENT;

	//VERBOSE("attempt to open \"%s\"", path + 1);

	if (entry->inuse || entry->to_be_deleted) {
		hashtbl_unlock_element_byptr(entry);
		return -EBUSY;
	}

	/* enforce readonly */
	if (entry->readonly && ((fi->flags&O_ACCMODE) != O_RDONLY)) {
		hashtbl_unlock_element_byptr(entry);
		return -EACCES;
	}

	entry->inuse++;
	//VERBOSE("openend \"%s\"", path + 1);

	/* no cancellation point between acquisition of entry
	 * and this unlock function */
	hashtbl_unlock_element_byptr(entry);
	return 0;
}

static int fuse_io_release(const char *path, struct fuse_file_info *fi) {
	int delete = 0;
	fuse_io_entry_t *entry = hashtbl_find_element_bykey(
			&((fuse_io_priv_t*)fuse_get_context()->private_data)->
			entries, path + 1);
	if (!entry) return -ENOENT;

	assert(entry->inuse);
	/* we should do some kind of cleanup here */
	//VERBOSE("release called on %s", path);
	entry->inuse--;
	free(entry->mountpoint);
	entry->mountpoint = NULL;

	if (entry->close_on_release) {
		delete = 1;
		entry->to_be_deleted = 1;
	}

	/* no cancellation point between acquisition of entry
	 * and this unlock function */
	hashtbl_unlock_element_byptr(entry);

	if (delete) hashtbl_delete_element_byptr(
			&((fuse_io_priv_t*)fuse_get_context()->
				private_data)-> entries, entry);

	return 0;
}


static int fuse_io_read(const char *path, char *buf, size_t size, off_t offset,
		struct fuse_file_info *fi) {
	fuse_io_entry_t *entry = hashtbl_find_element_bykey(
			&((fuse_io_priv_t*)fuse_get_context()->private_data)->
			entries, path + 1);

	if (!entry) return -ENOENT;

	pthread_cleanup_push(hashtbl_unlock_element_byptr, entry);

	do_req(&entry->l, SCUBED3_READ, offset, size, (char*)buf);

	pthread_cleanup_pop(1);

	return size;
}

static int fuse_io_write(const char *path, const char *buf, size_t size,
		off_t offset, struct fuse_file_info *fi) {
	fuse_io_entry_t *entry = hashtbl_find_element_bykey(
			&((fuse_io_priv_t*)fuse_get_context()->private_data)->
			entries, path + 1);
	if (!entry) return -ENOENT;

	assert(!entry->readonly);

	pthread_cleanup_push(hashtbl_unlock_element_byptr, entry);

	do_req(&entry->l, SCUBED3_WRITE, offset, size, (char*)buf);

	pthread_cleanup_pop(1);

	return size;
}

void *fuse_io_init(struct fuse_conn_info *conn, struct fuse_config *config) {
	fuse_io_priv_t *priv = fuse_get_context()->private_data;

	/* we assume the mountpoint is the first entry in struct fuse_session
	 * THIS IS VERY VERY BAD, since "struct fuse_session" is an opaque
	 * struct, let's say it is fuse's fault to not make it available */
	priv->control_thread_priv.mountpoint =
			*((char**)fuse_get_session(fuse_get_context()->fuse));

	/* start control thread */
	priv->control_thread_priv.h = &priv->entries;
	priv->control_thread_priv.ids = &priv->ids;
	pthread_create(&priv->control_thread, NULL, control_thread,
			&priv->control_thread_priv);

	/* start paranoia level manager thread */
	pthread_create(&priv->plmgr_thread, NULL, plmgr_thread,
			&priv->plmgr_thread_priv);

	return fuse_get_context()->private_data;
}

void fuse_io_destroy(void *arg) {
	fuse_io_priv_t *priv = fuse_get_context()->private_data;
	VERBOSE("destroy called");

	/* stop paranoia level manager caand control thread */
	plmgr_thread_cancel_join_cleanup(priv->plmgr_thread,
			&priv->plmgr_thread_priv);
	control_thread_cancel_join_cleanup(priv->control_thread,
			&priv->control_thread_priv);
}

#if 0
static int fuse_io_fsync(const char *path, int datasync,
		struct fuse_file_info *fi) {
	VERBOSE("fsync on %s", path);
	return 0;
}

static int fuse_io_flush(const char *path, struct fuse_file_info *fi) {
	VERBOSE("flush on %s", path);
	return 0;
}
#endif

static struct fuse_operations fuse_io_operations = {
	.getattr = fuse_io_getattr,
	.readdir = fuse_io_readdir,
	.open = fuse_io_open,
	.read = fuse_io_read,
	.write = fuse_io_write,
	.release = fuse_io_release,
	.init = fuse_io_init,
	.destroy = fuse_io_destroy,
//	.fsync = fuse_io_fsync,
//	.flush = fuse_io_flush
};

static void freer(fuse_io_entry_t *entry) {
	pthd_cond_destroy(&entry->cond);
	free(entry->head.key);
	free(entry->mountpoint);
	scubed3_free(&entry->l);
	blockio_dev_free(&entry->d);
	cipher_free(&entry->c);
	if (entry->ids) {
		hashtbl_delete_element_byptr(entry->ids, &entry->unique_id);
		wipememory(entry->unique_id.id, 32);
	}
	free(entry);
}

int fuse_io_start(int argc, char *argv[], blockio_t *b) {
	int ret;
	fuse_io_priv_t priv = { }; /* initialize to zeroes */
	hashtbl_init_default(&priv.entries, -1, 4, 1, 1,
			(void (*)(void*))freer);
	hashtbl_init_default(&priv.ids, 32, 4, 1, 1, NULL);

	priv.control_thread_priv.b = b;
	priv.plmgr_thread_priv.b = b;
	b->plmgr = &priv.plmgr_thread_priv;

	ret = fuse_main(argc, argv, &fuse_io_operations, &priv);

	hashtbl_free(&priv.entries);
	hashtbl_free(&priv.ids);

	return ret;
}

