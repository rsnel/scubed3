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
#include <unistd.h>
#include <time.h>
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

#ifdef SCUBED3_CRASH_TEST
/* testing only: reads that include one of the bytes in SCUBED3_BAD_READ
 * (comma separated offsets) fail, until the byte is written (as a disk
 * remaps a bad sector when it is written) */
static int bad_sector(uint64_t offset, uint32_t size, int write) {
	static uint64_t bad[64];
	static int no_bad = -1;
	char *list, *end;
	int i, ret = 0;

	if (no_bad < 0) {
		no_bad = 0;
		list = getenv("SCUBED3_BAD_READ");
		while (list && *list && no_bad < 64) {
			bad[no_bad] = strtoull(list, &end, 10);
			if (end == list) break;
			no_bad++;
			list = (*end == ',')?end + 1:end;
		}
	}

	for (i = 0; i < no_bad; i++) {
		if (bad[i] < offset || bad[i] >= offset + size) continue;
		if (!write) ret = 1;
		else bad[i--] = bad[--no_bad];
	}

	return ret;
}
#endif

/* a read error is not fatal: the mesoblocks that cannot be read are
 * treated as corrupt */
static int stream_read(void *fp, void *buf, uint64_t offset, uint32_t size) {
	assert(fp && buf && size);
#ifdef SCUBED3_CRASH_TEST
	if (bad_sector(offset, size, 0)) {
		errno = EIO;
		return -1;
	}
#endif
	if (fseeko(fp, offset, SEEK_SET)) return -1;

	if (fread(buf, size, 1, fp) != 1) {
		if (!ferror(fp)) errno = EIO; /* unexpected end of file */
		clearerr(fp);
		return -1;
	}

	return 0;
}

static void stream_write(void *fp, const void *buf, uint64_t offset,
		uint32_t size) {
#ifdef SCUBED3_CRASH_TEST
	/* testing only: simulate a crash before the Nth write */
	static int writes = 0;
	char *crash = getenv("SCUBED3_CRASH_AFTER");
	if (crash && ++writes > atoi(crash)) {
		uint32_t i, written = 0;
		if (getenv("SCUBED3_TEAR")) {
			/* power failure during the write: a random subset
			 * of the 4096 byte pieces reaches the disk */
			srand(getpid() ^ time(NULL));
			for (i = 0; i + 4096 <= size; i += 4096) {
				if (rand()&1) continue;
				if (fseeko(fp, offset + i, SEEK_SET) ||
						fwrite(buf + i, 4096, 1, fp) != 1)
					FATAL("tear: %s", strerror(errno));
				written++;
			}
			fflush(fp);
		}
		WARNING("simulated crash before write %d (%u pieces of it "
				"written)", writes, written);
		_exit(9);
	}
	bad_sector(offset, size, 1);
#endif
	stream_io(fp, (void*)buf, offset, size, fwrite, "writing");
}

/* make everything written so far durable */
static void stream_sync(void *fp) {
	if (fflush(fp))
		FATAL("error flushing: %s", strerror(errno));
	if (fdatasync(fileno(fp)))
		FATAL("error syncing: %s", strerror(errno));
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
#define RESERVED32		(BASE + 0x020) /* MUST be 0 */
#define TRIM_FLOOR_UINT64	(BASE + 0x040) /* followed by 24 zero bytes */
#define SEQNO_UINT64		(BASE + 0x060)
#define NEXT_BLOCK_UINT32	(BASE + 0x068) /* followed by 4 zero bytes */
#define MAGIC64			(BASE + 0x070)
#define NO_MACROBLOCKS_UINT32	(BASE + 0x078)
#define RESERVED_BLOCKS_UINT32	(BASE + 0x07C)
#define RESERVED_SPACE1024	(BASE + 0x080)
#define PENDING_UINT32		(BASE + 0x080)
#define NO_TRIMS_UINT32		(BASE + 0x084)
#define TRIMS			(BASE + 0x088) /* 12 bytes per trim */
#define TRIM_SEQNO_UINT64(i)	(TRIMS + 12*(i))
#define TRIM_SIZE_UINT32(i)	(TRIMS + 12*(i) + 8)
#define NO_INDICES_UINT32	(BASE + 0x100)
#define HASH_UINT64(k)		(BASE + dev->b->hashes_offset + ((k)<<3))
#define BITMAP			(BASE + dev->b->bitmap_offset)

/* allocate and free the per block tables (indices and hashes) */
static void blockio_info_alloc_tables(blockio_info_t *bi, uint16_t mmpm) {
	bi->indices = ecalloc(mmpm, sizeof(uint32_t));
	bi->hashes = ecalloc(mmpm, sizeof(uint64_t));
}

static void blockio_info_free_tables(blockio_info_t *bi) {
	free(bi->indices);
	free(bi->hashes);
	bi->indices = NULL;
	bi->hashes = NULL;
}

/* truncated SHA256 of the ciphertext of a mesoblock, the low bits are
 * left zero, they mark corrupt parts */
static uint64_t mesoblk_hash(const void *ciphertext, size_t size) {
	unsigned char sha256[32];

	gcry_md_hash_buffer(GCRY_MD_SHA256, sha256, ciphertext, size);

	return binio_read_uint64_be(sha256)&~(uint64_t)BLOCKIO_CORRUPT_MASK;
}

void blockio_free(blockio_t *b) {
	assert(b);
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

	/* fixed part, indices and mesoblock hashes (the hashes of the
	 * mesoblocks and one unused uint64_t, to keep it simple) */
	b->hashes_offset = 256 + (4<<(macroblock_log - mesoblk_log));
	b->bitmap_offset = b->hashes_offset +
		(8<<(macroblock_log - mesoblk_log));
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
	b->sync = stream_sync;

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

	/* a location (macroblock, mesoblock) must fit in 32 bits, and must
	 * not be 0xFFFFFFFF (unused) */
	if (((uint64_t)b->total_macroblocks<<(macroblock_log - mesoblk_log)) >=
			0xFFFFFFFF)
		FATAL("device is too large for these block sizes");

	b->blockio_infos = ecalloc(sizeof(blockio_info_t),
			b->total_macroblocks);

	dllarr_init(&b->unallocated, offsetof(blockio_info_t, ur));
	for (uint32_t i = 0; i < b->total_macroblocks; i++) 
		dllarr_append(&b->unallocated, &b->blockio_infos[i]);

	stream_close(priv);
	pthd_mutex_init(&b->unallocated_mutex);
}

static const char magic[8] = "SSS3v0.4";

static void blockio_dev_take_index(blockio_dev_t*);

static void blockio_dev_read_index(blockio_dev_t*, blockio_info_t*);

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
			blockio_info_free_tables(bi);
			bi->dev = NULL;
		}
	}

	/* the replay list may still contain blocks (creating an existing
	 * device for example), they are also in the pool of the juggler */
	while ((bi = dllarr_last(&dev->replay)))
		dllarr_remove(&dev->replay, bi);

	/* re-add blocks to unallocated (removed blocks are not in the pool,
	 * the removal is on disk: the current block was written above) */
	juggler_free_and_empty_into(&dev->j, dllarr_append,
			&dev->b->unallocated);
	while ((bi = dllarr_last(&dev->release))) {
		dllarr_remove(&dev->release, bi);
		dllarr_append(&dev->b->unallocated, bi);
	}

	pthd_mutex_unlock(&dev->b->unallocated_mutex);

	dllarr_free(&dev->replay);
	dllarr_free(&dev->release);
	free(dev->old_ondisk);
	free(dev->refcount);
	free(dev->tombs);
	free(dev->tmp_macroblock);
	free(dev->out_macroblock);
	free(dev->name);
	if (dev->b && dev->b->close) dev->b->close(dev->io);
}

uint32_t blockio_dev_usable_macroblocks(blockio_dev_t *dev) {
	uint32_t target = dev->no_macroblocks - dev->pending_removals;

	if (target <= dev->reserved_macroblocks) return 0;

	return target - dev->reserved_macroblocks;
}

/* the number of slots of the current block that can still be used for
 * data, the tombstones take slots after the data */
uint32_t blockio_dev_free_slots(blockio_dev_t *dev) {
	uint32_t used;
	assert(dev->bi);

	used = dev->bi->no_indices + (dev->no_tombs + dev->tombs_per_mesoblk -
			1)/dev->tombs_per_mesoblk;
	assert(used <= dev->b->mmpm);

	return dev->b->mmpm - used;
}

/* do no more tombstones fit in the current block? */
int blockio_dev_tomb_fits(blockio_dev_t *dev, uint32_t no) {
	assert(dev->bi);

	return dev->bi->no_indices + (dev->no_tombs + no +
			dev->tombs_per_mesoblk - 1)/dev->tombs_per_mesoblk <=
		dev->b->mmpm;
}

/* add a tombstone for logical mesoblock L to the current block, the
 * caller must check that it fits */
void blockio_dev_add_tomb(blockio_dev_t *dev, uint32_t L) {
	assert(blockio_dev_tomb_fits(dev, 1));

	if (dev->no_tombs == dev->size_tombs) {
		dev->size_tombs = dev->size_tombs?2*dev->size_tombs:
			dev->tombs_per_mesoblk;
		dev->tombs = erealloc(dev->tombs, dev->size_tombs,
				sizeof(uint32_t));
	}
	dev->tombs[dev->no_tombs++] = L;
}

static void count_range(blockio_dev_t *dev, uint32_t *indices, uint32_t n,
		uint32_t from) {
	uint32_t k;

	for (k = 0; k < n; k++)
		if (indices[k] >= from && indices[k] < dev->no_refcount)
			dev->refcount[indices[k]]++;
}

/* the partition has size mesoblocks now; when it grows, the indices on
 * disk that refer to the new part are counted (they may be in the blocks
 * of the partition from before an earlier shrink) */
void blockio_dev_resize_refcount(blockio_dev_t *dev, uint32_t size) {
	uint32_t old = dev->no_refcount, i;
	blockio_info_t *bi;

	dev->refcount = erealloc(dev->refcount, size?size:1, sizeof(uint32_t));
	dev->no_refcount = size;
	if (size <= old) return;

	memset(dev->refcount + old, 0, (size - old)*sizeof(uint32_t));

	for (i = 0; i < juggler_count(&dev->j); i++) {
		bi = juggler_nth(&dev->j, i);
		if (bi == dev->bi) count_range(dev, dev->old_ondisk,
				dev->no_old_ondisk, old);
		else count_range(dev, bi->indices, bi->no_ondisk, old);
	}

	for (bi = dllarr_first(&dev->release); bi;
			bi = dllarr_next(&dev->release, bi))
		count_range(dev, bi->indices, bi->no_ondisk, old);
}

/* count (sign = 1) or uncount (sign = -1) the on disk indices of a block */
void blockio_dev_count_ondisk(blockio_dev_t *dev, blockio_info_t *bi,
		int sign) {
	uint32_t k;

	for (k = 0; k < bi->no_ondisk; k++) {
		if (bi->indices[k] >= dev->no_refcount) continue;
		assert(sign > 0 || dev->refcount[bi->indices[k]]);
		dev->refcount[bi->indices[k]] += sign;
	}
}

/* read the tombstones in slot no of block bi into tombs (room for
 * tombs_per_mesoblk), returns how many there are; if the slot is corrupt,
 * the tombstones are lost (discarded data may come back) */
uint32_t blockio_dev_read_tombs(blockio_dev_t *dev, blockio_info_t *bi,
		uint32_t no, uint32_t *tombs) {
	unsigned char buf[1<<dev->b->mesoblk_log];
	uint32_t n, i, id = blockio_get_macroblock_index(bi);

	assert(bi->indices[no] == BLOCKIO_TOMBSTONES);

	if (blockio_dev_read_mesoblk(dev, buf, id, no)) {
		ERROR("tombstones in slot %u of block %u of \"%s\" are "
				"corrupt, discarded data may come back", no, id,
				dev->name);
		return 0;
	}

	n = binio_read_uint32_be(buf);
	if (n > dev->tombs_per_mesoblk) {
		ERROR("invalid number of tombstones %u in slot %u of block %u "
				"of \"%s\"", n, no, id, dev->name);
		return 0;
	}

	for (i = 0; i < n; i++)
		tombs[i] = binio_read_uint32_be(buf + 4 + 4*i);

	return n;
}

/* references from blocks older than trims[i].seqno to mesoblocks
 * >= trims[i].size are dead */
int blockio_dev_is_trimmed(blockio_dev_t *dev, uint64_t seqno,
		uint32_t mesoblock) {
	int i;

	for (i = 0; i < dev->no_trims; i++)
		if (seqno < dev->trims[i].seqno &&
				mesoblock >= dev->trims[i].size) return 1;

	return 0;
}

/* record that the partition is shrunk to size mesoblocks, at seqno;
 * returns -1 if there is no room to record it */
int blockio_dev_add_trim(blockio_dev_t *dev, uint64_t seqno, uint32_t size) {
	int i, j = 0;

	/* the new trim makes older trims with a larger size redundant */
	for (i = 0; i < dev->no_trims; i++)
		if (dev->trims[i].size < size) dev->trims[j++] = dev->trims[i];
	dev->no_trims = j;

	if (dev->no_trims == BLOCKIO_MAX_TRIMS) return -1;

	dev->trims[dev->no_trims].seqno = seqno;
	dev->trims[dev->no_trims++].size = size;

	return 0;
}

/* a trim is no longer needed if no block of the partition
 * on disk is older than the trim */
static void blockio_dev_prune_trims(blockio_dev_t *dev) {
	blockio_info_t *bi;
	uint64_t oldest = dev->bi->seqno;
	uint32_t i, j = 0;

	/* blocks that were never written (seqno 0) have no references */
	for (i = 0; i < juggler_count(&dev->j); i++) {
		bi = juggler_nth(&dev->j, i);
		if (bi->seqno && bi->seqno < oldest) oldest = bi->seqno;
	}

	/* a pruned trim may still apply to removed blocks that carry
	 * our signature, they must never be replayed if they are added
	 * to the partition again: raise the floor */
	for (i = 0; i < dev->no_trims; i++) {
		if (dev->trims[i].seqno > oldest) dev->trims[j++] = dev->trims[i];
		else if (dev->trims[i].seqno > dev->trim_floor)
			dev->trim_floor = dev->trims[i].seqno;
	}
	dev->no_trims = j;
}

/* the block (not the current or next block) with the least live data,
 * that is the block that is removed next when the partition is shrunk */
blockio_info_t *blockio_dev_find_victim(blockio_dev_t *dev) {
	blockio_info_t *bi, *victim = NULL;
	uint32_t i;

	for (i = 0; i < juggler_count(&dev->j); i++) {
		bi = juggler_nth(&dev->j, i);
		if (bi == dev->bi || bi == dev->j.next) continue;
		if (!victim || bi->no_nonobsolete < victim->no_nonobsolete)
			victim = bi;
	}

	return victim;
}

/* remove a block without live data from the partition, it is given to
 * the pool of unallocated blocks after the next write (which records
 * the removal on disk), until then no other partition may use it */
void blockio_dev_remove_block(blockio_dev_t *dev, blockio_info_t *bi) {
	assert(bi->dev == dev && !bi->no_nonobsolete && dev->pending_removals);

	VERBOSE("removing block %u", blockio_get_macroblock_index(bi));
	juggler_remove_macroblock(&dev->j, bi);
	blockio_dev_change_macroblock_status(bi, FREE);
	dev->no_macroblocks--;
	dev->pending_removals--;
	if (dev->ram_block == bi) dev->ram_block = NULL;
	dllarr_append(&dev->release, bi);
	dev->updated = 1; /* the removal must be written */
}

/* after the removals are on disk, the blocks can be given away */
static void blockio_dev_release_removed(blockio_dev_t *dev) {
	blockio_info_t *bi;

	pthd_mutex_lock(&dev->b->unallocated_mutex);
	while ((bi = dllarr_last(&dev->release))) {
		dllarr_remove(&dev->release, bi);
		/* its index will not be replayed anymore */
		blockio_dev_count_ondisk(dev, bi, -1);
		blockio_info_free_tables(bi);
		bi->dev = NULL;
		bi->seqno = 0;
		dllarr_append(&dev->b->unallocated, bi);
	}
	pthd_mutex_unlock(&dev->b->unallocated_mutex);
}

/* the next block of the juggler becomes the current block, remember its
 * indices on disk: they stay on disk (and count) until it is written */
static void blockio_dev_take_current(blockio_dev_t *dev) {
	dev->bi = juggler_get_devblock(&dev->j);

	memcpy(dev->old_ondisk, dev->bi->indices,
			dev->bi->no_ondisk*sizeof(uint32_t));
	dev->no_old_ondisk = dev->bi->no_ondisk;
	assert(!dev->no_tombs);
}

void blockio_dev_select_next_macroblock(blockio_dev_t *dev) {
	blockio_info_t *due, *victim;
	uint32_t i;
	assert(!dev->bi);

	dev->updated = 0;

	/* removing the last block of the partition: it gets one more
	 * write (in its turn, it is the only block), in which it is FREE, so
	 * that the removal is on disk; afterwards there is no current block */
	if (dev->pending_removals && juggler_count(&dev->j) == 1 &&
			!juggler_nth(&dev->j, 0)->no_nonobsolete) {
		blockio_dev_take_current(dev);
		juggler_set_next(&dev->j, NULL);
		VERBOSE("removing the last block %u",
				blockio_get_macroblock_index(dev->bi));
		dev->bi->no_indices = 0;
		blockio_dev_remove_block(dev, dev->bi);
		blockio_dev_write_current_macroblock(dev);
		return;
	}

	/* if the block that is due still has live mesoblocks, it was
	 * scheduled to be rewritten right after it was written, its live
	 * data has not been copied elsewhere, so it must be in RAM; if it
	 * is not (we just opened the device) load it now, while its
	 * old seqno (needed for the IV) is still known */
	due = dev->j.next;
	if (due && due->no_indices && due != dev->ram_block) {
		uint32_t id = blockio_get_macroblock_index(due);
		VERBOSE("block %u is due and still has %u indices, loading",
				id, due->no_indices);
		for (i = 0; i < due->no_indices; i++)
			due->hashes[i] |= blockio_dev_read_mesoblk(dev,
					dev->tmp_macroblock +
					((i + 1)<<dev->b->mesoblk_log), id, i);
		dev->ram_block = due;
	}

	blockio_dev_take_current(dev);
	dev->ram_block = dev->bi; /* tmp_macroblock now belongs to bi */
	dev->tail_macroblock = dev->j.next;

	/* if the (empty) selected block is drawn again for the next write,
	 * it is its own tail: data written to it now would have nowhere to
	 * go when it is rewritten, so we write it out empty and select
	 * again (this happens with probability 1/N); if the partition has
	 * only one block, it is always its own tail, and there is no other
	 * block to absorb, so it can keep its data */
	while (dev->tail_macroblock == dev->bi && !dev->bi->no_indices &&
			juggler_count(&dev->j) > 1) {
		VERBOSE("block %d is drawn twice, writing it out empty "
				"(pool=%u)", blockio_get_macroblock_index(dev->bi),
				juggler_count(&dev->j));
		blockio_dev_write_current_macroblock(dev);
		blockio_dev_take_current(dev);
		dev->ram_block = dev->bi;
		dev->tail_macroblock = dev->j.next;
	}

	/* if we are shrinking, blocks without live data can be removed
	 * at once, blocks with live data are drained first (see
	 * copy_old_block_to_current) */
	while (dev->pending_removals && (victim = blockio_dev_find_victim(dev))
			&& !victim->no_nonobsolete)
		blockio_dev_remove_block(dev, victim);

	VERBOSE("new block %d seqno=%ld next=%d pool=%u",
			blockio_get_macroblock_index(dev->bi), dev->bi->seqno,
			blockio_get_macroblock_index(dev->tail_macroblock),
			juggler_count(&dev->j));
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
	uint32_t next_block;
	blockio_info_t *bi, *next;

	assert(b && c);
	assert(b->mesoblk_log < b->macroblock_log);
	random_init(&dev->r);
	dllarr_init(&dev->replay, offsetof(blockio_info_t, ur));
	dllarr_init(&dev->release, offsetof(blockio_info_t, ur));
	bitmap_init(&dev->status, b->max_macroblocks);
	juggler_init(&dev->j, &dev->r);

	dev->b = b;
	dev->c = c;
	assert(bitmap_size(&dev->status) + dev->b->bitmap_offset ==
			1<<dev->b->mesoblk_log);

	assert(b->open);
	dev->io = (b->open)(b->open_priv);

	dev->tmp_macroblock = ecalloc(1, 1<<b->macroblock_log);
	dev->out_macroblock = ecalloc(1, 1<<b->macroblock_log);
	dev->ram_block = NULL;
	dev->old_ondisk = ecalloc(b->mmpm, sizeof(uint32_t));
	/* a slot of tombstones: number of tombstones, tombstones */
	dev->tombs_per_mesoblk = (1<<(b->mesoblk_log - 2)) - 1;

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

	/* only the most recently written block can be incomplete (see
	 * blockio_dev_write_current_macroblock), if it is, we ignore it,
	 * as if it was never written: its old contents were absorbed by
	 * the block written before it, and the index of that block says
	 * that it is written next, so the write will be redone */
	while ((bi = dllarr_last(&dev->replay)) &&
			blockio_check_data_hashes(bi, NULL)) {
		WARNING("most recent block %u (seqno=%lu) of \"%s\" is "
				"incomplete (interrupted write), ignoring it",
				blockio_get_macroblock_index(bi), bi->seqno,
				dev->name);
		pthd_mutex_lock(&b->unallocated_mutex);
		dllarr_remove(&dev->replay, bi);
		blockio_info_free_tables(bi);
		bi->dev = NULL;
		bi->seqno = 0;
		dllarr_append(&b->unallocated, bi);
		pthd_mutex_unlock(&b->unallocated_mutex);

		if (!(bi = dllarr_last(&dev->replay))) {
			/* nothing left, the partition was never written */
			seqno = 0;
			dev->no_macroblocks = 0;
			return;
		}
	}

	/* take the state of the partition from the most recent block (with
	 * equal seqnos, the scan may have taken it from another block) */
	bi = dllarr_last(&dev->replay);
	seqno = bi->seqno;
	next_block = bi->next_block;
	blockio_dev_read_index(dev, bi);
	blockio_dev_take_index(dev);

	/* blocks with our signature that are FREE in the most recent bitmap
	 * are not part of the partition (they were removed), blocks with a
	 * seqno below the trim floor were removed and added again, they are
	 * treated as fresh blocks (the loop below adds them) */
	pthd_mutex_lock(&b->unallocated_mutex);

	bi = dllarr_first(&dev->replay);
	while (bi) {
		next = dllarr_next(&dev->replay, bi);
		if (blockio_dev_get_macroblock_status(bi) == FREE ||
				bi->seqno < dev->trim_floor) {
			VERBOSE("block %u (seqno=%lu) is stale",
					blockio_get_macroblock_index(bi),
					bi->seqno);
			dllarr_remove(&dev->replay, bi);
			blockio_info_free_tables(bi);
			bi->dev = NULL;
			bi->seqno = 0;
			dllarr_append(&b->unallocated, bi);
		}
		bi = next;
	}

	pthd_mutex_unlock(&b->unallocated_mutex);

	/* the juggler continues after the most recent block */
	dev->j.seqno = seqno;

	VERBOSE("we currently got %d blocks, and we expect %d blocks",
			dllarr_count(&dev->replay), dev->no_macroblocks);

	pthd_mutex_lock(&b->unallocated_mutex);

	for (i = 0; i < dev->b->max_macroblocks; i++) {
		if (blockio_dev_get_macroblock_status_bynum(dev, i) == USED) {
			if (i >= dev->b->total_macroblocks)
				FATAL("macroblock marked as USED that "
						"does not exist");

			bi = &dev->b->blockio_infos[i];
			if (bi->dev == dev) continue; // ok
			else if (bi->dev)
				FATAL("block %u USED by us but claimed "
						"by another device", i);

			/* we don't know what it contains, if it has
			 * data, adding it as a fresh block would lose
			 * that data silently */
			if (bi->unreadable)
				FATAL("block %u of \"%s\" cannot be read",
						i, dev->name);

			dllarr_remove(&dev->b->unallocated, bi);
			bi->dev = dev;
			blockio_prepare_block(bi);
			juggler_add_macroblock(&dev->j, bi);
		}	
	}

	pthd_mutex_unlock(&b->unallocated_mutex);

	/* all blocks of the partition (also the ones that will be
	 * replayed) are in the pool of the juggler */
	for (bi = dllarr_first(&dev->replay); bi;
			bi = dllarr_next(&dev->replay, bi))
		juggler_add_macroblock(&dev->j, bi);

	if (dev->no_macroblocks != juggler_count(&dev->j))
		FATAL("count of macroblocks does not match "
				"the amount of blocks we have");

	/* the most recent block says which block is written next, its
	 * live data is in the most recent block */
	if (next_block != 0xFFFFFFFF) {
		if (next_block >= dev->b->total_macroblocks ||
				dev->b->blockio_infos[next_block].dev != dev)
			FATAL("next block %u is not part of the partition",
					next_block);
		juggler_set_next(&dev->j, &dev->b->blockio_infos[next_block]);
	}
}

/* take the state of the partition (bitmap, size, ...) from the decrypted
 * index in BASE, this is done for the most recent block */
static void blockio_dev_take_index(blockio_dev_t *dev) {
	uint32_t i;

	bitmap_read(&dev->status, (uint32_t*)BITMAP);

	dev->trim_floor = binio_read_uint64_be(TRIM_FLOOR_UINT64);

	dev->no_macroblocks = binio_read_uint32_be(NO_MACROBLOCKS_UINT32);
	dev->reserved_macroblocks = binio_read_uint32_be(
			RESERVED_BLOCKS_UINT32);
	dev->pending_removals = binio_read_uint32_be(PENDING_UINT32);
	if (dev->pending_removals > dev->no_macroblocks)
		FATAL("more pending removals than blocks");
	dev->no_trims = binio_read_uint32_be(NO_TRIMS_UINT32);
	assert(dev->no_trims <= BLOCKIO_MAX_TRIMS); /* checked by scan */
	for (i = 0; i < dev->no_trims; i++) {
		dev->trims[i].seqno = binio_read_uint64_be(TRIM_SEQNO_UINT64(i));
		dev->trims[i].size = binio_read_uint32_be(TRIM_SIZE_UINT32(i));
	}
}

/* read and decrypt the index of a block of ours into BASE */
static void blockio_dev_read_index(blockio_dev_t *dev, blockio_info_t *bi) {
	uint32_t no = bi - dev->b->blockio_infos;
	char sha256[32];

	if (dev->b->read(dev->io, BASE, ((off_t)no)<<dev->b->macroblock_log,
			1<<dev->b->mesoblk_log))
		FATAL("error reading index of block %u: %s", no,
				strerror(errno));
	cipher_dec(dev->c, BASE, BASE, 0, 0, no);

	gcry_md_hash_buffer(GCRY_MD_SHA256, sha256, BASE + sizeof(sha256),
			(1<<dev->b->mesoblk_log) - sizeof(sha256));
	if (memcmp(magic, MAGIC64, sizeof(magic)) ||
			memcmp(INDEXBLOCK_SHA256, sha256, sizeof(sha256)))
		FATAL("index of block %u changed while opening", no);
}

static void sort_to_replay(dllarr_t *replay, blockio_info_t *bi) {
	/* equal seqnos are possible: an interrupted (ignored) write leaves
	 * a block that may be FREE now, with the same seqno as the block
	 * that was written in its slot later, the stale filter removes it */
	void *compare_seqno(blockio_info_t *bi, uint64_t *seqno) {
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
	uint32_t no = bi - dev->b->blockio_infos, no_trims;

	assert(no < dev->b->total_macroblocks);

	char sha256[32];

	if (bi->dev) {
		//VERBOSE("macroblock %d already taken", no);
		return;
	}

	/* read header, if that fails, the block may be ours, that is only
	 * known when the bitmap is known (see blockio_dev_init) */
	bi->unreadable = 0;
	if (dev->b->read(dev->io, BASE, ((off_t)no)<<dev->b->macroblock_log,
			1<<dev->b->mesoblk_log)) {
		WARNING("error reading index of block %u: %s", no,
				strerror(errno));
		bi->unreadable = 1;
		return;
	}

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
	/* store the seqno and next block */
	bi->seqno = binio_read_uint64_be(SEQNO_UINT64);
	bi->next_block = binio_read_uint32_be(NEXT_BLOCK_UINT32);

	no_trims = binio_read_uint32_be(NO_TRIMS_UINT32);
	if (no_trims > BLOCKIO_MAX_TRIMS)
		FATAL("too many trims in index of block %u", no);
	if (memcmp(zero, TRIMS + 12*no_trims, 128 - 8 - 12*no_trims))
		FATAL("reserved space is not zeroed out");
	if (bi->seqno == 0) 
		FATAL("block found with seqno 0, not possibile");

	bi->no_nonobsolete = bi->no_indices = bi->no_ondisk =
		binio_read_uint32_be(NO_INDICES_UINT32);
	if (bi->no_indices > dev->b->mmpm)
		FATAL("too many indices in block %u", no);
	blockio_info_alloc_tables(bi, dev->b->mmpm);

	for (i = 0; i < dev->b->mmpm; i++)
		bi->hashes[i] = binio_read_uint64_be(HASH_UINT64(i));

	for (i = 1; i <= bi->no_indices; i++) {
		bi->indices[i-1] = binio_read_uint32_be(
				((uint32_t*)NO_INDICES_UINT32) + i);
	}

	/* continue with the same value of i */
	for (; i <= dev->b->mmpm; i++)
		if (binio_read_uint32_be(((uint32_t*)NO_INDICES_UINT32) + i))
			FATAL("unused indices must be zero, they are not");

	VERBOSE("block %u (seqno=%lu) of \"%s\" has %d indices, "
			"next=%d", no, bi->seqno, dev->name, bi->no_indices,
			(int)bi->next_block);

	dllarr_remove(&dev->b->unallocated, bi);
	sort_to_replay(replay, bi);

	if (bi->seqno > *seqno) { // candidate found for highest seqno
		*seqno = bi->seqno;
		blockio_dev_take_index(dev);
		//VERBOSE("block %u is candidate for highest seqno", no);
	}

	/* mark the block as ours */
	bi->dev = dev;
}

int blockio_dev_read_mesoblk_part(blockio_dev_t *dev, void *buf, uint32_t id,
		uint32_t no, uint32_t offset, uint32_t len) {
	assert(dev->b && dev->b->read && id < dev->b->total_macroblocks &&
			no < dev->b->mmpm);
	unsigned char mesoblk[1<<dev->b->mesoblk_log];
	int corrupt;

	/* FIXME: maybe do some caching */
	corrupt = blockio_dev_read_mesoblk(dev, mesoblk, id, no);
	memcpy(buf, mesoblk + offset, len);

	return corrupt;
}

/* read and decrypt a mesoblock, its ciphertext is checked against the
 * hash in the index of its block */
int blockio_dev_read_mesoblk(blockio_dev_t *dev,
		void *buf, uint32_t id, uint32_t no) {
	blockio_info_t *bi = &dev->b->blockio_infos[id];
	int corrupt = bi->hashes[no]&BLOCKIO_CORRUPT_MASK;

	if (dev->b->read(dev->io, buf, (((off_t)id)<<dev->b->macroblock_log) +
			((no + 1)<<dev->b->mesoblk_log) +
			0, 1<<dev->b->mesoblk_log)) {
		WARNING("error reading mesoblock %u of block %u of \"%s\": "
				"%s", no, id, dev->name, strerror(errno));
		return BLOCKIO_CORRUPT_MASK;
	}
	if ((bi->hashes[no]&~(uint64_t)BLOCKIO_CORRUPT_MASK) !=
			mesoblk_hash(buf, 1<<dev->b->mesoblk_log)) {
		WARNING("mesoblock %u of block %u (seqno=%lu) of \"%s\" is "
				"corrupt", no, id, bi->seqno, dev->name);
		corrupt = BLOCKIO_CORRUPT_MASK;
	}
	cipher_dec(dev->c, buf, buf, bi->seqno, no + 1, id);

	return corrupt;
}

uint32_t blockio_check_data_hashes(blockio_info_t *bi, char *live) {
	uint32_t id = blockio_get_macroblock_index(bi), k, mismatches = 0;
	blockio_t *b = bi->dev->b;
	char *data = ecalloc(b->mmpm, 1<<b->mesoblk_log);
	int err, err_k[b->mmpm];

	/* if the block cannot be read at once, read it mesoblock by
	 * mesoblock, the ones that cannot be read are corrupt */
	if ((err = b->read(bi->dev->io, data, (((off_t)id)<<b->macroblock_log) +
			(1<<b->mesoblk_log), b->mmpm<<b->mesoblk_log))) {
		WARNING("error reading block %u of \"%s\": %s", id,
				bi->dev->name, strerror(errno));
		for (k = 0; k < b->mmpm; k++)
			err_k[k] = b->read(bi->dev->io,
					data + (k<<b->mesoblk_log),
					(((off_t)id)<<b->macroblock_log) +
					((k + 1)<<b->mesoblk_log),
					1<<b->mesoblk_log);
	}

	for (k = 0; k < b->mmpm; k++) {
		if (live && !live[k]) continue;
		if ((err && err_k[k]) ||
				(bi->hashes[k]&~(uint64_t)BLOCKIO_CORRUPT_MASK)
				!= mesoblk_hash(data + (k<<b->mesoblk_log),
					1<<b->mesoblk_log)) {
			mismatches++;
			if (live) live[k] = 2 + BLOCKIO_CORRUPT_MASK;
		} else if (live && bi->hashes[k]&BLOCKIO_CORRUPT_MASK)
			live[k] = 2 + (bi->hashes[k]&BLOCKIO_CORRUPT_MASK);
	}

	free(data);

	return mismatches;
}

void blockio_dev_write_current_macroblock(blockio_dev_t *dev) {
	uint32_t id = blockio_get_macroblock_index(dev->bi);
	int i;
	assert(dev->bi && id < dev->b->total_macroblocks);
	
	DEBUG("write block %u (seqno=%lu)", id, dev->bi->seqno);
	dev->writes++;

	/* the tombstones go in slots after the data */
	for (i = 0; i < dev->no_tombs; i += dev->tombs_per_mesoblk) {
		uint32_t k, n = dev->no_tombs - i;
		char *slot = BASE + ((dev->bi->no_indices + 1)<<
				dev->b->mesoblk_log);
		if (n > dev->tombs_per_mesoblk) n = dev->tombs_per_mesoblk;
		assert(dev->bi->no_indices < dev->b->mmpm);
		memset(slot, 0, 1<<dev->b->mesoblk_log);
		binio_write_uint32_be(slot, n);
		for (k = 0; k < n; k++)
			binio_write_uint32_be(slot + 4 + 4*k,
					dev->tombs[i + k]);
		dev->bi->indices[dev->bi->no_indices] = BLOCKIO_TOMBSTONES;
		dev->bi->hashes[dev->bi->no_indices] = 0;
		dev->bi->no_indices++;
	}
	if (dev->no_tombs) DEBUG("block %u has %u tombstones", id,
			dev->no_tombs);
	dev->no_tombs = 0;

	/* zero out the unused datablock,
	 * so that we do not encrypt 'random' data */
	for (i = dev->bi->no_indices + 1; i <= dev->b->mmpm; i++) {
		memset(BASE + (i<<dev->b->mesoblk_log),
				0, 1<<dev->b->mesoblk_log);
	}

	/* encrypt datablocks (also the unused ones), the plaintext
	 * stays in tmp_macroblock, store the hash of each ciphertext
	 * in the index, together with the corrupt parts of the mesoblocks
	 * (corrupt mesoblocks that were copied into this block stay marked
	 * as corrupt) */
	for (i = 1; i <= dev->b->mmpm; i++) {
		cipher_enc(dev->c, dev->out_macroblock +
				(i<<dev->b->mesoblk_log),
			BASE + (i<<dev->b->mesoblk_log),
			dev->bi->seqno, i, id);
		dev->bi->hashes[i-1] = mesoblk_hash(dev->out_macroblock +
				(i<<dev->b->mesoblk_log),
				1<<dev->b->mesoblk_log)|
			((i <= dev->bi->no_indices)?
			 (dev->bi->hashes[i-1]&BLOCKIO_CORRUPT_MASK):0);
		binio_write_uint64_be(HASH_UINT64(i-1), dev->bi->hashes[i-1]);
	}
	memset(HASH_UINT64(dev->b->mmpm), 0, 8); /* unused */
	memset(RESERVED32, 0, 32);

	/* write static data, also which block is written next (its live
	 * data is absorbed by this block) */
	binio_write_uint64_be(SEQNO_UINT64, dev->bi->seqno);
	memset(NEXT_BLOCK_UINT32, 0, 8);
	binio_write_uint32_be(NEXT_BLOCK_UINT32, dev->j.next?
			blockio_get_macroblock_index(dev->j.next):0xFFFFFFFF);
	memcpy(MAGIC64, magic, sizeof(magic));
	binio_write_uint32_be(RESERVED_BLOCKS_UINT32,
			dev->reserved_macroblocks);
	binio_write_uint32_be(NO_MACROBLOCKS_UINT32, dev->no_macroblocks);
	memset(RESERVED_SPACE1024, 0, 128);
	blockio_dev_prune_trims(dev);
	memset(TRIM_FLOOR_UINT64, 0, 32);
	binio_write_uint64_be(TRIM_FLOOR_UINT64, dev->trim_floor);
	binio_write_uint32_be(PENDING_UINT32, dev->pending_removals);
	binio_write_uint32_be(NO_TRIMS_UINT32, dev->no_trims);
	for (i = 0; i < dev->no_trims; i++) {
		binio_write_uint64_be(TRIM_SEQNO_UINT64(i), dev->trims[i].seqno);
		binio_write_uint32_be(TRIM_SIZE_UINT32(i), dev->trims[i].size);
	}

	/* the dead slots (discarded while in RAM) are obsolete */
	dev->bi->no_nonobsolete = 0;
	for (i = 0; i < dev->bi->no_indices; i++)
		if (dev->bi->indices[i] != BLOCKIO_DEAD)
			dev->bi->no_nonobsolete++;
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
	cipher_enc(dev->c, dev->out_macroblock, BASE, 0, 0, id);

	dev->b->write(dev->io, dev->out_macroblock,
			((off_t)id)<<dev->b->macroblock_log,
			1<<dev->b->macroblock_log);

	/* the block must be on disk before the next block is written: the
	 * next block may be the tail of this block, its live data is in
	 * this block now; this also means that only the most recently
	 * written block can be incomplete after a power failure */
	dev->b->sync(dev->io);

	/* the old index of this block is replaced by the new one */
	for (i = 0; i < dev->no_old_ondisk; i++)
		if (dev->old_ondisk[i] < dev->no_refcount) {
			assert(dev->refcount[dev->old_ondisk[i]]);
			dev->refcount[dev->old_ondisk[i]]--;
		}
	dev->no_old_ondisk = 0;
	dev->bi->no_ondisk = dev->bi->no_indices;
	blockio_dev_count_ondisk(dev, dev->bi, 1);
	dev->bi->readded = 0;

	/* removed blocks are not part of the partition on disk anymore */
	blockio_dev_release_removed(dev);

	dev->ram_block = dev->bi; /* its plaintext is still in RAM */
	if (dev->bi->dev != dev) dev->ram_block = NULL; /* was removed */
	dev->bi = NULL; /* there is no current block */
	dev->updated = 0;
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

int blockio_dev_free_macroblocks(blockio_dev_t *dev, uint32_t size) {
	FATAL("blockio_dev_free_macroblocks is not implemented");
}

void blockio_prepare_block(blockio_info_t *bi) {
	assert(bi->dev);
	bi->seqno = 0;
	bi->no_indices = bi->no_nonobsolete = bi->no_ondisk = 0;
	bi->readded = 0;
	blockio_info_alloc_tables(bi, bi->dev->b->mmpm);
}

/* a block that is added to the partition may still have an index of ours
 * on disk (it was removed by shrinking, or it was stale when the partition
 * was opened), it would be replayed if the partition is opened again before
 * the block is written: its indices count, and discarded mesoblocks in it
 * need tombstones (see scubed3_readded) */
static void blockio_dev_check_readded(blockio_dev_t *dev, blockio_info_t *bi) {
	uint32_t no = blockio_get_macroblock_index(bi), n, k;
	unsigned char *buf = ecalloc(1, 1<<dev->b->mesoblk_log);
	char sha256[32];
	uint64_t seqno;

	if (dev->b->read(dev->io, buf, ((off_t)no)<<dev->b->macroblock_log,
				1<<dev->b->mesoblk_log)) goto out;
	cipher_dec(dev->c, (char*)buf, (char*)buf, 0, 0, no);
	if (memcmp(magic, buf + 0x070, sizeof(magic))) goto out;
	gcry_md_hash_buffer(GCRY_MD_SHA256, sha256, buf + sizeof(sha256),
			(1<<dev->b->mesoblk_log) - sizeof(sha256));
	if (memcmp(buf, sha256, sizeof(sha256))) goto out;

	/* below the trim floor it is never replayed */
	seqno = binio_read_uint64_be(buf + 0x060);
	if (seqno < dev->trim_floor) goto out;

	n = binio_read_uint32_be(buf + 0x100);
	if (n > dev->b->mmpm) goto out;
	for (k = 0; k < n; k++)
		bi->indices[k] = binio_read_uint32_be(buf + 0x104 + 4*k);
	bi->no_ondisk = n;
	bi->readded = 1;
	blockio_dev_count_ondisk(dev, bi, 1);
	VERBOSE("block %u (seqno=%lu) added to \"%s\" still has %u "
			"indices of ours", no, seqno, dev->name, n);
out:
	wipememory(buf, 1<<dev->b->mesoblk_log);
	free(buf);
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
		blockio_dev_check_readded(dev, bi);

		dev->no_macroblocks++;

		juggler_add_macroblock(&dev->j, bi);
		blockio_dev_change_macroblock_status(bi, USED);
	}
	
	pthd_mutex_unlock(&dev->b->unallocated_mutex);

	return err;
}
