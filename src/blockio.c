/* blockio.c - handles block input/output
 *
 * Copyright (C) 2019  Rik Snel <rik@snel.it>
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
#include <sys/types.h>
#include <sys/stat.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <fcntl.h>
#include <limits.h>
#include <assert.h>
#include "blockio.h"
#include "bitmap.h"
#include "verbose.h"
#include "binio.h"
#include "dllarr.h"
#include "util.h"
#include "gcry.h"
#include "ecch.h"

/* stream (FILE*) stuff */

static void stream_io(void *fp, void *buf, uint64_t offset, uint32_t size,
		size_t (*io)(), const char *rwing) {
	assert(fp && ((!buf && !size) || (buf && size)) && io);

	if (fseeko(fp, offset, SEEK_SET))
		FATAL("error seeking to byte %lu: %s",
				offset, strerror(errno));

	if (io(buf, size, 1, fp) != 1)
		FATAL("error %s %u bytes: %s", rwing, size,
				strerror(errno));
}

static void stream_read(void *fp, void *buf, uint64_t offset, uint32_t size) {
	stream_io(fp, buf, offset, size, fread, "reading");
}

static void stream_write(void *fp, const void *buf, uint64_t offset,
		uint32_t size) {
	stream_io(fp, (void*)buf, offset, size, fwrite, "writing");
}

static void stream_close(void *fp) {
	fclose(fp);
}

static void *stream_open(const char *path) {
	void *ret;
	if (!(ret = fopen(path, "r+")))
		ecch_throw(ECCH_DEFAULT, "fopening %s: %s",
				path, strerror(errno));

	return ret;
}

/* end stream stuff */

#define BASE			(dev->tmp_macroblock)
#define INDEXBLOCK_SHA256	(BASE + 0x000)
#define DATABLOCKS_SHA256	(BASE + 0x020)
#define SEQNOS_SHA256		(BASE + 0x040)
#define SEQNO_UINT64		(BASE + 0x060)
#define NEXT_SEQNO_UINT64	(BASE + 0x068)
#define MAGIC64			(BASE + 0x070)
#define NO_MACROBLOCKS_UINT32	(BASE + 0x078)
#define RESERVED_BLOCKS_UINT32	(BASE + 0x07C)
#define RESERVED_SPACE1024	(BASE + 0x080)
#define NO_INDICES_UINT32	(BASE + 0x100)
#define BITMAP			(BASE + dev->b->bitmap_offset)

void blockio_free(blockio_t *b) {
	assert(b);
	free(b->open_priv);
	dllarr_free(&b->unallocated);
	free(b->blockio_infos);
	pthd_mutex_destroy(&b->unallocated_mutex);
}

uint32_t blockio_get_macroblock_index(blockio_info_t *bi) {
	assert(bi->dev);
	return bi - bi->dev->b->blockio_infos;
}

/* open backing file and set macroblock size */
void blockio_init_file(blockio_t *b, const char *path, uint8_t macroblock_log,
		uint8_t mesoblk_log) {
	struct stat stat_info;
	struct flock lock;
	uint64_t tmp;
	void *priv; /* FILE* */
	assert(b);
	assert(sizeof(off_t)==8);
	assert(macroblock_log < 8*sizeof(uint32_t));

	b->open_priv = strdup(path);
	if (!b->open_priv) FATAL("out of memory");
	b->macroblock_log = macroblock_log;
	b->macroblock_size = 1<<macroblock_log;
	b->mesoblk_log = mesoblk_log;
	b->mmpm = (1<<(b->macroblock_log - b->mesoblk_log)) - 1;

        VERBOSE("mesoblock size %d bytes, macroblock size %d bytes",
			1<<b->mesoblk_log, b->macroblock_size);

	VERBOSE("%ld mesoblocks per macroblock, including index",
			1L<<(macroblock_log - mesoblk_log));

        b->bitmap_offset = 256 + (4<<(macroblock_log - mesoblk_log));
	if (b->bitmap_offset >= 1<<mesoblk_log)
		FATAL("not enough room for indexblock in mesoblock");
        VERBOSE("required minimumsize of indexblock excluding macroblock "
			"index is %d bytes", b->bitmap_offset);

        b->max_macroblocks = ((1L<<mesoblk_log) - b->bitmap_offset)<<3;
        VERBOSE("maximum amount of macroblocks supported %d",
			b->max_macroblocks);

	b->open = (void* (*)(const void*))stream_open;
	b->read = stream_read;
	b->write = stream_write;
	b->close = stream_close;

	/* each scubed device has it's own handle
	 * to the file (for thead safity), we open the
	 * file here temporarily to look at it */
	priv = stream_open(b->open_priv);

	lock.l_type = F_WRLCK;
	lock.l_whence = SEEK_SET;
	lock.l_start = 0;
	lock.l_len = 0;  /* whole file */

	if (fcntl(fileno(priv), F_SETLK, &lock) == -1) {
		if (fcntl(fileno(priv), F_GETLK, &lock) == -1) assert(0);

		FATAL("process with PID %d has already locked %s",
				lock.l_pid, path);
	}

	if (stat(path, &stat_info) < 0)
		FATAL("unable to 'stat' file %s: %s", path, strerror(errno));

	/* we support using a regular file or a block device as backend */
	if (S_ISREG(stat_info.st_mode)) {
		tmp = stat_info.st_size;
		DEBUG("%s is a regular file", path);

		b->total_macroblocks = stat_info.st_size>>b->macroblock_log;
	} else if (S_ISBLK(stat_info.st_mode)) {
		DEBUG("%s is a block device", path);
		if (ioctl(fileno(priv), BLKGETSIZE64, &tmp))
			FATAL("error querying size of blockdevice %s", path);

	} else FATAL("%s is not a regular file or a block device", path);

	DEBUG("backing size in bytes %ld having %ld macroblocks of size %u",
			tmp, tmp>>b->macroblock_log, b->macroblock_size);

	/* check if the device or file is not too large */
	tmp >>= b->macroblock_log;
	if (tmp > b->max_macroblocks) FATAL("device is too large");
	b->total_macroblocks = tmp;

	b->blockio_infos = ecalloc(sizeof(blockio_info_t),
			b->total_macroblocks);

	dllarr_init(&b->unallocated, offsetof(blockio_info_t, ur));
	for (uint32_t i = 0; i < b->total_macroblocks; i++) 
		dllarr_append(&b->unallocated, &b->blockio_infos[i]);

	stream_close(priv);
	pthd_mutex_init(&b->unallocated_mutex);
}

static const char magic[8] = "SSS3v0.1";

void blockio_dev_free(blockio_dev_t *dev) {
	int i;
	assert(dev);
	VERBOSE("closing \"%s\", %s", dev->name,
			dev->updated?"SHOULD BE WRITTEN":"no updates");
	if (dev->updated) blockio_dev_write_current_macroblock(dev);
	random_free(&dev->r);
	bitmap_free(&dev->status);

	blockio_info_t *bi;

	/* going to modify bi->dev pointer and the unallocated list */
	pthd_mutex_lock(&dev->b->unallocated_mutex);

	for (i = 0; i < dev->b->total_macroblocks; i++) {
		bi = &dev->b->blockio_infos[i];
		if (bi->dev == dev) {
			free(bi->indices);
			bi->dev = NULL;
		}
	}

	/* re-add blocks to unallocated */
	juggler_free_and_empty_into(&dev->j, dllarr_append,
			&dev->b->unallocated);

	
	/* also add blocks that are still in the replay
	 * list for whatever reason (creating an existing
	 * device for example) */
	while ((bi = dllarr_last(&dev->replay))) {
		dllarr_remove(&dev->replay, bi);
		dllarr_append(&dev->b->unallocated, bi);
	}

	pthd_mutex_unlock(&dev->b->unallocated_mutex);

	dllarr_free(&dev->replay);
	free(dev->tmp_macroblock);
	free(dev->name);
	if (dev->b && dev->b->close) dev->b->close(dev->io);
}

void blockio_dev_select_next_macroblock(blockio_dev_t *dev) {
	assert(!dev->bi);

	dev->updated = 0;

	dev->bi = juggler_get_devblock(&dev->j, 0);
	dev->tail_macroblock = juggler_get_obsoleted(&dev->j);

	assert(dev->bi);

	VERBOSE("new block %d seqno=%ld next_seqno=%ld",
			blockio_get_macroblock_index(dev->bi),
			dev->bi->seqno, dev->bi->next_seqno);
}

void blockio_dev_write_current_and_select_next_macroblock(
		blockio_dev_t *dev) {
	blockio_dev_write_current_macroblock(dev);
	blockio_dev_select_next_macroblock(dev);
}

void blockio_dev_init(blockio_dev_t *dev, blockio_t *b, cipher_t *c,
		const char *name) {
	int i; 
	uint64_t seqno = 0;

	assert(b && c);
	assert(b->mesoblk_log < b->macroblock_log);
	random_init(&dev->r);
	dllarr_init(&dev->replay, offsetof(blockio_info_t, ur));
	bitmap_init(&dev->status, b->max_macroblocks);
	juggler_init(&dev->j, &dev->r);

	dev->b = b;
	dev->c = c;
	assert(bitmap_size(&dev->status) + 260 + (dev->b->mmpm<<2) ==
			1<<dev->b->mesoblk_log);

	assert(b->open);
	dev->io = (b->open)(b->open_priv);

	dev->tmp_macroblock = ecalloc(1, 1<<b->macroblock_log);

	/* read macroblock headers, protected by mutex,
	 * because we will maybe touch blocks that are owned
	 * by other devices and the also touch the list
	 * of unallocated blocks */
	pthd_mutex_lock(&b->unallocated_mutex);

	for (i = 0; i < b->total_macroblocks; i++)
		blockio_dev_scan_header(&dev->replay, dev,
				dev->b->blockio_infos + i, &seqno);

	pthd_mutex_unlock(&b->unallocated_mutex);

	if (!dllarr_count(&dev->replay)) {
		assert(!seqno);
		return;
	}

	VERBOSE("we currently got %d blocks, and we expect %d blocks",
			dllarr_count(&dev->replay), dev->no_macroblocks);

	pthd_mutex_lock(&b->unallocated_mutex);

	for (i = 0; i < dev->b->max_macroblocks; i++) {
		if (blockio_dev_get_macroblock_status_bynum(dev, i) == USED) {
			if (i >= dev->b->total_macroblocks)
				FATAL("macroblock marked as USED that "
						"does not exist");

			blockio_info_t *bi = &dev->b->blockio_infos[i];
			if (bi->dev == dev) continue; // ok
			else if (bi->dev)
				FATAL("block %u USED by us but claimed "
						"by another device", i);

			dllarr_remove(&dev->b->unallocated, bi);
			bi->dev = dev;
			blockio_prepare_block(bi);
			juggler_add_macroblock(&dev->j, bi);
		}	
	}

	pthd_mutex_unlock(&b->unallocated_mutex);

	if (dev->no_macroblocks !=
			dllarr_count(&dev->replay) + juggler_count(&dev->j))
		FATAL("count of macroblocks does not match "
				"the amount of blocks we have");
}

static void sort_to_replay(dllarr_t *replay, blockio_info_t *bi) {
	void *compare_seqno(blockio_info_t *bi, uint64_t *seqno) {
		assert(*seqno != bi->seqno);
		if (*seqno > bi->seqno) return NULL;
		else return bi;
	}
	dllarr_insert(replay, bi, dllarr_iterate(replay,
				(dllarr_iterator_t)compare_seqno,
				&bi->seqno));
}

void blockio_dev_scan_header(dllarr_t *replay, blockio_dev_t *dev,
		blockio_info_t *bi, uint64_t *seqno) {
	assert(dev && dev->b);
	int i;
	char zero[128] = { };
	uint32_t no = bi - dev->b->blockio_infos;

	assert(no < dev->b->total_macroblocks);

	char sha256[32];

	if (bi->dev) {
		//VERBOSE("macroblock %d already taken", no);
		return;
	}

	/* read header */
	dev->b->read(dev->io, BASE, ((off_t)no)<<dev->b->macroblock_log,
			1<<dev->b->mesoblk_log);

	// decrypt indexblock with IV=0: ciphertext is unique due to
	// first block being a hash of the whole index and the index
	// containing a unique seqno (ONLY TRUE FOR CBC(LIKE)!!!!!)
	//
	// the seqno is used as IV for all the other mesoblocks
	// in the datablock
	cipher_dec(dev->c, BASE, BASE, 0, 0, no);

	/* check magic */
	if (memcmp(magic, MAGIC64, sizeof(magic))) {
//		DEBUG("magic \"%.*s\" not found in macroblock %u",
//				(int)sizeof(magic), magic, no);
		return;
	} //else DEBUG("magic \"%.*s\" found in macroblock %u",
	//		(int)sizeof(magic), magic, no);


	/* check indexblock hash */
	gcry_md_hash_buffer(GCRY_MD_SHA256, sha256, BASE + sizeof(sha256),
			(1<<dev->b->mesoblk_log) - sizeof(sha256));
	if (memcmp(INDEXBLOCK_SHA256, sha256, sizeof(sha256))) {
		DEBUG("sha256 hash of index block %d failed", no);
		return;
	}

	/* block seems to belong to us */
	/* store the hash of the datablocks, seqno and next_seqno */
	memcpy(bi->data_hash, DATABLOCKS_SHA256, 32);
	bi->seqno = binio_read_uint64_be(SEQNO_UINT64);
	bi->next_seqno = binio_read_uint64_be(NEXT_SEQNO_UINT64);

	if (memcmp(zero, RESERVED_SPACE1024, 128))
		FATAL("reserved space is not zeroed out");
	if (bi->seqno == 0) 
		FATAL("block found with seqno 0, not possibile");
	if (bi->next_seqno <= bi->seqno)
		FATAL("block found with seqno >= next_seqno, not possible");

	bi->no_nonobsolete = bi->no_indices =
		binio_read_uint32_be(NO_INDICES_UINT32);
	bi->indices = ecalloc(dev->b->mmpm, sizeof(uint32_t));
	
	for (i = 1; i <= bi->no_indices; i++) {
		bi->indices[i-1] = binio_read_uint32_be(
				((uint32_t*)NO_INDICES_UINT32) + i);
	}

	/* continue with the same value of i */
	for (; i <= dev->b->mmpm; i++)
		if (binio_read_uint32_be(((uint32_t*)NO_INDICES_UINT32) + i))
			FATAL("unused indices must be zero, they are not");

	VERBOSE("block %u (seqno=%lu) of \"%s\" has %d indices, "
			"next_seqno=%lu",
			no, bi->seqno, dev->name, bi->no_indices,
			bi->next_seqno);

	dllarr_remove(&dev->b->unallocated, bi);
	sort_to_replay(replay, bi);

	assert(bi->seqno != *seqno);
	if (bi->seqno > *seqno) { // candidate found for highest seqno
		*seqno = bi->seqno;

		bitmap_read(&dev->status, (uint32_t*)BITMAP);

		memcpy(dev->seqnos_hash, SEQNOS_SHA256, 32);

		dev->no_macroblocks = binio_read_uint32_be(
				NO_MACROBLOCKS_UINT32);
		dev->reserved_macroblocks = binio_read_uint32_be(
				RESERVED_BLOCKS_UINT32);

		//VERBOSE("block %u is candidate for highest seqno", no);
	}

	/* mark the block as ours */
	bi->dev = dev;
}

void blockio_dev_read_mesoblk_part(blockio_dev_t *dev, void *buf, uint32_t id,
		uint32_t no, uint32_t offset, uint32_t len) {
	assert(dev->b && dev->b->read && id < dev->b->total_macroblocks &&
			no < dev->b->mmpm);
	unsigned char mesoblk[1<<dev->b->mesoblk_log];

	/* FIXME: maybe do some caching */
	blockio_dev_read_mesoblk(dev, mesoblk, id, no);
	memcpy(buf, mesoblk + offset, len);
}

void blockio_dev_read_mesoblk(blockio_dev_t *dev,
		void *buf, uint32_t id, uint32_t no) {
	dev->b->read(dev->io, buf, (((off_t)id)<<dev->b->macroblock_log) +
			((no + 1)<<dev->b->mesoblk_log) +
			0, 1<<dev->b->mesoblk_log);
	cipher_dec(dev->c, buf, buf, dev->b->blockio_infos[id].seqno,
			no + 1, id);
}

int blockio_check_data_hash(blockio_info_t *bi) {
	uint32_t id = blockio_get_macroblock_index(bi);
	size_t size = (1<<bi->dev->b->macroblock_log) -
		(1<<bi->dev->b->mesoblk_log);
	char data[size];
	char hash[32];
	bi->dev->b->read(bi->dev->io, data,
			(((off_t)id)<<bi->dev->b->macroblock_log) +
			(1<<bi->dev->b->mesoblk_log), size);

	gcry_md_hash_buffer(GCRY_MD_SHA256, hash, data, size);

	//verbose_buffer("sha256_data", hash, 32);

	if (memcmp(bi->data_hash, hash, sizeof(hash))) return 0;

	return 1;
}

void blockio_dev_write_current_macroblock(blockio_dev_t *dev) {
	uint32_t id = blockio_get_macroblock_index(dev->bi);
	int i;
	assert(dev->bi && id < dev->b->total_macroblocks);
	
	DEBUG("write block %u (seqno=%lu, next_seqno=%lu)", id,
		       	dev->bi->seqno, dev->bi->next_seqno);
	dev->writes++;

	/* zero out the unused datablock,
	 * so that we do not encrypt 'random' data */
	for (i = dev->bi->no_indices + 1; i <= dev->b->mmpm; i++) {
		memset(BASE + (i<<dev->b->mesoblk_log),
				0, 1<<dev->b->mesoblk_log);
	}

	/* encrypt datablocks (also the unused ones) */
	for (i = 1; i <= dev->b->mmpm; i++)
		cipher_enc(dev->c, BASE + (i<<dev->b->mesoblk_log),
			BASE + (i<<dev->b->mesoblk_log),
			dev->bi->seqno, i, id);

	/* calculate hash of data, store in index */
	gcry_md_hash_buffer(GCRY_MD_SHA256, DATABLOCKS_SHA256,
			BASE + (1<<dev->b->mesoblk_log),
			dev->b->mmpm<<dev->b->mesoblk_log);
	//verbose_buffer("sha256_data", DATABLOCKS_HASH, 32);

	/* calculate hash of seqnos */
	juggler_hash_scheduled_seqnos(&dev->j, SEQNOS_SHA256);

	/* write static data */
	binio_write_uint64_be(SEQNO_UINT64, dev->bi->seqno);
	binio_write_uint64_be(NEXT_SEQNO_UINT64, dev->bi->next_seqno);
	memcpy(MAGIC64, magic, sizeof(magic));
	binio_write_uint32_be(RESERVED_BLOCKS_UINT32,
			dev->reserved_macroblocks);
	binio_write_uint32_be(NO_MACROBLOCKS_UINT32, dev->no_macroblocks);
	memset(RESERVED_SPACE1024, 0, 128);

	dev->bi->no_nonobsolete = dev->bi->no_indices;
	binio_write_uint32_be(NO_INDICES_UINT32, dev->bi->no_indices);
	for (i = 1; i <= dev->bi->no_indices; i++)
		binio_write_uint32_be(((uint32_t*)NO_INDICES_UINT32) + i,
				dev->bi->indices[i-1]);

	/* set unused indices to zero, continue with last value of i */
	for (; i <= dev->b->mmpm; i++)
		binio_write_uint32_be(((uint32_t*)NO_INDICES_UINT32) + i, 0);

	bitmap_write((uint32_t*)(BASE + dev->b->bitmap_offset), &dev->status);

	/* calculate hash of indexblock */
	gcry_md_hash_buffer(GCRY_MD_SHA256, INDEXBLOCK_SHA256,
			dev->tmp_macroblock + 32 /* size of hash */,
			(1<<dev->b->mesoblk_log) - 32 /* size of hash */);

	/* encrypt index */
	cipher_enc(dev->c, BASE, BASE, 0, 0, id);

	dev->b->write(dev->io, BASE, ((off_t)id)<<dev->b->macroblock_log,
			1<<dev->b->macroblock_log);

	dev->bi = NULL; /* there is no current block */
}

blockio_dev_macroblock_status_t blockio_dev_get_macroblock_status_bynum(
		blockio_dev_t *dev, uint32_t num) {
	return bitmap_getbits(&dev->status, num, 1);
}

blockio_dev_macroblock_status_t blockio_dev_get_macroblock_status(
		blockio_info_t *bi) {
	assert(bi);
	uint32_t index = blockio_get_macroblock_index(bi);
	assert(index < bi->dev->b->total_macroblocks);

	return blockio_dev_get_macroblock_status_bynum(bi->dev, index);
}

void blockio_dev_change_macroblock_status(blockio_info_t *bi,
		blockio_dev_macroblock_status_t new) {
	assert(bi);
	uint32_t index = blockio_get_macroblock_index(bi);
	assert(bitmap_getbits(&bi->dev->status, index, 1) != new);

	if (new&1) bitmap_setbit_safe(&bi->dev->status, index);
	else bitmap_clearbit_safe(&bi->dev->status, index);
}

void blockio_prepare_block(blockio_info_t *bi) {
	assert(bi->dev);
	bi->next = NULL;
	bi->seqno = bi->next_seqno = 0;
	bi->no_indices = bi->no_nonobsolete = 0;
	bi->indices = ecalloc(bi->dev->b->mmpm, sizeof(uint32_t));
}

int blockio_dev_allocate_macroblocks(blockio_dev_t *dev, uint32_t size) {
	int err = 0;
	DEBUG("we got a request to add %d macroblocks to %s",
			size, dev->name);

	pthd_mutex_lock(&dev->b->unallocated_mutex);

	if (dllarr_count(&dev->b->unallocated) < size) {
		err = -1; // not enough unclaimed blocks avaiable
		// fall through to unlock
	} else while (size--) {
		blockio_info_t *bi;
		uint32_t no = random_custom(&dev->r,
				dllarr_count(&dev->b->unallocated));

		bi = dllarr_remove(&dev->b->unallocated,
					dllarr_nth(&dev->b->unallocated, no));

		bi->dev = dev;

		blockio_prepare_block(bi);

		dev->no_macroblocks++;

		juggler_add_macroblock(&dev->j, bi);
		blockio_dev_change_macroblock_status(bi, USED);
	}
	
	pthd_mutex_unlock(&dev->b->unallocated_mutex);

	return err;
}
