/* keystore.c - a deniable store of the keys of partitions (for scubed3ctl)
 *
 * Copyright (C) 2026 Rik Snel <rik@snel.it>
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
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <assert.h>
#include <libgen.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/file.h>
#include "verbose.h"
#include "util.h"
#include "gcry.h"
#include "cipher.h"
#include "keystore.h"

_Static_assert(sizeof(ks_entry_t) == 128, "an entry is 128 bytes");

/* random data from getrandom(2) */
void ks_random(void *buf, size_t len) {
	ssize_t n;

	while (len) {
		if ((n = getrandom(buf, len, 0)) < 0) {
			if (errno == EINTR) continue;
			FATAL("getrandom: %s", strerror(errno));
		}
		buf = (char*)buf + n;
		len -= n;
	}
}

/* the additional data of slot i: the IV and the tag belong to this slot */
static void ks_aad(gcry_cipher_hd_t hd, int i) {
	unsigned char aad[20] = "scubed3 keys v1";

	aad[16] = i>>24; aad[17] = i>>16; aad[18] = i>>8; aad[19] = i;
	gcry_call(cipher_authenticate, hd, aad, sizeof(aad));
}

static gcry_cipher_hd_t ks_cipher(const unsigned char *key,
		const unsigned char *iv, int i) {
	gcry_cipher_hd_t hd;

	gcry_call(cipher_open, &hd, GCRY_CIPHER_AES256, GCRY_CIPHER_MODE_GCM,
			GCRY_CIPHER_SECURE);
	gcry_call(cipher_setkey, hd, key, 32);
	gcry_call(cipher_setiv, hd, iv, KS_IV);
	ks_aad(hd, i);

	return hd;
}

/* decrypt slot i with key into e (KS_ENTRIES entries), 0 if the tag is
 * right (the slot is a view of this key), -1 if not */
static int ks_decrypt(keystore_t *k, int i, const unsigned char *key,
		ks_entry_t *e) {
	unsigned char *slot = k->data + (size_t)i*KS_SLOT;
	gcry_cipher_hd_t hd = ks_cipher(key, slot, i);
	int ret;

	gcry_call(cipher_decrypt, hd, e, KS_PLAIN, slot + KS_IV, KS_PLAIN);
	ret = gcry_cipher_checktag(hd, slot + KS_IV + KS_PLAIN, KS_TAG)?-1:0;
	gcry_cipher_close(hd);
	if (ret) wipememory(e, KS_PLAIN);

	return ret;
}

static void ks_encrypt(ks_view_t *v, unsigned char *slot) {
	gcry_cipher_hd_t hd;

	ks_random(slot, KS_IV);
	hd = ks_cipher(v->key, slot, v->slot);
	gcry_call(cipher_encrypt, hd, slot + KS_IV, KS_PLAIN, v->e, KS_PLAIN);
	gcry_call(cipher_gettag, hd, slot + KS_IV + KS_PLAIN, KS_TAG);
	gcry_cipher_close(hd);
}

/* write data (KS_SIZE bytes) to path: to a new file next to it, that
 * replaces it (a crash leaves the old or the new store) */
/* write data to a new file next to path (its name has the pid: two
 * processes never write the same file), then replace path with it (create:
 * only if path does not exist, EEXIST otherwise: link fails if it exists,
 * also when another process makes it at the same moment); a crash leaves
 * the old or the new key store */
static int ks_replace(const char *path, const unsigned char *data,
		int create) {
	char *tmp, *dir, *d;
	int fd, ret = -1;
	ssize_t n;
	size_t off = 0;

	if (asprintf(&tmp, "%s.new.%ld", path, (long)getpid()) == -1)
		FATAL("out of memory");
	unlink(tmp);
	if ((fd = open(tmp, O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC, 0600)) == -1)
		goto out;
	while (off < KS_SIZE) {
		if ((n = write(fd, data + off, KS_SIZE - off)) < 0) {
			if (errno == EINTR) continue;
			close(fd);
			goto out_unlink;
		}
		off += n;
	}
	if (fsync(fd) || close(fd)) goto out_unlink;
	if (create) {
		if (link(tmp, path)) goto out_unlink;
		unlink(tmp);
	} else if (rename(tmp, path)) goto out_unlink;

	/* the rename must be on disk too */
	d = estrdup(path);
	dir = dirname(d);
	if ((fd = open(dir, O_RDONLY|O_DIRECTORY)) != -1) {
		fsync(fd);
		close(fd);
	}
	free(d);
	ret = 0;
	goto out;
out_unlink:
	n = errno;
	unlink(tmp);
	errno = n;
out:
	free(tmp);
	return ret;
}

int ks_create(const char *path) {
	unsigned char *data;
	int ret;

	data = ecalloc(1, KS_SIZE);
	ks_random(data, KS_SIZE);
	ret = ks_replace(path, data, 1);
	free(data);

	return ret;
}

/* read the key store from fd into buf, 0 or -1 (errno) */
static int ks_read_fd(int fd, unsigned char *buf) {
	ssize_t n;
	size_t off = 0;
	struct stat st;

	if (fstat(fd, &st)) return -1;
	if (st.st_size != KS_SIZE) {
		errno = EINVAL;
		return -1;
	}
	while (off < KS_SIZE) {
		if ((n = pread(fd, buf + off, KS_SIZE - off, off)) <= 0) {
			if (n < 0 && errno == EINTR) continue;
			if (!n) errno = EIO;
			return -1;
		}
		off += n;
	}

	return 0;
}

/* lock the key store against other writers (flock of the file at path, the
 * file that is there when the lock is taken: a writer that waited for it
 * may find another file, the key store of the writer before it), the fd,
 * -1 (errno) */
static int ks_lock(const char *path) {
	struct stat a, b;
	int fd;

	for (;;) {
		if ((fd = open(path, O_RDONLY|O_CLOEXEC)) == -1) return -1;
		if (flock(fd, LOCK_EX)) {
			close(fd);
			return -1;
		}
		if (!fstat(fd, &a) && !stat(path, &b) && a.st_dev == b.st_dev &&
				a.st_ino == b.st_ino) return fd;
		close(fd);
	}
}

int ks_read(keystore_t *k) {
	int fd, n;

	if (k->data) return 0;
	if ((fd = open(k->path, O_RDONLY|O_CLOEXEC)) == -1) return -1;
	k->data = ecalloc(1, KS_SIZE);
	if (ks_read_fd(fd, k->data)) {
		n = errno;
		free(k->data);
		k->data = NULL;
		close(fd);
		errno = n;
		return -1;
	}
	close(fd);

	return 0;
}

int ks_unlock(keystore_t *k, const unsigned char *key) {
	ks_view_t *v;
	int i;

	assert(k->data);
	for (i = 0; i < k->no_views; i++)
		if (!memcmp(k->views[i]->key, key, 32)) return i;

	v = ecalloc(1, sizeof(*v));
	for (i = 0; i < KS_SLOTS; i++) if (!ks_decrypt(k, i, key, v->e)) {
		v->slot = i;
		memcpy(v->key, key, 32);
		k->views[k->no_views] = v;
		return k->no_views++;
	}
	wipememory(v, sizeof(*v));
	free(v);

	return -1;
}

int ks_add_view(keystore_t *k, const unsigned char *key) {
	int free_slots[KS_SLOTS], no_free = 0, i, j;
	uint32_t r;
	ks_view_t *v;

	for (i = 0; i < KS_SLOTS; i++) {
		for (j = 0; j < k->no_views; j++)
			if (k->views[j]->slot == i) break;
		if (j == k->no_views) free_slots[no_free++] = i;
	}
	if (!no_free) return -1;

	ks_random(&r, sizeof(r));
	v = ecalloc(1, sizeof(*v));
	v->slot = free_slots[r%no_free];
	memcpy(v->key, key, 32);
	k->views[k->no_views] = v;

	return k->no_views++;
}

void ks_remove_view(keystore_t *k, int view) {
	assert(view >= 0 && view < k->no_views);
	wipememory(k->views[view], sizeof(ks_view_t));
	free(k->views[view]);
	memmove(&k->views[view], &k->views[view + 1],
			(k->no_views - view - 1)*sizeof(ks_view_t*));
	k->no_views--;
}

ks_entry_t *ks_find(keystore_t *k, int view, const char *name, int *found) {
	int i, v;

	for (v = (view < 0)?0:view; v < k->no_views; v++) {
		for (i = 0; i < KS_ENTRIES; i++) {
			ks_entry_t *e = &k->views[v]->e[i];
			if ((e->flags&KS_USED) && !strcmp(e->name, name)) {
				if (found) *found = v;
				return e;
			}
		}
		if (view >= 0) break;
	}

	return NULL;
}

/* the same partition (and the same LUKS key) */
static int ks_same(const ks_entry_t *a, const ks_entry_t *b) {
	if (memcmp(a->key, b->key, KS_KEY)) return 0;
	if ((a->flags&KS_HAS_LUKS) != (b->flags&KS_HAS_LUKS)) return 0;
	if ((a->flags&KS_HAS_LUKS) && memcmp(a->luks, b->luks, KS_LUKS))
		return 0;
	return 1;
}

int ks_conflict(keystore_t *k, int view, const char *name) {
	ks_entry_t *a, *b;
	int v;

	if (!(a = ks_find(k, view, name, NULL))) return -1;
	for (v = 0; v < k->no_views; v++) {
		if (v == view || !(b = ks_find(k, v, name, NULL))) continue;
		if (!ks_same(a, b)) return v;
	}

	return -1;
}

ks_entry_t *ks_new_entry(keystore_t *k, int view, const char *name) {
	int i;

	assert(view >= 0 && view < k->no_views && strlen(name) < KS_NAME);
	for (i = 0; i < KS_ENTRIES; i++) {
		ks_entry_t *e = &k->views[view]->e[i];
		if (e->flags&KS_USED) continue;
		memset(e, 0, sizeof(*e));
		e->flags = KS_USED;
		strcpy(e->name, name);
		return e;
	}

	return NULL;
}

int ks_write(keystore_t *k) {
	unsigned char *data, *now;
	int i, j, fd, n;

	/* another scubed3ctl may have written the key store since we read it
	 * (a view it added, or changed, is not unlocked here: our write would
	 * lose it), with the lock nobody writes between the check and our
	 * write */
	assert(k->data);
	if ((fd = ks_lock(k->path)) == -1) return -1;
	now = ecalloc(1, KS_SIZE);
	if (ks_read_fd(fd, now)) n = errno;
	else n = memcmp(now, k->data, KS_SIZE)?ESTALE:0;
	free(now);
	if (n) {
		close(fd);
		errno = n;
		return -1;
	}

	data = ecalloc(1, KS_SIZE);
	for (i = 0; i < KS_SLOTS; i++) {
		unsigned char *slot = data + (size_t)i*KS_SLOT;
		for (j = 0; j < k->no_views; j++)
			if (k->views[j]->slot == i) break;
		if (j < k->no_views) ks_encrypt(k->views[j], slot);
		else ks_random(slot, KS_SLOT);
	}
	if (ks_replace(k->path, data, 0)) {
		n = errno;
		free(data);
		close(fd);
		errno = n;
		return -1;
	}
	close(fd);
	free(k->data);
	k->data = data;

	return 0;
}

void ks_free(keystore_t *k) {
	while (k->no_views) ks_remove_view(k, k->no_views - 1);
	free(k->data);
	k->data = NULL;
}
