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
#define FUSE_USE_VERSION 26

#include <fuse.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include "verbose.h"
#include "fuse_io.h"
#include "hashtbl.h"
#include "util.h"
#include "blockio.h"
#include "pthd.h"
#include "control.h"
#include "fuse_io.h"

typedef struct fuse_io_priv_s {
	hashtbl_t entries;
	pthread_t control_thread;
	control_thread_priv_t control_thread_priv;
} fuse_io_priv_t;

static int fuse_io_getattr(const char *path, struct stat *stbuf) {
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

	stbuf->st_mode = S_IFREG|0600;
	stbuf->st_nlink = 1;
	stbuf->st_size = entry->size;

	hashtbl_unlock_element_byptr(entry);

	return 0;
}

typedef struct fuse_io_readdir_priv_s {
	fuse_fill_dir_t filler;
	void *buf;
} fuse_io_readdir_priv_t;

static int fuse_io_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
		off_t offset, struct fuse_file_info *fi) {
	fuse_io_readdir_priv_t priv = {
		.filler = filler,
		.buf = buf
	};
	int rep(fuse_io_readdir_priv_t *priv, fuse_io_entry_t *entry) {
		priv->filler(priv->buf, entry->name, NULL, 0);
		return 0;
	}
	if (strcmp(path, "/") != 0) return -ENOENT;

	filler(buf, ".", NULL, 0);
	filler(buf, "..", NULL, 0);

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

	if (entry->inuse || entry->to_be_deleted) {
		hashtbl_unlock_element_byptr(entry);
		return -EBUSY;
	}
	entry->inuse++;
	hashtbl_unlock_element_byptr(entry);
	return 0;
}

static int fuse_io_release(const char *path, struct fuse_file_info *fi) {
	fuse_io_entry_t *entry = hashtbl_find_element_bykey(
			&((fuse_io_priv_t*)fuse_get_context()->private_data)->
			entries, path + 1);
	if (!entry) return -ENOENT;

	assert(entry->inuse);
	/* we should do some kind of cleanup here */
	entry->inuse--;
	hashtbl_unlock_element_byptr(entry);
	return 0;
}


static int fuse_io_read(const char *path, char *buf, size_t size, off_t offset,
		struct fuse_file_info *fi) {
	fuse_io_entry_t *entry = hashtbl_find_element_bykey(
			&((fuse_io_priv_t*)fuse_get_context()->private_data)->
			entries, path + 1);

	if (!entry) return -ENOENT;

	do_req(&entry->l, SCUBED3_READ, offset, size, (char*)buf);

	hashtbl_unlock_element_byptr(entry);

	return size;
}

static int fuse_io_write(const char *path, const char *buf, size_t size,
		off_t offset, struct fuse_file_info *fi) {
	fuse_io_entry_t *entry = hashtbl_find_element_bykey(
			&((fuse_io_priv_t*)fuse_get_context()->private_data)->
			entries, path + 1);
	if (!entry) return -ENOENT;

	do_req(&entry->l, SCUBED3_WRITE, offset, size, (char*)buf);

	hashtbl_unlock_element_byptr(entry);

	return size;
}

void *fuse_io_init(struct fuse_conn_info *conn) {
	fuse_io_priv_t *priv = fuse_get_context()->private_data;
	priv->control_thread_priv.h = &priv->entries;
	priv->control_thread_priv.bla = (void*)3;
	pthread_create(&priv->control_thread, NULL, control_thread,
			&priv->control_thread_priv);
	//fuse_exit(fuse_get_context()->fuse);
	return fuse_get_context()->private_data;
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

void fuse_io_destroy(void *arg) {
	fuse_io_priv_t *priv = fuse_get_context()->private_data;
	VERBOSE("destroy called");
	pthread_cancel(priv->control_thread);
	pthread_join(priv->control_thread, NULL);
}

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
	free(entry->name);
	free(entry);
}

int fuse_io_start(int argc, char **argv, blockio_t *b) {
	int ret;
	fuse_io_priv_t priv;
	//fuse_io_entry_t *entry;
	hashtbl_init_default(&priv.entries, 4, -1, 1, 1,
			(void (*)(void*))freer);

	priv.control_thread_priv.b = b;
#if 0
	/* /test */
	entry = hashtbl_allocate_and_add_element(&priv.entries,
			estrdup("test"), sizeof(*entry));
	entry->size = ((l->dev->no_macroblocks-l->dev->reserved)*l->dev->mmpm)
		<<l->dev->mesoblk_log;
	entry->to_be_deleted = 0;
	entry->inuse = 0;
	entry->l = l;
	hashtbl_unlock_element_byptr(entry);
#endif

	ret = fuse_main(argc, argv, &fuse_io_operations, &priv);

	hashtbl_free(&priv.entries);

	return ret;
}

