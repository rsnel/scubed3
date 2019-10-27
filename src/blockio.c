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

void blockio_free(blockio_t *b) {
	assert(b);
	free(b->blockio_infos);
	pthd_mutex_destroy(&b->unallocated_mutex);
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

	dllarr_init(&b->unallocated, offsetof(blockio_info_t, ufs));
	for (uint32_t i = 0; i < b->total_macroblocks; i++) 
		dllarr_append(&b->unallocated, &b->blockio_infos[i]);

	stream_close(priv);
	pthd_mutex_init(&b->unallocated_mutex);
}

static const char magic[8] = "SSS3v0.1";

void blockio_dev_free(blockio_dev_t *dev) {
	int i;
	assert(dev);
	//VERBOSE("closing \"%s\", %s", dev->name,
	//		dev->updated?"SHOULD BE WRITTEN":"no updates");
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

	pthd_mutex_unlock(&dev->b->unallocated_mutex);

	free(dev->tmp_macroblock);
	free(dev->name);
	if (dev->b && dev->b->close) dev->b->close(dev->io);
}

void blockio_dev_select_next_macroblock(blockio_dev_t *dev) {
	assert(!dev->bi);

	DEBUG("blockio_dev_select_next_macroblock(dev=%p)", dev);
	dev->updated = 0;

	dev->bi = juggler_get_devblock(&dev->j, 0);
	assert(dev->bi);

	uint32_t number = dev->bi - dev->b->blockio_infos;
	VERBOSE("new block %d seqno=%ld next_seqno=%ld",
			number, dev->bi->seqno, dev->bi->next_seqno);
}

void blockio_dev_write_current_and_select_next_macroblock(
		blockio_dev_t *dev) {
	blockio_dev_write_current_macroblock(dev);
	blockio_dev_select_next_macroblock(dev);
}

void blockio_dev_init(blockio_dev_t *dev, blockio_t *b, cipher_t *c,
		const char *name) {
	int i; //, tmp_no_macroblocks = 0; //, tmp2 = 0;
	uint64_t highest_seqno = 0;
	//uint8_t hash[32];

	assert(b && c);
	assert(b->mesoblk_log < b->macroblock_log);
	random_init(&dev->r);
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
		blockio_dev_scan_header(dev, i, &highest_seqno);

	pthd_mutex_unlock(&b->unallocated_mutex);

	if (dev->no_macroblocks)
		FATAL("we found blocks, blockio dev init "
				"cannot handle that currently");
#if 0
	/* check all macroblocks in the map */
	for (i = 0; i < b->no_macroblocks; i++) {
		blockio_info_t *bi = &b->blockio_infos[i];
		fos = &dev->free_blocks;
		switch (blockio_dev_get_macroblock_status(dev, i)) {
			case NOT_ALLOCATED:
				if (bi->dev == dev) ecch_throw(ECCH_DEFAULT,
						"block %d contains our "
						"signature, but it is not in "
						"the list, it should have "
						"been overwritten, BUG!!!",
						i);
				break;

			case HAS_DATA:
				if (!bi->dev) ecch_throw(ECCH_DEFAULT, "unable "
						"to open partition \"%s\", "
						"datablock %d missing "
						"(overwritten?)", name, i);
				if (bi->dev != dev) ecch_throw(ECCH_DEFAULT,
						"unable to open partition "
						"\"%s\", datablock %d is in "
						"use by \"%s\"", name, i,
						bi->dev->name);
				if (!bi->no_indices) ecch_throw(ECCH_DEFAULT,
						"unable to open partition "
						"\"%s\", datablock %d is empty",
						name, i);
				assert(tmp_no_macroblocks >
						dev->rev[0].no_macroblocks);
				bi->internal = dev->rev[0].no_macroblocks;
				dev->macroblock_ref[
					dev->rev[0].no_macroblocks++] = i;
				add_to_used(bi);
				add_to_ordered(bi);
				break;

			case SELECTFROM:
				if (bi->seqno == highest_seqno)
					dev->keep_revisions--;
				assert(tmp2 < dev->random_len - 1);
				rebuild_prng[tmp2++] = dev->rev[0].no_macroblocks;
				bi->next_seqno = 0;
				bi->layout_revision = dev->layout_revision;
				fos = &dev->selected_blocks;
			case FREE:
				/* check if someone has taken it over */
				if (bi->dev != dev) {
					if (bi->dev) ecch_throw(ECCH_DEFAULT,
							"unable to open "
							"partition, datablock "
							"%ld is claimed by "
							"partition \"%s\"",
							bi - dev->b->
							blockio_infos,
							bi->dev->name);

					/* it is our's, we just never wrote
					 * to it */
					bi->next_seqno = bi->seqno = 0;
					bi->layout_revision = 0;
					assert(!bi->dev);
					bi->dev = dev;
					bi->indices = ecalloc(dev->mmpm,
							sizeof(uint32_t));
				}

				/* block is free, if it contains data,
				 * this data is obsolete... */
				bi->no_indices = 0;
				bi->no_nonobsolete = 0;
				bi->no_indices_gc = 0;
				bi->no_indices_preempt = 0;

				// FIXME, this is ugly
				if (i == dev->tail_macroblock) {
					bi->next_seqno = 0;
					bi->layout_revision = dev->layout_revision;
				}

				assert(tmp_no_macroblocks >
						dev->rev[0].no_macroblocks);
				bi->internal = dev->rev[0].no_macroblocks;
				dev->macroblock_ref[
					dev->rev[0].no_macroblocks++] = i;
				dllarr_append(fos, bi);
				add_to_ordered(bi);
				break;
		}
	}
#endif
	//assert(tmp_no_macroblocks == dev->rev[0].no_macroblocks);

	//assert(tmp2 <= dev->random_len - 1);

#if 0
	blockio_verbose_ordered(dev);

	for (i = 0; i < MACROBLOCK_HISTORY; i++)
		VERBOSE("bla %llu %u %u", dev->rev[i].seqno,
				dev->rev[i].no_macroblocks,
				dev->rev[i].work);
#endif

	/*while (bi->layout_revision < dev->layout_revision) bi = uptranslate(bi);*/


#if 0
	random_push(&dev->r, dev->b->blockio_infos[
			dev->tail_macroblock].internal);
	for (i = 0; i < tmp2; i++) random_push(&dev->r, rebuild_prng[i]);

	blockio_info_t *latest = dllarr_last(&dev->used_blocks);

	if (latest) {
		hash_seqnos(dev, hash, latest);
		if (memcmp(hash, latest->seqnos_hash, 32))
			ecch_throw(ECCH_DEFAULT,
				       	"hash of seqnos of required "
					"datablocks failed, revision "
					"%ld corrupt", latest->seqno);
	}

	VERBOSE("we have %d macroblocks in partition \"%s\", "
			"latest revision %ld OK", dev->rev[0].no_macroblocks,
		       	dev->name, (latest)?latest->seqno:0);

	/* select next block */
	blockio_dev_select_next_macroblock(dev, 0);
#endif
}

void blockio_dev_scan_header(blockio_dev_t *dev, uint32_t no,
		uint64_t *highest_seqno) {
	assert(dev && dev->b && no < dev->b->total_macroblocks);
	blockio_info_t *bi = &dev->b->blockio_infos[no];
	int i;
	char sha256[32];
	//VERBOSE("reading macroblock %d", no);
	bi = &dev->b->blockio_infos[no];

	if (bi->dev) {
		VERBOSE("macroblock %d already taken", no);
		return;
	}

	dev->b->read(dev->io, BASE, no<<dev->b->macroblock_log,
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
		DEBUG("magic \"%.*s\" not found in macroblock %u",
				(int)sizeof(magic), magic, no);
		return;
	} else DEBUG("magic \"%.*s\" found in macroblock %u", (int)sizeof(magic), magic, no);


	/* check indexblock hash */
	gcry_md_hash_buffer(GCRY_MD_SHA256, sha256, BASE + sizeof(sha256),
			(1<<dev->b->mesoblk_log) - sizeof(sha256));
	if (memcmp(INDEXBLOCK_SHA256, sha256, sizeof(sha256))) {
		DEBUG("sha256 hash of index block %d failed", no);
		return;
	}

	/* block seems to belong to us */
	memcpy(bi->data_hash, DATABLOCKS_SHA256, 32);
	memcpy(bi->seqnos_hash, SEQNOS_SHA256, 32);
	bi->seqno = binio_read_uint64_be(SEQNO_UINT64);
	bi->next_seqno = binio_read_uint64_be(NEXT_SEQNO_UINT64);

	dllarr_remove(&dev->b->unallocated, bi);
	dev->no_macroblocks++;
	juggler_add_macroblock(&dev->j, bi);

	FATAL("blockio_dev_scan_header not completely implemented");

#if 0
	if (bi->seqno > *highest_seqno) {
		*highest_seqno = bi->seqno;
		dev->layout_revision = bi->layout_revision;
		dev->tail_macroblock =
			binio_read_uint16_be(TAIL_MACROBLOCK);
		for (i = 0; i < MACROBLOCK_HISTORY; i++) {
			dev->rev[i].seqno =
				binio_read_uint64_be(REV_SEQNOS + 8*i);
			dev->rev[i].no_macroblocks =
				binio_read_uint16_be(NO_MACROBLOCKS + 2*i);
			dev->rev[i].work =
				binio_read_uint16_be(REVISION_WORK + 2*i);
		}
		dev->reserved_macroblocks =
			binio_read_uint16_be(RESERVED_MACROBLOCKS);
		dev->random_len = binio_read_uint32_be(RANDOM_LEN);
		bitmap_read(&dev->status, (uint32_t*)BITMAP);
	}
#endif

	bi->no_nonobsolete = bi->no_indices =
		binio_read_uint32_be(NO_INDICES_UINT32);
	bi->indices = ecalloc(dev->b->mmpm, sizeof(uint32_t));
	for (i = 1; i <= bi->no_indices; i++)
		bi->indices[i-1] = binio_read_uint32_be(
				((uint32_t*)NO_INDICES_UINT32) + i);

	VERBOSE("block %u (seqno=%lu) of \"%s\" has %d indices, "
			"next_seqno=%lu",
			no, bi->seqno, dev->name, bi->no_indices,
			bi->next_seqno);

	bi->dev = dev;

	return;
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
	dev->b->read(dev->io, buf, (id<<dev->b->macroblock_log) +
			((no + 1)<<dev->b->mesoblk_log) +
			0, 1<<dev->b->mesoblk_log);
	cipher_dec(dev->c, buf, buf, dev->b->blockio_infos[id].seqno,
			no + 1, id);
}

int blockio_check_data_hash(blockio_info_t *bi) {
	uint32_t id = bi - bi->dev->b->blockio_infos;
	size_t size = (1<<bi->dev->b->macroblock_log) -
		(1<<bi->dev->b->mesoblk_log);
	char data[size];
	char hash[32];
	bi->dev->b->read(bi->dev->io, data,
			(id<<bi->dev->b->macroblock_log) +
			(1<<bi->dev->b->mesoblk_log), size);

	gcry_md_hash_buffer(GCRY_MD_SHA256, hash, data, size);

	//verbose_buffer("sha256_data", hash, 32);

	if (memcmp(bi->data_hash, hash, sizeof(hash))) return 0;

	return 1;
}

void blockio_dev_write_current_macroblock(blockio_dev_t *dev) {
	uint32_t id = dev->bi - dev->b->blockio_infos;
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

	dev->bi->no_nonobsolete = dev->bi->no_indices;
	binio_write_uint32_be(NO_INDICES_UINT32, dev->bi->no_indices);
	for (i = 1; i <= dev->bi->no_indices; i++)
		binio_write_uint32_be(((uint32_t*)NO_INDICES_UINT32) + i,
				dev->bi->indices[i-1]);

	/* set unused indices to zero */
	for (i = dev->bi->no_indices; i <= dev->b->mmpm; i++)
		binio_write_uint32_be(((uint32_t*)NO_INDICES_UINT32) + i, 0);

	bitmap_write((uint32_t*)(BASE + dev->b->bitmap_offset), &dev->status);

	/* calculate hash of indexblock */
	gcry_md_hash_buffer(GCRY_MD_SHA256, INDEXBLOCK_SHA256,
			dev->tmp_macroblock + 32 /* size of hash */,
			(1<<dev->b->mesoblk_log) - 32 /* size of hash */);

	/* encrypt index */
	cipher_enc(dev->c, BASE, BASE, 0, 0, id);

	dev->b->write(dev->io, BASE, id<<dev->b->macroblock_log,
			1<<dev->b->macroblock_log);

	dev->bi = NULL; /* there is no current block */
}

static void blockio_dev_set_macroblock_status(blockio_dev_t *dev,
		uint32_t raw_no, blockio_dev_macroblock_status_t status) {
	assert(dev);
	assert(raw_no < dev->b->total_macroblocks);

	if (status&1) bitmap_setbit_safe(&dev->status, raw_no);
	else bitmap_clearbit_safe(&dev->status, raw_no);
}

blockio_dev_macroblock_status_t blockio_dev_get_macroblock_status(
		blockio_dev_t *dev, uint32_t raw_no) {
	assert(dev);
	assert(raw_no < dev->b->total_macroblocks);

	return bitmap_getbits(&dev->status, raw_no, 1);
}

void blockio_dev_change_macroblock_status(blockio_dev_t *dev, uint32_t no,
		blockio_dev_macroblock_status_t old,
		blockio_dev_macroblock_status_t new) {
	assert(blockio_dev_get_macroblock_status(dev, no) == old);
	blockio_dev_set_macroblock_status(dev, no, new);
}

int blockio_dev_free_macroblocks(blockio_dev_t *dev, uint32_t size) {
	FATAL("blockio_dev_free_macroblocks is not implemented");
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

		/* cleanup block */
		bi->next = NULL;
		bi->seqno = bi->next_seqno = 0;
		bi->no_indices = bi->no_nonobsolete = bi->no_indices_gc = 0;
		bi->no_indices_preempt = 0;
		bi->indices = ecalloc(dev->b->mmpm, sizeof(uint32_t));

		juggler_add_macroblock(&dev->j, bi);

		dev->no_macroblocks++;
		bi->dev = dev;
	}
	
	pthd_mutex_unlock(&dev->b->unallocated_mutex);

	return err;
}
