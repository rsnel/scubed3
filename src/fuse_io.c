#define FUSE_USE_VERSION 25

#include <fuse.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include "verbose.h"
#include "fuse_io.h"
#include "hashtbl.h"
#include "util.h"
#include "blockio.h"

typedef struct fuse_io_entry_s {
	hashtbl_elt_t head;
	char *name;

	uint64_t size;
	int inuse;
	scubed3_t *l;
} fuse_io_entry_t;

/* in Debian Etch fuse 26 is not yet available, we are not yet
 * able to pass a pointer to this struct as private data */
static hashtbl_t fuse_io_entries;

static int fuse_io_getattr(const char *path, struct stat *stbuf) {
	fuse_io_entry_t *entry;
	assert(path && *path == '/');

	memset(stbuf, 0, sizeof(*stbuf));

	if (!strcmp(path, "/")) {
		stbuf->st_mode = S_IFDIR|0755;
		stbuf->st_nlink = 2;
		return 0;
	}

	entry = hashtbl_find_element_bykey(&fuse_io_entries, path + 1);
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
	void rep(fuse_io_readdir_priv_t *priv, fuse_io_entry_t *entry) {
		priv->filler(priv->buf, entry->name, NULL, 0);
	}
	if (strcmp(path, "/") != 0) return -ENOENT;

	filler(buf, ".", NULL, 0); filler(buf, "..", NULL, 0);

	hashtbl_ts_traverse(&fuse_io_entries,
			(void (*)(void*, hashtbl_elt_t*))rep, &priv);

	return 0;
}

static int fuse_io_open(const char *path, struct fuse_file_info *fi) {
	fuse_io_entry_t *entry =
		hashtbl_find_element_bykey(&fuse_io_entries, path + 1);
	if (!entry) return -ENOENT;
	if (entry->inuse) {
		hashtbl_unlock_element_byptr(entry);
		return -EBUSY;
	}
	entry->inuse++;
	hashtbl_unlock_element_byptr(entry);
	return 0;
}

static int fuse_io_release(const char *path, struct fuse_file_info *fi) {
	fuse_io_entry_t *entry =
		hashtbl_find_element_bykey(&fuse_io_entries, path + 1);
	if (!entry) return -ENOENT;
	assert(entry->inuse);
	/* we should do some kind of cleanup here */
	entry->inuse--;
	hashtbl_unlock_element_byptr(entry);
	return 0;
}


static int fuse_io_read(const char *path, char *buf, size_t size, off_t offset,
		struct fuse_file_info *fi) {
	fuse_io_entry_t *entry =
		hashtbl_find_element_bykey(&fuse_io_entries, path + 1);
	if (!entry) return -ENOENT;

	do_req(entry->l, SCUBED3_READ, offset, size, (char*)buf);

	hashtbl_unlock_element_byptr(entry);

	return size;
}

static int fuse_io_write(const char *path, const char *buf, size_t size,
		off_t offset, struct fuse_file_info *fi) {
	fuse_io_entry_t *entry =
		hashtbl_find_element_bykey(&fuse_io_entries, path + 1);
	if (!entry) return -ENOENT;

	do_req(entry->l, SCUBED3_WRITE, offset, size, (char*)buf);

	hashtbl_unlock_element_byptr(entry);

	return size;
}

static struct fuse_operations fuse_io_operations = {
	.getattr = fuse_io_getattr,
	.readdir = fuse_io_readdir,
	.open = fuse_io_open,
	.read = fuse_io_read,
	.write = fuse_io_write,
	.release = fuse_io_release,
};

static void freer(fuse_io_entry_t *entry) {
	free(entry->name);
	free(entry);
}

int fuse_io_start(int argc, char **argv, scubed3_t *l) {
	int ret;
	fuse_io_entry_t *entry;
	hashtbl_init_default(&fuse_io_entries, 4, -1, 1, 1,
			(void (*)(void*))freer);

	/* /test */
	entry = hashtbl_allocate_and_add_element(&fuse_io_entries,
			"test", sizeof(*entry));
	entry->name = estrdup("test");
	entry->size = ((l->dev->no_macroblocks-l->dev->reserved)*l->dev->mmpm)
		<<l->dev->mesoblk_log;
	entry->inuse = 0;
	entry->l = l;
	hashtbl_unlock_element_byptr(entry);

	ret = fuse_main(argc, argv, &fuse_io_operations);

	hashtbl_free(&fuse_io_entries);

	return ret;
}

