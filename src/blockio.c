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
#include <math.h>
#include <pthread.h>
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
#define NONCE_UINT64		(BASE + 0x020) /* followed by 24 zero bytes */
#define RESERVED24		(BASE + 0x028) /* MUST be 0 */
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
	free(b->devs);
	random_free(&b->r);
	pthd_mutex_destroy(&b->unallocated_mutex);
	pthd_mutex_destroy(&b->io_mutex);
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

	b->paranoia = 1;
	random_init(&b->r);
	pthd_mutex_init(&b->io_mutex);

	/* the clock of paranoia level 3 (CLOCK_MONOTONIC, see
	 * blockio_wait_tick) */
	if (!b->rate) b->rate = BLOCKIO_DEFAULT_RATE;
	pthd_mutex_init(&b->tick_mutex);
	{
		pthread_condattr_t attr;
		pthread_condattr_init(&attr);
		pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
		if (pthread_cond_init(&b->tick_cond, &attr))
			FATAL("unable to initialize condition variable");
		pthread_condattr_destroy(&attr);
	}
}

static const char magic[8] = "SSS3v0.5";

/* blocks of version 0.4 have no nonce (it is 0, the IV is the same as
 * before), they are still read; they become version 0.5 when they are
 * written again */
static const char magic_v04[8] = "SSS3v0.4";

static int magic_ok(const void *p) {
	return !memcmp(p, magic, sizeof(magic)) ||
		!memcmp(p, magic_v04, sizeof(magic_v04));
}

/* the IV of the data mesoblocks of a block is ESSIV(seqno^nonce, slot,
 * block): the seqno alone repeats with other data if the base device is
 * restored from a backup and used again, or if a torn write is redone (the
 * torn block is ignored, the write is redone in the same block with the
 * same seqno), and someone who has both versions would see the repeated
 * IVs; the nonce is random and new for every write, so the IVs do not
 * repeat (unless two random 64 bit nonces of one block happen to collide
 * with their seqnos) */
static uint64_t blockio_iv0(blockio_info_t *bi) {
	return bi->seqno^bi->nonce;
}

static void blockio_dev_take_index(blockio_dev_t*);

static void blockio_dev_read_index(blockio_dev_t*, blockio_info_t*);

static void blockio_dev_resume_wipe(blockio_dev_t*);

void blockio_dev_free(blockio_dev_t *dev) {
	int i;
	assert(dev);
	VERBOSE("closing \"%s\", %s", dev->name,
			dev->updated?"SHOULD BE WRITTEN":"no updates");
	if (dev->updated) blockio_dev_write_current_macroblock(dev);

	/* a removed partition that is not completely wiped is wiped now (in
	 * paranoia level 2 and 3 control refuses to close it, but scubed3 may
	 * be stopped) */
	if (dev->dying) {
		if (dev->b->paranoia >= 2) WARNING("\"%s\" is closed before it "
				"is wiped, wiping it at once", dev->name);
		blockio_dev_wipe_all(dev);
	}
	random_free(&dev->r);
	bitmap_free(&dev->status);

	/* the partition is not open anymore, its blocks look free to us:
	 * paranoia level 2 would overwrite them */
	for (i = 0; i < dev->b->no_devs; i++) if (dev->b->devs[i] == dev) {
		dev->b->devs[i] = dev->b->devs[--dev->b->no_devs];
		break;
	}
	if (dev->b->paranoia >= 2 && (dev->no_macroblocks ||
				dllarr_count(&dev->release))) {
		WARNING("\"%s\" is closed, paranoia level %d is switched off "
				"(level 1)", dev->name, dev->b->paranoia);
		blockio_set_paranoia(dev->b, 1, 0);
	}

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
	 * device for example), they are also in the pool of the juggler,
	 * unless opening failed before they were added to it */
	while ((bi = dllarr_last(&dev->replay))) {
		dllarr_remove(&dev->replay, bi);
		if (!juggler_contains(&dev->j, bi))
			dllarr_append(&dev->b->unallocated, bi);
	}

	/* re-add blocks to unallocated (removed blocks are not in the pool,
	 * the removal is on disk: the current block was written above, and
	 * scubed3_free makes sure that it is not its own next block) */
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

	if (dev->self_tail || dev->final_pending) return 0;

	used = dev->bi->no_indices + (dev->no_tombs + dev->tombs_per_mesoblk -
			1)/dev->tombs_per_mesoblk;
	assert(used <= dev->b->mmpm);

	return dev->b->mmpm - used;
}

/* do no more tombstones fit in the current block? */
int blockio_dev_tomb_fits(blockio_dev_t *dev, uint32_t no) {
	assert(dev->bi);

	if (dev->self_tail || dev->final_pending) return 0;

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
		/* while shrinking to 0, see blockio_dev_select_next_macroblock */
		if (bi == dev->zero[0] || bi == dev->zero[1]) continue;
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

/* after the removals are on disk, the blocks can be given away, this is
 * called after a write (that records the removals); a block that was removed
 * after the next block was drawn is kept until the next block is drawn
 * without it (until then the write that comes after the next write may
 * land on it, see juggler.h; its live data is in the current block, so it
 * would be needed if the next write is interrupted, with the next write it
 * is on disk); the write that recorded the removal is not rewritten
 * by the next write: a block that is its own next block takes no data and
 * removes nothing (see blockio_dev_select_next_macroblock and
 * copy_old_block_to_current), so the state never falls back to a state in
 * which a released block is still used */
static void blockio_dev_release_removed(blockio_dev_t *dev) {
	blockio_info_t *bi, *next;

	pthd_mutex_lock(&dev->b->unallocated_mutex);
	for (bi = dllarr_first(&dev->release); bi; bi = next) {
		next = dllarr_next(&dev->release, bi);
		if (dev->j.next && bi->removed >= dev->j.draws) continue;
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
	dev->j.greedy = !dev->b->paranoia;
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

	/* all blocks are removed */
	if (!juggler_count(&dev->j)) return;

	/* removing the last blocks of the partition (it shrinks to 0): the
	 * last blocks are the current and the next block (and the blocks in
	 * zero, see below), they cannot be removed one by one (see
	 * blockio_dev_find_victim), so when none of the remaining blocks has
	 * live data they are removed at once; the current block gets one more
	 * write (in its turn), in which they are FREE, so that the removal is
	 * on disk; afterwards there is no current block, and the partition is
	 * wiped (see blockio_dev_start_wipe); in paranoia level 2 and 3 the
	 * write waits for its turn (it is done by the scheduler, this
	 * function is called after a write, and one turn is one write);
	 *
	 * the block before the final write must also say that the size is 0
	 * (prelast, it is kept until the end of the wipe: if the final write,
	 * or a later write of the last index, is interrupted, the state falls
	 * back to it; an older state would bring the partition back with its
	 * data), so one of the two blocks in zero must not be the current
	 * block, otherwise there is one more write first */
	victim = NULL;
	if (dev->pending_removals &&
			dev->pending_removals == juggler_count(&dev->j)) {
		for (i = 0; i < juggler_count(&dev->j); i++)
			if (juggler_nth(&dev->j, i)->no_nonobsolete) break;
	} else i = 0;
	if (dev->pending_removals && i == juggler_count(&dev->j)) {
		victim = dev->zero[0] != dev->j.next?dev->zero[0]:dev->zero[1];
		if (victim == dev->j.next) victim = NULL;
		if (!victim && juggler_count(&dev->j) > 1) i = 0;
	}
	if (dev->pending_removals && i == juggler_count(&dev->j)) {
		dev->prelast = victim; /* may be NULL with one block */
		dev->final_write = 1;
		blockio_dev_take_current(dev);
		juggler_set_next(&dev->j, NULL);
		VERBOSE("removing the last block %u",
				blockio_get_macroblock_index(dev->bi));
		dev->bi->no_indices = 0;
		while (juggler_count(&dev->j) > 1) {
			victim = juggler_nth(&dev->j, 0);
			if (victim == dev->bi) victim = juggler_nth(&dev->j, 1);
			blockio_dev_remove_block(dev, victim);
		}
		blockio_dev_remove_block(dev, dev->bi);
		if (dev->b->paranoia >= 2) dev->final_pending = 1;
		else blockio_dev_write_current_macroblock(dev);
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
	 * go when it is rewritten, so it is written out empty first (this
	 * happens with probability 1/N), that is the next write of this
	 * partition: until then the block takes no data (see free_slots), so
	 * that a write is needed (one write per turn in paranoia level 2);
	 * if the partition has only one block, it is always its own tail,
	 * and there is no other block to absorb, so it can keep its data */
	if (dev->tail_macroblock == dev->bi && !dev->bi->no_indices &&
			juggler_count(&dev->j) > 1) {
		VERBOSE("block %d is drawn twice, writing it out empty "
				"(pool=%u)", blockio_get_macroblock_index(dev->bi),
				juggler_count(&dev->j));
		dev->self_tail = 1;
		return;
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
	blockio_info_t *bi, *next, *prev;
	bitmap_t prev_status;
	char err[256] = "";
	int have_prev = 0;

	assert(b && c);
	assert(b->mesoblk_log < b->macroblock_log);
	random_init(&dev->r);
	dllarr_init(&dev->replay, offsetof(blockio_info_t, ur));
	dllarr_init(&dev->release, offsetof(blockio_info_t, ur));
	dllarr_init(&dev->wipe, offsetof(blockio_info_t, ur));
	bitmap_init(&dev->status, b->max_macroblocks);
	juggler_init(&dev->j, &dev->r);

	dev->b = b;
	dev->c = c;

	/* the scheduler of paranoia level 2 needs all open partitions */
	b->devs = erealloc(b->devs, b->no_devs + 1, sizeof(blockio_dev_t*));
	b->devs[b->no_devs++] = dev;
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

	/* the blocks that the most recent block removed are not yet
	 * released (see blockio_dev_release_removed), they are released
	 * after the next write; they may still be needed if the next write is
	 * interrupted and the most recent block is also written next (in older
	 * versions, a block that was its own next block could remove blocks) */
	if (next_block != 0xFFFFFFFF &&
			(prev = dllarr_prev(&dev->replay, bi))) {
		bitmap_init(&prev_status, b->max_macroblocks);
		blockio_dev_read_index(dev, prev);
		bitmap_read(&prev_status, (uint32_t*)BITMAP);
		have_prev = 1;
	}

	blockio_dev_read_index(dev, bi);
	blockio_dev_take_index(dev);

	/* the most recent index says that the partition has no blocks: it
	 * was removed, and its wipe was interrupted */
	if (!dev->no_macroblocks) {
		if (have_prev) bitmap_free(&prev_status);
		dev->j.seqno = seqno;
		blockio_dev_resume_wipe(dev);
		return;
	}

	/* blocks with our signature that are FREE in the most recent bitmap
	 * are not part of the partition (they were removed), blocks with a
	 * seqno below the trim floor were removed and added again, they are
	 * treated as fresh blocks (the loop below adds them) */
	pthd_mutex_lock(&b->unallocated_mutex);

	bi = dllarr_first(&dev->replay);
	while (bi) {
		next = dllarr_next(&dev->replay, bi);
		if (have_prev && bi->seqno >= dev->trim_floor &&
				blockio_dev_get_macroblock_status(bi) == FREE &&
				bitmap_getbit(&prev_status,
					blockio_get_macroblock_index(bi))) {
			VERBOSE("block %u (seqno=%lu) is removed, but not yet "
					"released", blockio_get_macroblock_index(bi),
					bi->seqno);
			dllarr_remove(&dev->replay, bi);
			dllarr_append(&dev->release, bi);
		} else if (blockio_dev_get_macroblock_status(bi) == FREE ||
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
	if (have_prev) bitmap_free(&prev_status);

	/* the juggler continues after the most recent block */
	dev->j.seqno = seqno;

	VERBOSE("we currently got %d blocks, and we expect %d blocks",
			dllarr_count(&dev->replay), dev->no_macroblocks);

	/* if the state of the partition does not fit, it is not opened (the
	 * other partitions are not affected, the blocks that we took are
	 * given back by blockio_dev_free); this happens if the most recent
	 * index of the partition was overwritten, and an old one, from before
	 * it gave blocks away, is used (for example after it was removed) */
	pthd_mutex_lock(&b->unallocated_mutex);

	for (i = 0; i < dev->b->max_macroblocks && !*err; i++) {
		if (blockio_dev_get_macroblock_status_bynum(dev, i) == USED) {
			if (i >= dev->b->total_macroblocks) {
				snprintf(err, sizeof(err), "block %u is used "
						"but it does not exist", i);
				break;
			}

			bi = &dev->b->blockio_infos[i];
			if (bi->dev == dev) continue; // ok
			else if (bi->dev) {
				snprintf(err, sizeof(err), "block %u is used, "
						"but it belongs to partition "
						"\"%s\"", i, bi->dev->name);
				break;
			}

			/* we don't know what it contains, if it has
			 * data, adding it as a fresh block would lose
			 * that data silently */
			if (bi->unreadable) {
				snprintf(err, sizeof(err), "block %u cannot be "
						"read", i);
				break;
			}

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

	if (!*err && dev->no_macroblocks != juggler_count(&dev->j))
		snprintf(err, sizeof(err), "count of macroblocks does not "
				"match the amount of blocks we have");

	if (!*err && next_block != 0xFFFFFFFF &&
			(next_block >= dev->b->total_macroblocks ||
			 dev->b->blockio_infos[next_block].dev != dev))
		snprintf(err, sizeof(err), "next block %u is not part of the "
				"partition", next_block);

	if (*err) {
		dev->no_macroblocks = 0;
		ecch_throw(ECCH_DEFAULT, "\"%s\" is not opened: %s",
				dev->name, err);
	}

	/* the most recent block says which block is written next, its
	 * live data is in the most recent block */
	if (next_block != 0xFFFFFFFF) {
		juggler_drawn_next(&dev->j, &dev->b->blockio_infos[next_block]);

		/* the blocks that are removed but not released are in the
		 * draw set of the next block */
		for (bi = dllarr_first(&dev->release); bi;
				bi = dllarr_next(&dev->release, bi)) {
			bi->added = 0;
			bi->removed = dev->j.draws;
			dev->j.next_pool++;
		}
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
	if (dev->pending_removals > dev->no_macroblocks) {
		dev->no_macroblocks = dev->pending_removals = 0;
		ecch_throw(ECCH_DEFAULT, "\"%s\" is not opened: more pending "
				"removals than blocks", dev->name);
	}
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

	/* this is only done while the partition is opened, if it fails, it
	 * is not opened (see blockio_dev_init) */
	if (dev->b->read(dev->io, BASE, ((off_t)no)<<dev->b->macroblock_log,
			1<<dev->b->mesoblk_log)) {
		dev->no_macroblocks = 0;
		ecch_throw(ECCH_DEFAULT, "\"%s\" is not opened: error reading "
				"the index of block %u: %s", dev->name, no,
				strerror(errno));
	}
	cipher_dec(dev->c, BASE, BASE, 0, 0, no);

	gcry_md_hash_buffer(GCRY_MD_SHA256, sha256, BASE + sizeof(sha256),
			(1<<dev->b->mesoblk_log) - sizeof(sha256));
	if (!magic_ok(MAGIC64) ||
			memcmp(INDEXBLOCK_SHA256, sha256, sizeof(sha256))) {
		dev->no_macroblocks = 0;
		ecch_throw(ECCH_DEFAULT, "\"%s\" is not opened: the index of "
				"block %u changed while opening", dev->name, no);
	}
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
	if (!magic_ok(MAGIC64)) {
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
	bi->nonce = memcmp(MAGIC64, magic_v04, sizeof(magic_v04))?
		binio_read_uint64_be(NONCE_UINT64):0;
	bi->next_block = binio_read_uint32_be(NEXT_BLOCK_UINT32);

	no_trims = binio_read_uint32_be(NO_TRIMS_UINT32);
	if (no_trims > BLOCKIO_MAX_TRIMS)
		FATAL("too many trims in index of block %u", no);
	if (memcmp(zero, TRIMS + 12*no_trims, 128 - 8 - 12*no_trims))
		FATAL("reserved space is not zeroed out");
	if (bi->seqno == 0) 
		FATAL("block found with seqno 0, not possible");

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
	cipher_dec(dev->c, buf, buf, blockio_iv0(bi), no + 1, id);

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

/* the number of blocks of the partition for the scheduler: the size of the
 * draw set of its current block, the write lands uniformly in it (also
 * the final write of its last block, the draw set of that block is the
 * block itself and the blocks that are removed but not released) */
uint32_t blockio_dev_weight(blockio_dev_t *dev) {
	return dev->bi?dev->j.cur_pool:0;
}

/* the blocks of an open partition that are not in the draw set of its
 * current block: blocks that were added after it was drawn (they may
 * already be in the draw set of the next block), the partition did not
 * write anything to them, so they are free for the scheduler (also if they
 * are removed again: no other partition gets them before the removal is on
 * disk, see blockio_dev_release_removed); returns fresh block n, the number
 * of fresh blocks is put in fresh and the number of blocks in the draw set
 * in pool (that must be the weight of the partition) */
static blockio_info_t *nth_fresh_block(blockio_dev_t *dev, uint32_t n,
		uint32_t *fresh, uint32_t *pool) {
	blockio_info_t *bi, *ret = NULL;
	uint32_t i;

	*fresh = *pool = 0;

	/* the blocks of a removed partition that are wiped (it has no draw
	 * set), see blockio_dev_wipe_step */
	if (dev->dying) {
		for (bi = dllarr_first(&dev->wipe); bi;
				bi = dllarr_next(&dev->wipe, bi))
			if ((*fresh)++ == n) ret = bi;
		if (dev->prelast && (*fresh)++ == n) ret = dev->prelast;
		if (dev->last && (*fresh)++ == n) ret = dev->last;
		return ret;
	}

	if (!dev->bi) return NULL; /* not yet started, or no blocks */

	for (i = 0; i < juggler_count(&dev->j); i++) {
		bi = juggler_nth(&dev->j, i);
		if (juggler_in_current(&dev->j, bi)) (*pool)++;
		else if ((*fresh)++ == n) ret = bi;
	}

	for (bi = dllarr_first(&dev->release); bi;
			bi = dllarr_next(&dev->release, bi)) {
		if (juggler_in_current(&dev->j, bi)) (*pool)++;
		else if ((*fresh)++ == n) ret = bi;
	}

	return ret;
}

/* the blocks that are free for the scheduler: the unallocated blocks and
 * the fresh blocks of the open partitions (see nth_fresh_block), returns
 * free block n and puts the number of free blocks in count; the draw sets
 * of the current blocks of the open partitions and the free blocks are all
 * blocks of the base device, except the blocks of partitions that are not
 * open (the scheduler does not know about them); a block that is removed
 * from a partition stays in a draw set until it is released, then it is
 * unallocated */
static blockio_info_t *nth_free_block(blockio_t *b, uint32_t n,
		uint32_t *count) {
	blockio_info_t *bi, *ret = NULL;
	uint32_t i, fresh, pool;

	*count = dllarr_count(&b->unallocated);
	if (n < *count) ret = dllarr_nth(&b->unallocated, n);

	for (i = 0; i < b->no_devs; i++) {
		bi = nth_fresh_block(b->devs[i], n - *count, &fresh, &pool);
		if (n >= *count && !ret) ret = bi;
		assert(pool == blockio_dev_weight(b->devs[i]));
		*count += fresh;
	}

	return ret;
}

/* write macroblock id (buf is the whole macroblock, the index is its first
 * mesoblock): the data mesoblocks first, then the index, each followed by a
 * sync, so the index is only on disk when its data is; an interrupted
 * write leaves either the old index (with data that does not match its
 * hashes: the block is ignored, its live data is in the most recent block)
 * or the new index with all of its data; a wipe (random data, see
 * blockio_write_random) that is interrupted leaves the old index if any of
 * the old data is left, so the block is found again when the wipe is
 * resumed (see blockio_dev_resume_wipe); every block is written like this,
 * so a wipe looks the same as other writes */
static void blockio_write_block(blockio_t *b, void *io, uint32_t id,
		const char *buf) {
	off_t offset = ((off_t)id)<<b->macroblock_log;
	uint32_t meso = 1<<b->mesoblk_log;
#ifdef SCUBED3_CRASH_TEST
	/* testing only: log the offset of every block write (and the time
	 * on CLOCK_MONOTONIC if SCUBED3_WRITE_LOG_TIME is set) */
	static FILE *wlog = NULL;
	if (!wlog && getenv("SCUBED3_WRITE_LOG"))
		wlog = fopen(getenv("SCUBED3_WRITE_LOG"), "a");
	if (wlog) {
		struct timespec now;
		if (getenv("SCUBED3_WRITE_LOG_TIME")) {
			clock_gettime(CLOCK_MONOTONIC, &now);
			fprintf(wlog, "%lu %ld.%09ld\n", (unsigned long)offset,
					(long)now.tv_sec, now.tv_nsec);
		} else fprintf(wlog, "%lu\n", (unsigned long)offset);
		fflush(wlog);
	}
#endif

	b->write(io, buf + meso, offset + meso, b->macroblock_size - meso);
	b->sync(io);
	b->write(io, buf, offset, meso);
	b->sync(io);
}

/* paranoia level 2: overwrite a free block with random data, it looks the
 * same as a block of a partition (with io: the io of the partition that
 * waits for its turn, or of the clock thread) */
static void blockio_write_random(blockio_t *b, void *io, blockio_info_t *bi) {
	uint32_t id = bi - b->blockio_infos;
	char *buf = ecalloc(1, 1<<b->macroblock_log);

	DEBUG("write random data to free block %u", id);
	if (fread(buf, 1<<b->macroblock_log, 1, b->r.fp) != 1)
		FATAL("reading from /dev/urandom: %s", strerror(errno));
	blockio_write_block(b, io, id, buf);
	b->random_writes++;
	free(buf);
}

/* a wiped block (or a block that is not needed anymore) of a partition that
 * is removed is given to the unallocated blocks */
static void blockio_dev_release_wiped(blockio_dev_t *dev, blockio_info_t *bi) {
	pthd_mutex_lock(&dev->b->unallocated_mutex);
	if (bi->wipe) dllarr_remove(&dev->wipe, bi);
	bi->wipe = 0;
	if (bi == dev->last) dev->last = NULL;
	if (bi == dev->prelast) dev->prelast = NULL;
	blockio_info_free_tables(bi);
	bi->dev = NULL;
	bi->seqno = 0;
	dllarr_append(&dev->b->unallocated, bi);
	pthd_mutex_unlock(&dev->b->unallocated_mutex);
}

/* does the (free) block have an index of this partition? */
static int blockio_dev_has_index(blockio_dev_t *dev, blockio_info_t *bi) {
	uint32_t no = bi - dev->b->blockio_infos;
	unsigned char *buf = ecalloc(1, 1<<dev->b->mesoblk_log);
	char sha256[32];
	int ret = 0;

	if (dev->b->read(dev->io, buf, ((off_t)no)<<dev->b->macroblock_log,
				1<<dev->b->mesoblk_log)) goto out;
	cipher_dec(dev->c, (char*)buf, (char*)buf, 0, 0, no);
	if (!magic_ok(buf + 0x070)) goto out;
	gcry_md_hash_buffer(GCRY_MD_SHA256, sha256, buf + sizeof(sha256),
			(1<<dev->b->mesoblk_log) - sizeof(sha256));
	ret = !memcmp(buf, sha256, sizeof(sha256));
out:
	wipememory(buf, 1<<dev->b->mesoblk_log);
	free(buf);
	return ret;
}

/* the partition is removed: its last index (the final write, in dev->bi)
 * says that it has no blocks; but blocks that still have an older index of
 * it would bring it back (with its data) if that index is the most recent
 * one that is left, when the partition is opened again: they are wiped
 * (random data), the blocks that it removed, and the free blocks with an
 * old index of it (from an earlier shrink, or stale when it was opened);
 * the last two indices (last and prelast, both say that the size is 0)
 * are kept until the others are wiped, then they are wiped too; in
 * paranoia level 0 and 1 this is done at once, in level 2 and 3 the
 * blocks are wiped when the scheduler draws them (see
 * blockio_dev_wipe_step), so the writes stay uniform; until then the
 * partition stays open, and its key cannot be used by another partition;
 * the caller sets last (and prelast) and wipes at once in level 0 and 1 */
static void blockio_dev_start_wipe(blockio_dev_t *dev) {
	blockio_t *b = dev->b;
	blockio_info_t *bi, *next;
	uint32_t i, found = 0;

	pthd_mutex_lock(&b->unallocated_mutex);
	for (bi = dllarr_first(&dev->release); bi; bi = next) {
		next = dllarr_next(&dev->release, bi);
		dllarr_remove(&dev->release, bi);
		if (bi == dev->last || bi == dev->prelast) continue;
		bi->wipe = 1;
		dllarr_append(&dev->wipe, bi);
	}
	for (i = 0; i < b->total_macroblocks; i++) {
		bi = &b->blockio_infos[i];
		if (bi->dev || !blockio_dev_has_index(dev, bi)) continue;
		dllarr_remove(&b->unallocated, bi);
		bi->dev = dev;
		bi->wipe = 1;
		dllarr_append(&dev->wipe, bi);
		found++;
	}
	pthd_mutex_unlock(&b->unallocated_mutex);

	dev->dying = 1;
	dev->removed = 1;
	VERBOSE("\"%s\" is removed, %u blocks are wiped (%u free blocks had "
			"an old index of it)", dev->name, dllarr_count(&dev->wipe) +
			(dev->last?1:0) + (dev->prelast?1:0), found);
}

/* the most recent index of the partition says that it has no blocks: it
 * was removed, and scubed3 was stopped (or crashed) before all blocks with
 * an index of it were wiped (see blockio_dev_start_wipe), the wipe is
 * resumed; the most recent block is last, the block before it is prelast if
 * its index also says that the partition is empty (all its blocks are
 * pending removals), the other blocks with an index of it are free now,
 * they are found by the scan of blockio_dev_start_wipe; without prelast the
 * first block of the wipe that is drawn becomes prelast (see
 * blockio_dev_wipe_step); this only works as long as last or prelast is
 * left: if both are overwritten (by another partition, before the removed
 * partition is opened again), an older index is the most recent one */
static void blockio_dev_resume_wipe(blockio_dev_t *dev) {
	blockio_t *b = dev->b;
	blockio_info_t *bi, *last, *prelast;

	last = dllarr_last(&dev->replay);
	prelast = dllarr_prev(&dev->replay, last);
	if (prelast) {
		blockio_dev_read_index(dev, prelast);
		if (binio_read_uint32_be(PENDING_UINT32) !=
				binio_read_uint32_be(NO_MACROBLOCKS_UINT32))
			prelast = NULL;
	}

	/* last and prelast stay ours, the other blocks are free */
	pthd_mutex_lock(&b->unallocated_mutex);
	while ((bi = dllarr_first(&dev->replay))) {
		dllarr_remove(&dev->replay, bi);
		if (bi == last || bi == prelast) continue;
		blockio_info_free_tables(bi);
		bi->dev = NULL;
		bi->seqno = 0;
		dllarr_append(&b->unallocated, bi);
	}
	pthd_mutex_unlock(&b->unallocated_mutex);

	dev->no_macroblocks = dev->reserved_macroblocks = 0;
	dev->pending_removals = 0;
	dev->last = last;
	dev->prelast = prelast;
	blockio_dev_start_wipe(dev);
	dev->resumed = blockio_dev_wipe_left(dev);
	WARNING("\"%s\" was removed, but it is not completely wiped: the "
			"wipe of its %u remaining blocks is resumed", dev->name,
			dev->resumed);

	if (b->paranoia < 2) blockio_dev_wipe_all(dev);
}

/* the most recent index of a removed partition is written (again) to bi:
 * it says that the partition has no blocks */
static void blockio_dev_write_marker(blockio_dev_t *dev, blockio_info_t *bi) {
	assert(!dev->bi && bi->dev == dev && !dev->no_macroblocks);
	if (!bi->indices) blockio_info_alloc_tables(bi, dev->b->mmpm);
	bi->no_indices = 0;
	bi->seqno = ++dev->j.seqno;
	dev->bi = bi;
	dev->no_old_ondisk = 0;
	dev->turn = 1; /* this is the turn */
	blockio_dev_write_current_macroblock(dev);
}

/* the scheduler (paranoia level 2 or 3) drew block bi of a removed
 * partition (see blockio_dev_start_wipe): a block in wipe is overwritten
 * with random data; if it is last, its index is written again, if it is
 * prelast, the index is written there, then it is last (so there are
 * always two indices that say that the partition is empty, if a write is
 * interrupted, the other one is left); when only last and prelast are
 * left, they are wiped in the order in which they are drawn */
static void blockio_dev_wipe_step(blockio_dev_t *dev, blockio_info_t *bi,
		void *io) {
	/* only one index says that the partition is empty (no prelast
	 * could be chosen, see blockio_dev_select_next_macroblock, or the
	 * wipe was resumed without it): the first block of
	 * the wipe that is drawn gets such an index too, and becomes last; if
	 * last itself is drawn first, its index is written again in place */
	if (bi->wipe && !dev->prelast) {
		pthd_mutex_lock(&dev->b->unallocated_mutex);
		dllarr_remove(&dev->wipe, bi);
		bi->wipe = 0;
		pthd_mutex_unlock(&dev->b->unallocated_mutex);
		blockio_dev_write_marker(dev, bi);
		dev->prelast = dev->last;
		dev->last = bi;
		return;
	}

	if (bi->wipe || !dllarr_count(&dev->wipe)) {
		blockio_write_random(dev->b, io, bi);
		blockio_dev_release_wiped(dev, bi);
		if (!dev->last && dev->prelast) {
			dev->last = dev->prelast;
			dev->prelast = NULL;
		}
		if (!dev->last) {
			dev->dying = 0;
			VERBOSE("\"%s\" is wiped", dev->name);
		}
		return;
	}

	blockio_dev_write_marker(dev, bi);
	if (bi == dev->prelast) {
		dev->prelast = dev->last;
		dev->last = bi;
	}
}

/* the number of blocks of a removed partition that are not wiped yet (in
 * paranoia level 2 and 3, when it waits for its final write, all its
 * blocks) */
uint32_t blockio_dev_wipe_left(blockio_dev_t *dev) {
	if (dev->dying) return dllarr_count(&dev->wipe) + (dev->last?1:0) +
		(dev->prelast?1:0);
	if (dev->final_write) return dllarr_count(&dev->release);
	return 0;
}

/* wipe a removed partition at once (paranoia level 0 and 1, or when it is
 * closed) */
void blockio_dev_wipe_all(blockio_dev_t *dev) {
	blockio_info_t *bi;

	if (!dev->dying) return;
	while ((bi = dllarr_first(&dev->wipe)))
		blockio_dev_wipe_step(dev, bi, dev->io);
	while (dev->dying) blockio_dev_wipe_step(dev, dev->prelast?
			dev->prelast:dev->last, dev->io);
}

/* the paranoia level is below 2: removed partitions are wiped at once (the
 * caller holds io_mutex) */
void blockio_finish_wipes(blockio_t *b) {
	uint32_t i;

	if (b->paranoia >= 2) return;
	for (i = 0; i < b->no_devs; i++) blockio_dev_wipe_all(b->devs[i]);
}

/* paranoia level 2: one draw of the scheduler, a write goes to a uniformly
 * random block of the whole base device: with probability proportional to
 * its weight an open partition (it writes its current block, which is
 * uniform over its draw set, see blockio_dev_weight), or the free space (a
 * uniformly random free block is overwritten with random data, with io);
 * returns the partition that must write, or NULL if the free space was
 * written; the caller holds io_mutex */
static blockio_dev_t *blockio_draw(blockio_t *b, void *io) {
	uint32_t i, total, no_free, r;
	blockio_info_t *bi;

	pthd_mutex_lock(&b->unallocated_mutex);
	nth_free_block(b, UINT32_MAX, &no_free);
	total = no_free;
	for (i = 0; i < b->no_devs; i++)
		total += blockio_dev_weight(b->devs[i]);
	if (!total) { /* only possible for the clock thread */
		pthd_mutex_unlock(&b->unallocated_mutex);
		return NULL;
	}
	r = random_custom(&b->r, total);

	for (i = 0; i < b->no_devs; i++) {
		if (r < blockio_dev_weight(b->devs[i])) break;
		r -= blockio_dev_weight(b->devs[i]);
	}

	if (i == b->no_devs) { /* the free space */
		bi = nth_free_block(b, r, &no_free);
		assert(bi);
		pthd_mutex_unlock(&b->unallocated_mutex);
		if (bi->dev && bi->dev->dying)
			blockio_dev_wipe_step(bi->dev, bi, io);
		else blockio_write_random(b, io, bi);
		return NULL;
	}
	pthd_mutex_unlock(&b->unallocated_mutex);

	return b->devs[i];
}

/* let partition dev write one block (it is drawn, and it is not the
 * partition that is waiting for its turn) */
static void blockio_give_turn(blockio_t *b, blockio_dev_t *dev) {
	dev->turn = 1;
	b->step(dev);
	if (dev->turn) {
		WARNING("partition \"%s\" did not write in its turn", dev->name);
		dev->turn = 0;
	}
}

/* paranoia level 3: the writes are done at random moments, independent of
 * what is written: a Poisson process with on average b->rate bytes per
 * second (one block write per tick), so the time to the next tick is
 * exponentially distributed; the ticks are counted on CLOCK_MONOTONIC (it
 * does not run while the computer sleeps); next_tick is protected by
 * tick_mutex, it is the innermost lock */
static double blockio_tick_interval(blockio_t *b) {
	return (double)b->macroblock_size/b->rate;
}

static void blockio_next_tick(blockio_t *b) {
	double u = (random_uint32(&b->r) + 1.)/4294967296.; /* (0, 1] */
	double d = -log(u)*blockio_tick_interval(b);

	b->next_tick.tv_sec += (time_t)d;
	b->next_tick.tv_nsec += (long)((d - (time_t)d)*1e9);
	if (b->next_tick.tv_nsec >= 1000000000) {
		b->next_tick.tv_nsec -= 1000000000;
		b->next_tick.tv_sec++;
	}
}

/* is a tick due? if so, it is taken (and the next tick is drawn); if the
 * ticks are far behind (writes that took long, or a long control command),
 * they are not done all at once, the clock starts again now; the caller
 * holds tick_mutex */
static int blockio_take_tick(blockio_t *b) {
	struct timespec now;
	double late;

	clock_gettime(CLOCK_MONOTONIC, &now);
	late = (now.tv_sec - b->next_tick.tv_sec) +
		(now.tv_nsec - b->next_tick.tv_nsec)/1e9;
	if (late < 0) return 0;

	if (late > 4*blockio_tick_interval(b) + 1) {
		VERBOSE("paranoia level 3: %.1fs behind, starting again",
				late);
		b->next_tick = now;
	}
	blockio_next_tick(b);
	b->ticks++;

	return 1;
}

/* wait for the next tick and take it, returns 0 (without a tick) if the
 * paranoia level is not 3 anymore; the caller holds io_mutex (nobody else
 * can take the tick), and has to do one draw */
static int blockio_wait_tick(blockio_t *b) {
	int ret;

	pthd_mutex_lock(&b->tick_mutex);
	while (b->paranoia == 3 && !b->clock_stop &&
			!blockio_take_tick(b))
		pthread_cond_timedwait(&b->tick_cond, &b->tick_mutex,
				&b->next_tick);
	ret = b->paranoia == 3 && !b->clock_stop;
	pthd_mutex_unlock(&b->tick_mutex);

	return ret;
}

/* is a tick due? (it is not taken) */
static int blockio_tick_due(blockio_t *b) {
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return now.tv_sec > b->next_tick.tv_sec ||
		(now.tv_sec == b->next_tick.tv_sec &&
		 now.tv_nsec >= b->next_tick.tv_nsec);
}

/* the clock thread: it does the ticks while no partition is waiting for its
 * turn; a partition that waits holds io_mutex and takes the ticks itself, so
 * a tick is only taken by the one that holds io_mutex (if the clock thread
 * would take it first, and wait for io_mutex, the write would come too
 * late: when the waiting partition is done) */
static void *blockio_clock(void *arg) {
	blockio_t *b = arg;
	blockio_dev_t *dev;

	pthd_mutex_lock(&b->tick_mutex);
	while (!b->clock_stop) {
		if (b->paranoia != 3) {
			pthd_cond_wait(&b->tick_cond, &b->tick_mutex);
			continue;
		}
		if (!blockio_tick_due(b)) {
			pthread_cond_timedwait(&b->tick_cond, &b->tick_mutex,
					&b->next_tick);
			continue;
		}

		/* a tick is due, get io_mutex first, the others wait
		 * until we have it (see blockio_io_lock) */
		b->tick_waiting = 1;
		pthd_mutex_unlock(&b->tick_mutex);
		pthd_mutex_lock(&b->io_mutex);
		pthd_mutex_lock(&b->tick_mutex);
		b->tick_waiting = 0;
		pthread_cond_broadcast(&b->tick_cond);

		/* do the ticks that are due (unless a waiting partition did
		 * them while we waited for io_mutex) */
		while (b->paranoia == 3 && !b->clock_stop && b->no_devs &&
				blockio_take_tick(b)) {
			pthd_mutex_unlock(&b->tick_mutex);
			if ((dev = blockio_draw(b, b->clock_io)))
				blockio_give_turn(b, dev);
			pthd_mutex_lock(&b->tick_mutex);
		}
		pthd_mutex_unlock(&b->tick_mutex);
		pthd_mutex_unlock(&b->io_mutex);
		pthd_mutex_lock(&b->tick_mutex);
	}
	pthd_mutex_unlock(&b->tick_mutex);

	return NULL;
}

/* io_mutex for everything that may write (see blockio_t); the clock thread
 * goes first: when a tick is due, it waits for io_mutex, and nobody else
 * may take it before the clock thread had it (the mutex is not fair, a
 * busy writer would take it again and again, and the writes of the ticks
 * would come late, when the writer is done: then the times of the writes
 * would show that something is written) */
void blockio_io_lock(blockio_t *b) {
	pthd_mutex_lock(&b->tick_mutex);
	while (b->tick_waiting)
		pthd_cond_wait(&b->tick_cond, &b->tick_mutex);
	pthd_mutex_unlock(&b->tick_mutex);
	pthd_mutex_lock(&b->io_mutex);
}

void blockio_io_unlock(blockio_t *b) {
	pthd_mutex_unlock(&b->io_mutex);
}

void blockio_clock_start(blockio_t *b) {
	int err;

	/* the clock thread writes free blocks with its own io */
	b->clock_io = (b->open)(b->open_priv);
	if ((err = pthread_create(&b->clock_thread, NULL, blockio_clock, b)))
		FATAL("unable to start the clock thread: %s", strerror(err));
	b->clock_running = 1;
}

/* stop the clock thread (it may be writing, so this is done before the
 * partitions are closed) */
void blockio_clock_stop(blockio_t *b) {
	if (!b->clock_running) return;

	/* without the clock there is no level 3 (and the partitions are
	 * closed next, see freer in fuse_io.c) */
	pthd_mutex_lock(&b->tick_mutex);
	b->clock_stop = 1;
	if (b->paranoia == 3) b->paranoia = 1;
	pthread_cond_broadcast(&b->tick_cond);
	pthd_mutex_unlock(&b->tick_mutex);

	pthread_join(b->clock_thread, NULL);
	b->clock_running = 0;
	if (b->close) (b->close)(b->clock_io);
	b->clock_io = NULL;
}

/* set the paranoia level (and the rate of level 3, if rate is not 0); this
 * is done without io_mutex: a partition that waits for its turn in level 3
 * holds it, and must be woken up if the level changes */
void blockio_set_paranoia(blockio_t *b, int level, uint64_t rate) {
	pthd_mutex_lock(&b->tick_mutex);
	if (rate) b->rate = rate;
	if (level == 3 && (b->paranoia != 3 || rate)) {
		clock_gettime(CLOCK_MONOTONIC, &b->next_tick);
		blockio_next_tick(b);
	}
	if (b->paranoia != level) VERBOSE("paranoia level %d", level);
	b->paranoia = level;
	pthread_cond_broadcast(&b->tick_cond);
	pthd_mutex_unlock(&b->tick_mutex);
}

/* paranoia level 2 and 3: every write goes to a uniformly random block of
 * the whole base device; dev wants to write, so the scheduler draws who
 * writes until dev is drawn (see blockio_draw); everything that may write
 * holds io_mutex, so the scheduler can let the other partitions write; in
 * level 3 there is one draw per tick (see blockio_wait_tick), and dev
 * waits for the ticks (with io_mutex: nothing else can write anyway, the
 * other partitions and control commands that wait for io_mutex wait);
 *
 * the lock of dev (wait_unlock, if the writer set it) is released while
 * it waits, so dev can be read: dev does not change in the meantime
 * (everything that changes a partition holds io_mutex), its current block
 * is complete in RAM and is not written yet; the draws use the io of the
 * clock thread (only the holder of io_mutex uses it), not the io of dev,
 * that the reads use */
static void blockio_dev_wait_turn(blockio_dev_t *dev) {
	blockio_t *b = dev->b;
	blockio_dev_t *drawn;
	pthread_mutex_t *unlock = NULL;
	void *io = dev->io;

	/* the scheduler gave this partition the turn */
	if (dev->turn) {
		dev->turn = 0;
		return;
	}

	if (b->paranoia < 2) return;

	if (dev->wait_unlock && b->clock_io) {
		unlock = dev->wait_unlock;
		io = b->clock_io;
		pthd_mutex_unlock(unlock);
	}

	for (;;) {
		if (b->paranoia < 2) break;
		if (b->paranoia == 3 && !blockio_wait_tick(b)) continue;

		if (!(drawn = blockio_draw(b, io))) continue;

		if (drawn == dev) break;

		/* another partition writes one block */
		blockio_give_turn(b, drawn);
	}

	if (unlock) pthd_mutex_lock(unlock);
}

void blockio_dev_write_current_macroblock(blockio_dev_t *dev) {
	uint32_t id = blockio_get_macroblock_index(dev->bi);
	int i, wipe = 0;
	assert(dev->bi && id < dev->b->total_macroblocks);
	
	blockio_dev_wait_turn(dev);

	DEBUG("write block %u (seqno=%lu)", id, dev->bi->seqno);
	dev->writes++;
	dev->self_tail = 0;
	dev->final_pending = 0;

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

	/* a new nonce for every write (see blockio_iv0) */
	dev->bi->nonce = ((uint64_t)random_uint32(&dev->r)<<32)|
		random_uint32(&dev->r);

	/* encrypt datablocks (also the unused ones), the plaintext
	 * stays in tmp_macroblock, store the hash of each ciphertext
	 * in the index, together with the corrupt parts of the mesoblocks
	 * (corrupt mesoblocks that were copied into this block stay marked
	 * as corrupt) */
	for (i = 1; i <= dev->b->mmpm; i++) {
		cipher_enc(dev->c, dev->out_macroblock +
				(i<<dev->b->mesoblk_log),
			BASE + (i<<dev->b->mesoblk_log),
			blockio_iv0(dev->bi), i, id);
		dev->bi->hashes[i-1] = mesoblk_hash(dev->out_macroblock +
				(i<<dev->b->mesoblk_log),
				1<<dev->b->mesoblk_log)|
			((i <= dev->bi->no_indices)?
			 (dev->bi->hashes[i-1]&BLOCKIO_CORRUPT_MASK):0);
		binio_write_uint64_be(HASH_UINT64(i-1), dev->bi->hashes[i-1]);
	}
	memset(HASH_UINT64(dev->b->mmpm), 0, 8); /* unused */
	binio_write_uint64_be(NONCE_UINT64, dev->bi->nonce);
	memset(RESERVED24, 0, 24);

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

	/* the block must be on disk before the next block is written: the
	 * next block may be the tail of this block, its live data is in
	 * this block now; this also means that only the most recently
	 * written block can be incomplete after a power failure (see
	 * blockio_write_block, it syncs) */
	blockio_write_block(dev->b, dev->io, id, dev->out_macroblock);

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

	/* while shrinking to 0: remember the two most recently written
	 * (different) blocks (see blockio_dev_select_next_macroblock) */
	if (dev->pending_removals &&
			dev->pending_removals == dev->no_macroblocks) {
		if (dev->bi != dev->zero[0]) {
			dev->zero[1] = dev->zero[0];
			dev->zero[0] = dev->bi;
		}
	} else if (!dev->final_write) dev->zero[0] = dev->zero[1] = NULL;

	/* removed blocks are not part of the partition on disk anymore; the
	 * final write: the partition is removed, it is wiped */
	if (dev->final_write) {
		dev->final_write = 0;
		dev->zero[0] = dev->zero[1] = NULL;
		dev->last = dev->bi;
		blockio_dev_start_wipe(dev);
		wipe = 1;
	} else if (!dev->dying) blockio_dev_release_removed(dev);

	dev->ram_block = dev->bi; /* its plaintext is still in RAM */
	if (dev->bi->dev != dev) dev->ram_block = NULL; /* was removed */
	dev->bi = NULL; /* there is no current block */
	dev->updated = 0;

	if (wipe && dev->b->paranoia < 2) blockio_dev_wipe_all(dev);
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
	if (!magic_ok(buf + 0x070)) goto out;
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

	/* the new blocks may become the next block (see juggler.c), that is
	 * not possible if the current block is its own next block (it may not
	 * change the state of the partition, see scubed3_settle) */
	assert(!dev->self_tail);
	dev->j.greedy = !dev->b->paranoia;

	pthd_mutex_lock(&dev->b->unallocated_mutex);

	if (dllarr_count(&dev->b->unallocated) + dllarr_count(&dev->release) <
			size) {
		err = -1; // not enough unclaimed blocks avaiable
		// fall through to unlock
	} else while (size--) {
		blockio_info_t *bi;

		/* blocks that were removed, but are not yet released, are
		 * taken back first; they are in the draw set of the current
		 * block, see juggler_take_back; their index on disk still
		 * counts (see blockio_dev_count_ondisk), but their indices
		 * are not live (also of the blocks that are not replayed at
		 * startup), they are treated like added blocks that still
		 * have an index of ours (see blockio_dev_check_readded) */
		if ((bi = dllarr_last(&dev->release))) {
			dllarr_remove(&dev->release, bi);
			VERBOSE("taking back block %u",
					blockio_get_macroblock_index(bi));
			bi->no_indices = bi->no_nonobsolete = 0;
			bi->readded = 1;
			dev->no_macroblocks++;
			juggler_take_back(&dev->j, bi);
			blockio_dev_change_macroblock_status(bi, USED);
			continue;
		}
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

	/* the next block may have changed, it is the tail of the current
	 * block; if the old tail is absorbed already, the copy of its live
	 * data in the current block is newer, so it does not matter */
	if (dev->bi) dev->tail_macroblock = dev->j.next;
	
	pthd_mutex_unlock(&dev->b->unallocated_mutex);

	return err;
}
