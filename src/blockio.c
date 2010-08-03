/* blockio.c - handles block input/output
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
#include <sys/types.h>
#include <sys/stat.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <assert.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <fcntl.h>
#include "blockio.h"
#include "bit.h"
#include "bitmap.h"
#include "verbose.h"
#include "binio.h"
#include "util.h"
#include "gcry.h"

#define DM_SECTOR_LOG 9

/* stream (FILE*) stuff */

static void stream_io(void *fp, void *buf, uint64_t offset, uint32_t size,
		size_t (*io)(), const char *rwing) {
	assert(fp && ((!buf && !size) || (buf && size)) && io);

	if (fseeko(fp, offset, SEEK_SET))
		FATAL("error seeking to byte %llu: %s",
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

/* end stream stuff */

#define MAX_MACROBLOCKS		(3*4096*4)

#define BASE			(dev->tmp_macroblock)
#define INDEXBLOCK_HASH		(BASE + 0)
#define DATABLOCKS_HASH		(BASE + 32)
#define SEQNOS_HASH		(BASE + 64)
#define SEQNO			(BASE + 96)
#define MAGIC			(BASE + 104)
#define TAIL_MACROBLOCK		(BASE + 112)
#define NO_MACROBLOCKS		(BASE + 116)
#define RESERVED_MACROBLOCKS	(BASE + 120)
#define KEEP_REVISIONS		(BASE + 124)
#define RANDOM_LEN		(BASE + 125)
#define MACROBLOCK_LOG		(BASE + 126)
#define MESOBLOCK_LOG		(BASE + 127)
#define NO_INDICES		(BASE + 256)
#define BITMAP			(BASE + 4096)

void blockio_free(blockio_t *b) {
	assert(b);
	random_free(&b->r);
	if (b->close) b->close(b->priv);
	free(b->blockio_infos);
}

/* open backing file and set macroblock size */
void blockio_init_file(blockio_t *b, const char *path, uint8_t macroblock_log,
		uint8_t mesoblk_log) {
	struct stat stat_info;
	struct flock lock;
	uint64_t tmp;
	assert(b);
	assert(sizeof(off_t)==8);
	assert(macroblock_log < 8*sizeof(uint32_t));

	b->macroblock_log = macroblock_log;
	b->macroblock_size = 1<<macroblock_log;
	b->mesoblk_log = mesoblk_log;

	b->read = stream_read;
	b->write = stream_write;
	b->close = stream_close;

	if (!(b->priv = fopen(path, "r+")))
		FATAL("fopening %s: %s", path, strerror(errno));

	lock.l_type = F_WRLCK;
	lock.l_whence = SEEK_SET;
	lock.l_start = 0;
	lock.l_len = 0;  /* whole file */

	if (fcntl(fileno(b->priv), F_SETLK, &lock) == -1) {
		if (fcntl(fileno(b->priv), F_GETLK, &lock) == -1) assert(0);

		FATAL("process with PID %d has already locked %s",
				lock.l_pid, path);
	}

	if (stat(path, &stat_info) < 0)
		FATAL("unable to 'stat' file %s: %s", path, strerror(errno));

	/* we support using a regular file or a block device as backend */
	if (S_ISREG(stat_info.st_mode)) {
		tmp = stat_info.st_size;
		DEBUG("%s is a regular file", path);

		b->no_macroblocks = stat_info.st_size>>b->macroblock_log;
	} else if (S_ISBLK(stat_info.st_mode)) {
		DEBUG("%s is a block device", path);
		if (ioctl(fileno(b->priv), BLKGETSIZE64, &tmp))
			FATAL("error querying size of blockdevice %s", path);

	} else FATAL("%s is not a regular file", path);

	DEBUG("backing size in bytes %lld, macroblock_size=%u",
			tmp, b->macroblock_size);

	/* check if the device or file is not too large */
	tmp >>= b->macroblock_log;
	if (tmp > MAX_MACROBLOCKS) FATAL("device is too large");
	b->no_macroblocks = tmp;

	b->blockio_infos = ecalloc(sizeof(blockio_info_t), b->no_macroblocks);
	random_init(&b->r, b->no_macroblocks);
}

static const char magic[8] = "SSS3v0.1";

void blockio_dev_free(blockio_dev_t *dev) {
	int i;
	assert(dev);
	VERBOSE("closing \"%s\", %s", dev->name, dev->updated?"SHOULD BE WRITTEN":"no updates");
	random_free(&dev->r);
	bitmap_free(&dev->status);

	for (i = 0; i < dev->no_macroblocks; i++) {
		dev->our_macroblocks[i]->elt.prev = NULL;
		dev->our_macroblocks[i]->elt.next = NULL;
		dev->our_macroblocks[i]->dev = NULL;
		free(dev->our_macroblocks[i]->indices);
	}

	dllist_free(&dev->used_blocks);
	free(dev->tmp_macroblock);
	free(dev->our_macroblocks);
	free(dev->name);
}

void blockio_dev_init(blockio_dev_t *dev, blockio_t *b, cipher_t *c,
		const char *name) {
	int i;
	uint64_t highest_seqno = 0;
	assert(b && c);
	assert(b->mesoblk_log < b->macroblock_log);
	bitmap_init(&dev->status, 2*b->no_macroblocks);
	dllist_init(&dev->used_blocks);

	dev->tmp_macroblock = ecalloc(1, 1<<b->macroblock_log);
	dev->b = b;
	dev->c = c;
	dev->mmpm = (1<<(b->macroblock_log - b->mesoblk_log)) - 1;

	/* read macroblock headers */
	for (i = 0; i < b->no_macroblocks; i++)
		blockio_dev_read_header(dev, i, &highest_seqno);
	if (highest_seqno == 0) {
		VERBOSE("no data");
		return;
	}

	FATAL("we have %d macroblocks, can't handle that yet", dev->no_macroblocks);
#if 0

	for (i = 0; i < b->no_macroblocks; i++) {
		if (bitmap_getbit(&dev->used, i)) {
			if (b->blockio_infos[i].dev == NULL) {
				/* claim the yet unwritten block */
				b->blockio_infos[i].dev = dev;
				b->blockio_infos[i].indices =
					ecalloc(dev->mmpm, sizeof(uint32_t));
			} else if (b->blockio_infos[i].dev != dev) {
				/* our block is claimed by someone else */
				FATAL("very bad stuff happens");
			}
		} else {
			/* this should be handled by removing the block */
			if (b->blockio_infos[i].dev == dev) {
				FATAL("block looks like ours, but isn't");
			}
		}
	}

	dev->no_macroblocks = bitmap_count(&dev->used);
	dev->macroblocks =
		ecalloc(dev->no_macroblocks, sizeof(blockio_info_t*));

	//VERBOSE("we have %d macroblocks", dev->no_macroblocks);

	for (i = 0; i < b->no_macroblocks; i++) {
		if (b->blockio_infos[i].dev == dev) {
			assert(j < dev->no_macroblocks);
			dev->macroblocks[j++] = &b->blockio_infos[i];
		}
	}

#endif
}

#if 0
blockio_info_t *blockio_dev_gc_which_macroblock(blockio_dev_t *dev,
		uint32_t id) {
	blockio_info_t *bi = &dev->b->blockio_infos[id];
	return bi;
}

blockio_info_t *blockio_dev_get_new_macroblock(blockio_dev_t *dev) {
	blockio_info_t *bi;

	bi = &dev->b->blockio_infos[dev->next_free_macroblock];
	assert(!bi->no_nonobsolete);
	bi->max_indices = dev->mmpm;
	return bi;
}

#endif

int compare_seqno(blockio_info_t *bi, uint64_t *seqno) {
	return *seqno > bi->seqno;
}

void blockio_dev_read_header(blockio_dev_t *dev, uint32_t no,
		uint64_t *highest_seqno) {
	assert(dev && dev->b && no < dev->b->no_macroblocks);
	blockio_info_t *tmp, *bi = &dev->b->blockio_infos[no];
	int i;
	char sha256[32];
	//VERBOSE("reading macroblock %d", no);
	bi = &dev->b->blockio_infos[no];

	if (bi->dev) {
		VERBOSE("macroblock %d already taken", no);
		return;
	}

	dev->b->read(dev->b->priv, BASE, no<<dev->b->macroblock_log,
			1<<dev->b->mesoblk_log);
	
	// decrypt indexblock with IV=0: ciphertext is unique due to
	// first block being a hash of the whole index and the index
	// containing a unique seqno (ONLY TRUE FOR CBC(LIKE)!!!!!)
	//
	// the seqno is used as IV for all the other mesoblocks
	// in the datablock
	cipher_dec(dev->c, BASE, BASE, 0, 0);

	/* check magic */
	if (memcmp(magic, MAGIC, sizeof(magic))) {
		DEBUG("magic \"%.*s\" not found in mesoblock %u",
				sizeof(magic), magic, no);
		return;
	}

	/* check indexblock hash */
	gcry_md_hash_buffer(GCRY_MD_SHA256, sha256, BASE + sizeof(sha256),
			(1<<dev->b->mesoblk_log) - sizeof(sha256));
	if (memcmp(INDEXBLOCK_HASH, sha256, sizeof(sha256))) {
		DEBUG("md5sum of index block %d failed", no);
		return;
	}

	/* check the macroblock log */
	if (dev->b->macroblock_log != binio_read_uint8(MACROBLOCK_LOG))
		FATAL("macroblock log doesn't agree (on disk %d, should be %d)",
				binio_read_uint8(MACROBLOCK_LOG),
				dev->b->macroblock_log);

	/* check the mesoblock log */
	if (dev->b->mesoblk_log != binio_read_uint8(MESOBLOCK_LOG))
		FATAL("mesoblock log doesn't agree (on disk %d, should be %d)",
				binio_read_uint8(MESOBLOCK_LOG),
				dev->b->mesoblk_log);

	/* block seems to belong to us */
	memcpy(bi->data_hash, DATABLOCKS_HASH, 32);
	memcpy(bi->seqnos_hash, SEQNOS_HASH, 32);
	bi->seqno = binio_read_uint64_be(SEQNO);

	if (bi->seqno > *highest_seqno) {
		*highest_seqno = bi->seqno;
		dev->tail_macroblock = binio_read_uint32_be(TAIL_MACROBLOCK);
		dev->no_macroblocks = binio_read_uint32_be(NO_MACROBLOCKS);
		dev->reserved_macroblocks =
			binio_read_uint32_be(RESERVED_MACROBLOCKS);
		dev->keep_revisions = binio_read_uint8(KEEP_REVISIONS);
		dev->random_len = binio_read_uint8(RANDOM_LEN);
		bitmap_read(&dev->status, (uint32_t*)BITMAP);
	}

	bi->no_indices = binio_read_uint32_be(NO_INDICES);
	bi->indices = ecalloc(dev->mmpm, sizeof(uint32_t));
	for (i = 1; i <= bi->no_indices; i++)
		bi->indices[i-1] = *(((uint32_t*)NO_INDICES) + i);
	
	VERBOSE("block %u (seqno=%llu) belongs to \"%s\" and has %d indices",
			no, bi->seqno, dev->name, bi->no_indices);

	/* put block in the 'in use' list, if it contains data */
	if (bi->no_indices) {
		tmp = dllist_iterate(&dev->used_blocks,
				(int (*)(dllist_elt_t*, void*))compare_seqno,
				&bi->seqno);

		if (tmp) dllist_insert_before(&tmp->elt, &bi->elt);
		else dllist_append(&dev->used_blocks, &bi->elt);
	}
	bi->dev = dev;

	return;
}

#if 0
void blockio_dev_read_mesoblk_part(blockio_dev_t *dev, void *buf, uint32_t id,
		uint32_t no, uint32_t offset, uint32_t len) {
	assert(dev->b && dev->b->read && id < dev->b->no_macroblocks &&
			no < dev->mmpm);
	unsigned char mesoblk[1<<dev->b->mesoblk_log];

	/* FIXME: do some caching */
	blockio_dev_read_mesoblk(dev, mesoblk, id, no);
	memcpy(buf, mesoblk + offset, len);
}

void blockio_dev_read_mesoblk(blockio_dev_t *dev,
		void *buf, uint32_t id, uint32_t no) {
	dev->b->read(dev->b->priv, buf, (id<<dev->b->macroblock_log) +
			((no + dev->no_indexblocks)<<dev->b->mesoblk_log) +
			0, dev->mesoblk_size);
	cipher_dec(dev->c, buf, buf, dev->b->blockio_infos[id].seqno,
			no + dev->no_indexblocks);
}

int blockio_check_data_hash(blockio_info_t *bi) {
	uint32_t id = bi - bi->dev->b->blockio_infos;
	char md5[16];
	bi->dev->b->read(bi->dev->b->priv, bi->dev->tmp_macroblock +
			(bi->dev->no_indexblocks<<bi->dev->b->mesoblk_log),
			(id<<bi->dev->b->macroblock_log) +
			(bi->dev->no_indexblocks<<bi->dev->b->mesoblk_log),
			bi->dev->mmpm<<bi->dev->b->mesoblk_log);

	gcry_md_hash_buffer(GCRY_MD_MD5, md5, bi->dev->tmp_macroblock +
			(bi->dev->no_indexblocks<<bi->dev->b->mesoblk_log),
			bi->dev->mmpm<<bi->dev->b->mesoblk_log);

	if (memcmp(bi->data_hash, md5, sizeof(md5))) return 0;

	return 1;
}
#endif

void blockio_dev_write_macroblock(blockio_dev_t *dev, const void *data,
		blockio_info_t *bi) {
	uint32_t id = bi - dev->b->blockio_infos;
	//int i;
	assert(bi && id < dev->b->no_macroblocks);

	DEBUG("write block %u (seqno=%llu)", id, bi->seqno);

	/* write magic */
	memcpy(MAGIC, magic, sizeof(magic));

	/* write static data */
	binio_write_uint8(MACROBLOCK_LOG, dev->b->macroblock_log);
	binio_write_uint8(MESOBLOCK_LOG, dev->b->mesoblk_log);

	binio_write_uint64_be(SEQNO, bi->seqno);
#if 0
	binio_write_uint32_be(RESERVED, dev->reserved);
	binio_write_uint32_be(NO_INDEXBLOCKS, dev->no_indexblocks);

	/* write other data */
	memcpy(SEQNOS_HASH, bi->seqnos_hash, 16);
	binio_write_uint64_be(SEQNO, bi->seqno);
	binio_write_uint16_be(NO_INDICES, bi->no_indices);
	binio_write_uint32_be(NEXT_MACROBLOCK, dev->next_free_macroblock);

	bit_pack(INDICES, bi->indices, bi->no_indices, dev->strip_bits);
	binio_write_uint32_be(INDICES + 4*bit_get_size(dev->mmpm,
				dev->strip_bits), dev->used.no_bits);
	bitmap_write(INDICES + 4*bit_get_size(dev->mmpm,
				dev->strip_bits) + 4, &dev->used);

	/* encrypt data */
	for (i = 0; i < dev->mmpm; i++)
		cipher_enc(dev->c, BASE +
				((dev->no_indexblocks + i)<<
				 dev->b->mesoblk_log),
				data + (i<<dev->b->mesoblk_log), bi->seqno,
				i + dev->no_indexblocks);

	/* calculate md5sum of data, store in index */
	gcry_md_hash_buffer(GCRY_MD_MD5, DATABLOCKS_HASH,
			BASE + (dev->no_indexblocks<<dev->b->mesoblk_log),
			dev->mmpm<<dev->b->mesoblk_log);

	/* calculate md5sum of indexblock */
	gcry_md_hash_buffer(GCRY_MD_MD5, INDEXBLOCK_HASH, MAGIC0,
			(dev->no_indexblocks<<dev->b->mesoblk_log) -
			MAGIC0_OFFSET);

	/* encrypt index, first block with IV 0, we never encrypt
	 * the same first block twice, since the first block depends
	 * (through a cryptographic hash) on the sequence number */
	cipher_enc(dev->c, BASE, BASE, 0, 0);

	for (i = 1; i < dev->no_indexblocks; i++)
		cipher_enc(dev->c, BASE + (i<<dev->b->mesoblk_log),
				BASE + (i<<dev->b->mesoblk_log), bi->seqno, i);

	dev->b->write(dev->b->priv, BASE, id<<dev->b->macroblock_log,
			1<<dev->b->macroblock_log);
#endif
}
