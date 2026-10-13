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
/* the API of libfuse 3.1, it works with every version of libfuse 3 */
#define FUSE_USE_VERSION 31

#include <fuse.h>
#include <fuse_lowlevel.h> /* fuse_parse_cmdline */
#include <stddef.h>
#include <string.h>
#include <unistd.h>
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
	hashtbl_t entries, ids;
	pthread_t control_thread;
	control_thread_priv_t control_thread_priv;
	int direct_io;
} fuse_io_priv_t;

/* everything that may write blocks takes the io lock of the base device
 * before the lock of the partition (see blockio_t) */
static void io_lock(void) {
	blockio_io_lock(((fuse_io_priv_t*)fuse_get_context()->private_data)->
			control_thread_priv.b);
}

static void io_unlock(void) {
	blockio_io_unlock(((fuse_io_priv_t*)fuse_get_context()->private_data)->
			control_thread_priv.b);
}

/* paranoia level 2: the scheduler lets partition dev write one block (the
 * caller holds the io lock, and the lock of another partition) */
static void fuse_io_step(blockio_dev_t *dev) {
	fuse_io_entry_t *entry = (fuse_io_entry_t*)
		((char*)dev - offsetof(fuse_io_entry_t, d));

	pthd_mutex_lock(&entry->head.data_mutex);
	scubed3_cycle(&entry->l);
	pthd_mutex_unlock(&entry->head.data_mutex);
}

static int fuse_io_getattr(const char *path, struct stat *stbuf,
		struct fuse_file_info *fi) {
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
		off_t offset, struct fuse_file_info *fi,
		enum fuse_readdir_flags flags) {
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

	/* any number of openers is allowed (resize2fs opens the partition
	 * twice), requests are serialized by the lock on the entry */
	if (entry->to_be_deleted) {
		hashtbl_unlock_element_byptr(entry);
		return -EBUSY;
	}
	entry->inuse++;
	//VERBOSE("openend \"%s\"", path + 1);

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

	/* the last opener is gone: it is not mounted anymore */
	if (!entry->inuse) {
		free(entry->mountpoint);
		entry->mountpoint = NULL;
	}

	if (!entry->inuse && entry->close_on_release) {
		delete = 1;
		entry->to_be_deleted = 1;
	}
	hashtbl_unlock_element_byptr(entry);

	/* closing the partition writes its current block */
	if (delete) {
		io_lock();
		hashtbl_delete_element_byptr(
				&((fuse_io_priv_t*)fuse_get_context()->
					private_data)-> entries, entry);
		io_unlock();
	}

	return 0;
}


static int fuse_io_read(const char *path, char *buf, size_t size, off_t offset,
		struct fuse_file_info *fi) {
	int ret;
	fuse_io_entry_t *entry = hashtbl_find_element_bykey(
			&((fuse_io_priv_t*)fuse_get_context()->private_data)->
			entries, path + 1);

	if (!entry) return -ENOENT;

	ret = do_req(&entry->l, SCUBED3_READ, offset, size, (char*)buf);

	hashtbl_unlock_element_byptr(entry);

	return ret;
}

static int fuse_io_write(const char *path, const char *buf, size_t size,
		off_t offset, struct fuse_file_info *fi) {
	int ret;
	fuse_io_entry_t *entry;

	io_lock();
	entry = hashtbl_find_element_bykey(
			&((fuse_io_priv_t*)fuse_get_context()->private_data)->
			entries, path + 1);
	if (!entry) {
		io_unlock();
		return -ENOENT;
	}

	ret = do_req(&entry->l, SCUBED3_WRITE, offset, size, (char*)buf);

	hashtbl_unlock_element_byptr(entry);
	io_unlock();

	return ret;
}

void *fuse_io_init(struct fuse_conn_info *conn, struct fuse_config *cfg) {
	fuse_io_priv_t *priv = fuse_get_context()->private_data;

	/* -o direct_io (libfuse 2 had it as a mount option) */
	cfg->direct_io = priv->direct_io;

	/* start control thread */
	priv->control_thread_priv.h = &priv->entries;
	priv->control_thread_priv.ids = &priv->ids;
	pthread_create(&priv->control_thread, NULL, control_thread,
			&priv->control_thread_priv);

	/* start the clock of paranoia level 3 (it is idle in other levels) */
	blockio_clock_start(priv->control_thread_priv.b);

	return fuse_get_context()->private_data;
}

static int fuse_io_fsync(const char *path, int datasync,
		struct fuse_file_info *fi) {
	fuse_io_entry_t *entry;

	io_lock();
	entry = hashtbl_find_element_bykey(
			&((fuse_io_priv_t*)fuse_get_context()->private_data)->
			entries, path + 1);
	if (!entry) {
		io_unlock();
		return -ENOENT;
	}

	scubed3_fsync(&entry->l);

	hashtbl_unlock_element_byptr(entry);
	io_unlock();

	return 0;
}

/* the size of a partition is changed with resize, truncate behaves as on a
 * block device: open(O_TRUNC), truncate() and ftruncate() succeed, but
 * nothing changes (dd without conv=notrunc truncates its output file, an
 * accidental "> partition" in a shell does not destroy anything) */
static int fuse_io_truncate(const char *path, off_t size,
		struct fuse_file_info *fi) {
	fuse_io_entry_t *entry = hashtbl_find_element_bykey(
			&((fuse_io_priv_t*)fuse_get_context()->private_data)->
			entries, path + 1);
	if (!entry) return -ENOENT;

	DEBUG("truncate of \"%s\" to %ld bytes ignored", path + 1,
			(long)size);

	hashtbl_unlock_element_byptr(entry);

	return 0;
}

/* discard: the loop driver turns discard requests (fstrim, mount -o
 * discard) and write zeroes requests into fallocate(PUNCH_HOLE) and
 * fallocate(ZERO_RANGE) on the partition; both make the range read as
 * zeroes, whole mesoblocks are discarded (see scubed3_discard); allocating
 * is possible within the partition (nothing to do), not past its end */
static int fuse_io_fallocate(const char *path, int mode, off_t offset,
		off_t len, struct fuse_file_info *fi) {
	int ret = 0;
	fuse_io_entry_t *entry;

	io_lock();
	entry = hashtbl_find_element_bykey(
			&((fuse_io_priv_t*)fuse_get_context()->private_data)->
			entries, path + 1);
	if (!entry) {
		io_unlock();
		return -ENOENT;
	}

	if (offset < 0 || len <= 0) ret = -EINVAL;
	else if (mode&~(FALLOC_FL_KEEP_SIZE|FALLOC_FL_PUNCH_HOLE|
				FALLOC_FL_ZERO_RANGE)) ret = -EOPNOTSUPP;
	else if (mode&(FALLOC_FL_PUNCH_HOLE|FALLOC_FL_ZERO_RANGE))
		ret = scubed3_discard(&entry->l, offset, len);
	else if (offset + len > entry->size) ret = -ENOSPC;

	hashtbl_unlock_element_byptr(entry);
	io_unlock();

	return ret;
}

#if 0

static int fuse_io_flush(const char *path, struct fuse_file_info *fi) {
	VERBOSE("flush on %s", path);
	return 0;
}
#endif

void fuse_io_destroy(void *arg) {
	fuse_io_priv_t *priv = fuse_get_context()->private_data;
	VERBOSE("destroy called");
	blockio_clock_stop(priv->control_thread_priv.b);
	pthread_cancel(priv->control_thread);
	pthread_join(priv->control_thread, NULL);
	hashtbl_free(&priv->control_thread_priv.c);
	close(priv->control_thread_priv.listen_fd);
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
	.fsync = fuse_io_fsync,
	.truncate = fuse_io_truncate,
	.fallocate = fuse_io_fallocate,
//	.flush = fuse_io_flush
};

static void freer(fuse_io_entry_t *entry) {
	/* the partition is closed, its blocks would look free (see
	 * blockio_dev_free), the level is lowered first, so that its last
	 * writes do not wait for their turn (a partition without blocks,
	 * for example after an open with the wrong passphrase, does not
	 * matter) */
	if (entry->d.b && entry->d.b->paranoia >= 2 &&
			entry->d.no_macroblocks) {
		WARNING("\"%s\" is closed, paranoia level %d is switched off "
				"(level 1)", entry->head.key, entry->d.b->paranoia);
		blockio_set_paranoia(entry->d.b, 1, 0);
		blockio_finish_wipes(entry->d.b);
	}
	free(entry->head.key);
	free(entry->mountpoint);
	scubed3_free(&entry->l);
	/* an open that failed early (key not valid or already in use) did
	 * not get to blockio_dev_init */
	if (entry->d.b) blockio_dev_free(&entry->d);
	cipher_free(&entry->c);
	if (entry->ids) {
		hashtbl_delete_element_byptr(entry->ids, &entry->unique_id);
		wipememory(entry->unique_id.id, 32);
	}
	free(entry);
}

/* fuse_main, but we want access to the mountpoint (it is passed to the
 * control thread) */
static int fuse_main_custom(int argc, char *argv[],
		const struct fuse_operations *op,
		size_t op_size, void *user_data) {
	fuse_io_priv_t *priv = user_data;
	struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
	struct fuse_cmdline_opts opts;
	struct fuse_session *se;
	struct fuse *fuse;
	int res = 1;

	if (fuse_parse_cmdline(&args, &opts)) return 1;

	if (opts.show_version) {
		printf("FUSE library version %s\n", fuse_pkgversion());
		res = 0;
		goto out_args;
	}

	if (opts.show_help) {
		printf("usage: %s -b BASE [options] MOUNTPOINT\n\n", argv[0]);
		fuse_cmdline_help();
		fuse_lib_help(&args);
		res = 0;
		goto out_args;
	}

	if (!opts.mountpoint) {
		ERROR("no mountpoint given");
		goto out_args;
	}

	if (*opts.mountpoint != '/') {
		ERROR("the mountpoint must start with a /");
		goto out_args;
	}
	priv->control_thread_priv.mountpoint = opts.mountpoint;

	if (!(fuse = fuse_new(&args, op, op_size, priv))) goto out_args;

	if (fuse_mount(fuse, opts.mountpoint)) goto out_destroy;

	if (fuse_daemonize(opts.foreground)) goto out_unmount;

	se = fuse_get_session(fuse);
	if (fuse_set_signal_handlers(se)) goto out_unmount;

	res = opts.singlethread?fuse_loop(fuse):
		fuse_loop_mt(fuse, opts.clone_fd);
	res = res?1:0;

	fuse_remove_signal_handlers(se);
out_unmount:
	fuse_unmount(fuse);
out_destroy:
	fuse_destroy(fuse);
out_args:
	free(opts.mountpoint);
	fuse_opt_free_args(&args);

	return res;
}

int fuse_io_start(int argc, char **argv, blockio_t *b,
		const char *control_socket, int direct_io) {
	int ret;
	fuse_io_priv_t priv;
	hashtbl_init_default(&priv.entries, -1, 4, 1, 1,
			(void (*)(void*))freer);
	hashtbl_init_default(&priv.ids, 32, 4, 1, 1, NULL);

	priv.control_thread_priv.b = b;
	priv.direct_io = direct_io;
	b->step = fuse_io_step;

	/* create the control socket before mounting, so that errors
	 * are reported before scubed3 goes to the background */
	priv.control_thread_priv.listen_fd = control_listen(control_socket);

	ret = fuse_main_custom(argc, argv, &fuse_io_operations,
			sizeof(fuse_io_operations), &priv);

	unlink(control_socket);

	hashtbl_free(&priv.entries);

	return ret;
}

