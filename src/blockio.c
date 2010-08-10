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
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <fcntl.h>
#include <assert.h>
#include "blockio.h"
#include "bitmap.h"
#include "verbose.h"
#include "binio.h"
#include "util.h"
#include "gcry.h"
#include "ecch.h"

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

static void *stream_open(const char *path) {
	void *ret;
	if (!(ret = fopen(path, "r+")))
		ecch_throw(ECCH_DEFAULT, "fopening %s: %s", path, strerror(errno));

	return ret;
}

/* end stream stuff */

#define MAX_MACROBLOCKS		(3*4096*4)

#define BASE			(dev->tmp_macroblock)
#define INDEXBLOCK_HASH		(BASE + 0)
#define DATABLOCKS_HASH		(BASE + 32)
#define SEQNOS_HASH		(BASE + 64)
#define SEQNO			(BASE + 96)
#define MAGIC			(BASE + 104)
#define RANDOM_LEN		(BASE + 112)
#define NO_MACROBLOCKS		(BASE + 116)
#define RESERVED_MACROBLOCKS	(BASE + 118)
#define LAYOUT_REVISION		(BASE + 120)
#define MACROBLOCK_LOG		(BASE + 122)
#define MESOBLOCK_LOG		(BASE + 123)
#define NEXT_SEQNO_DIFF		(BASE + 124)
#define TAIL_MACROBLOCK_GLOBAL	(BASE + 128)
#define NO_INDICES		(BASE + 256)
#define BITMAP			(BASE + 4096)

void blockio_free(blockio_t *b) {
	assert(b);
	random_free(&b->r);
	free(b->blockio_infos);
}

/* open backing file and set macroblock size */
void blockio_init_file(blockio_t *b, const char *path, uint8_t macroblock_log,
		uint8_t mesoblk_log) {
	struct stat stat_info;
	struct flock lock;
	uint64_t tmp;
	void *priv;
	assert(b);
	assert(sizeof(off_t)==8);
	assert(macroblock_log < 8*sizeof(uint32_t));

	b->open_priv = strdup(path);
	if (!b->open_priv) FATAL("out of memory");
	b->macroblock_log = macroblock_log;
	b->macroblock_size = 1<<macroblock_log;
	b->mesoblk_log = mesoblk_log;

	b->open = (void* (*)(const void*))stream_open;
	b->read = stream_read;
	b->write = stream_write;
	b->close = stream_close;

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

		b->no_macroblocks = stat_info.st_size>>b->macroblock_log;
	} else if (S_ISBLK(stat_info.st_mode)) {
		DEBUG("%s is a block device", path);
		if (ioctl(fileno(priv), BLKGETSIZE64, &tmp))
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
	stream_close(priv);
}

static const char magic[8] = "SSS3v0.1";

void blockio_dev_free(blockio_dev_t *dev) {
	int i;
	assert(dev);
	//VERBOSE("closing \"%s\", %s", dev->name, dev->updated?"SHOULD BE WRITTEN":"no updates");
	if (dev->updated) blockio_dev_write_current_macroblock(dev);
	random_free(&dev->r);
	bitmap_free(&dev->status);

	for (i = 0; i < dev->no_macroblocks; i++) {
		blockio_info_t *bi =
			&dev->b->blockio_infos[dev->macroblock_ref[i]];
		bi->elt.prev = NULL;
		bi->elt.next = NULL;
		bi->elt2.prev = NULL;
		bi->elt2.next = NULL;
		bi->dev = NULL;
		free(bi->indices);
	}

	dllist_free(&dev->used_blocks);
	dllist_free(&dev->free_blocks);
	dllist_free(&dev->selected_blocks);
	dllist_free(&dev->ordered);
	free(dev->tmp_macroblock);
	free(dev->macroblock_ref);
	free(dev->name);
	if (dev->b->close) dev->b->close(dev->io);
}

static void verbose_ordered(dllist_t *u) {
	int count = 0;
	int verbose_ding(void *i, int *count) {
		blockio_info_t *bi = i - offsetof(blockio_info_t, elt2); 
		int id = bi - bi->dev->b->blockio_infos;
		char *state;
		switch (blockio_dev_get_macroblock_status(bi->dev, id)) {
			case HAS_DATA:
				state = "HAS DATA";
				break;
			case FREE:
				state = "FREE";
				break;
			case SELECTFROM:
				state = "SELECT";
				break;
			default:
				assert(0);
		}

		VERBOSE("i=%3d, id=%3d, rev=%d, seq=%4lld, nseq=%4lld %s", *count,
				id, bi->layout_revision,
				bi->seqno, bi->next_seqno, state);
		(*count)++;
		return 1;
	}

	dllist_iterate(u, (int (*)(dllist_elt_t*, void*))
			verbose_ding, &count);
}

static blockio_info_t *find_ordered_equal_or_first_after(dllist_t *u, uint16_t rev,
		uint64_t seqno, uint32_t *index) {
	struct priv_s {
		uint16_t rev;
		uint64_t seqno;
		uint32_t *index;
	} priv = {
		.rev = rev,
		.seqno = seqno,
		.index = index,
	
	};
	*index = 0;
	int compare_rev_nseq(void *e, struct priv_s *p) {
		blockio_info_t *bi = e - offsetof(blockio_info_t, elt2);
		if (bi->layout_revision == p->rev && bi->next_seqno < seqno) {
			(*p->index)++;
			return 1;
		}
		if (bi->layout_revision < rev) {
			(*p->index)++;
			return 1;
		}
		return 0;
	}

	void *tmp;

	tmp = dllist_iterate(u, (int (*)(dllist_elt_t*, void*))
			compare_rev_nseq, &priv);

	if (!tmp) return NULL;

	return (blockio_info_t*)(tmp - offsetof(blockio_info_t, elt2));
}

static void add_to_ordered(dllist_t *u, blockio_info_t *bi) {
	int compare_rev_nseq(void *o, blockio_info_t *new) {
		blockio_info_t *old = o - offsetof(blockio_info_t, elt2); 
		//VERBOSE("compareth old: rev=%d, nseq=%lld, new: rev=%d, nseq=%lld",
		//		old->layout_revision, old->next_seqno,
		//		new->layout_revision, new->next_seqno);
		if (old->layout_revision == new->layout_revision &&
				old->next_seqno < new->next_seqno) return 1;
		if (old->layout_revision < new->layout_revision) return 1;
		return 0;
	}

	dllist_elt_t *tmp;

	//VERBOSE("insert %d seq=%lld, nseq=%lld in ordered",
	//		bi - bi->dev->b->blockio_infos, bi->seqno,
	//		bi->next_seqno);

	tmp = dllist_iterate(u, (int (*)(dllist_elt_t*, void*))
			compare_rev_nseq, bi);

	if (tmp) dllist_insert_before(tmp, &bi->elt2);
	else dllist_append(u, &bi->elt2);
}

static void add_to_used(dllist_t *u, blockio_info_t *bi) {
	int compare_seqno(blockio_info_t *bi, uint64_t *seqno) {
		return *seqno > bi->seqno;
	}
	blockio_info_t *tmp;

	/* put block in the 'in use' list, if it contains data */
	/* sort with sequence number */
	if (bi->no_indices) {
		tmp = dllist_iterate(u,
				(int (*)(dllist_elt_t*, void*))compare_seqno,
				&bi->seqno);

		if (tmp) dllist_insert_before(&tmp->elt, &bi->elt);
		else dllist_append(u, &bi->elt);
	}
}

static int last_diff(random_t *r, int last, int *first) {
        int i;
        assert(last >= 0);

        for (i = 0; i < last; i++)
                if (random_peek(r, i) == random_peek(r, last)) {
                        if (i == 0) *first = 0;
                        return 0;
                }

        return 1;
}

void blockio_dev_select_next_macroblock(blockio_dev_t *dev, int first) {
	int number, different = 1, tmp2 = 0, tmp3;
	uint32_t next_seqno_diff = 0;

	assert(!dev->bi);

	dev->valid = 1;

	number = dev->macroblock_ref[random_peek(&dev->r, 0)];
	dev->bi = &dev->b->blockio_infos[number];
	VERBOSE("select next: id=%d next_seqno=%lld, bi->seqno=%lld, bi->next_seqno=%lld", number, dev->next_seqno, dev->bi->seqno, dev->bi->next_seqno);
	assert(((dev->bi->seqno == dev->bi->next_seqno) && dev->bi->seqno == 0) ||
			dev->next_seqno == dev->bi->next_seqno);
	dev->bi->seqno = dev->next_seqno;
	dev->bi->layout_revision = dev->layout_revision;

	do next_seqno_diff++;
	while (random_peek(&dev->r, next_seqno_diff) != random_peek(&dev->r, 0));

	dev->bi->next_seqno = dev->bi->seqno + next_seqno_diff;
#if 0
	VERBOSE("new block %d (local_id=%d) seqno=%lld next_seqno=%lld",
			number, random_peek(&dev->r, 0),
			dev->bi->seqno, dev->bi->next_seqno);
#endif


	dllist_remove(&dev->bi->elt); // we don't want the current next
				      // block on any list

	if (first) assert(blockio_dev_get_macroblock_status(dev,
				number) == FREE);
	else blockio_dev_change_macroblock_status(dev,
			number, SELECTFROM, FREE);

	//VERBOSE("---start---");
	while (different <= dev->keep_revisions) {
		tmp2++;
		tmp3 = dev->macroblock_ref[random_peek(&dev->r, tmp2)];
		if (last_diff(&dev->r, tmp2, &dev->valid)) {
			if (different != dev->keep_revisions) {
				if (tmp2 + 1 >= dev->random_len) {
					blockio_dev_change_macroblock_status(dev,
							tmp3,  FREE, SELECTFROM);
					dllist_remove(&dev->b->blockio_infos[tmp3].elt);
					dllist_append(&dev->selected_blocks,
							&dev->b->blockio_infos[tmp3].elt);
				} else assert(blockio_dev_get_macroblock_status(
							dev,
							dev->macroblock_ref[
							random_peek(&dev->r,
								tmp2)]) ==
						SELECTFROM);
			}
			different++;
		}
		//VERBOSE("bla different=%d, tmp2=%d, tmp3=%d, status=%d, rl=%d", different, tmp2, tmp3, blockio_dev_get_macroblock_status(dev, tmp3), dev->random_len);
	}

	if (!dev->valid) {
		blockio_dev_change_macroblock_status(dev, number, FREE, SELECTFROM);
		dllist_append(&dev->selected_blocks, &dev->bi->elt);
	}

	dev->tail_macroblock_global =
		dev->macroblock_ref[random_peek(&dev->r, tmp2)];

	dev->random_len = tmp2;

	random_pop(&dev->r);
}

void blockio_dev_write_current_and_select_next_valid_macroblock(
		blockio_dev_t *dev) {
	do {
		blockio_dev_write_current_macroblock(dev);
		blockio_dev_select_next_macroblock(dev, 0);
	} while (!dev->valid);
}

struct hash_seqnos_s {
        gcry_md_hd_t hd;
        blockio_info_t *last;
};

uint8_t *hash_seqnos(blockio_dev_t *dev, uint8_t *hash_res, blockio_info_t *last) {
        struct hash_seqnos_s priv = {
                .last = last
        };
        int add_seqno(blockio_info_t *bi, struct hash_seqnos_s *priv) {
                //printf(" %llu", bi->seqno);
                gcry_md_write(priv->hd, &bi->seqno, sizeof(uint64_t));
                return !(priv->last == bi);
        }

        //printf("sns:");
        gcry_call(md_open, &priv.hd, GCRY_MD_SHA256, 0);
        dllist_iterate(&dev->used_blocks,
                        (int (*)(dllist_elt_t*, void*))add_seqno, &priv);
        //printf("\n");
        memcpy(hash_res, gcry_md_read(priv.hd, 0), 32);
        gcry_md_close(priv.hd);

        return hash_res;
}

void blockio_dev_init(blockio_dev_t *dev, blockio_t *b, cipher_t *c,
		const char *name) {
	int tail_macroblock = 0;
	int i, tmp_no_macroblocks = 0, tmp2 = 0;
	int no_different_selectblocks, is_select_block;
	uint64_t highest_seqno = 0;
	uint8_t hash[32];

	assert(b && c);
	assert(b->mesoblk_log < b->macroblock_log);
	bitmap_init(&dev->status, 2*b->no_macroblocks);
	dllist_init(&dev->used_blocks);
	dllist_init(&dev->free_blocks);
	dllist_init(&dev->selected_blocks);
	dllist_init(&dev->ordered);

	assert(b->open);
	dev->io = (b->open)(b->open_priv);

	dev->tmp_macroblock = ecalloc(1, 1<<b->macroblock_log);
	dev->b = b;
	dev->c = c;
	dev->mmpm = (1<<(b->macroblock_log - b->mesoblk_log)) - 1;

	/* read macroblock headers */
	for (i = 0; i < b->no_macroblocks; i++)
		blockio_dev_read_header(dev, i, &highest_seqno);
	
	random_init(&dev->r, 0);
	dev->next_seqno = highest_seqno + 1;

	if (highest_seqno == 0) {
		VERBOSE("device \"%s\" is empty; no macroblocks found", dev->name);
		return;
	}

	uint16_t rebuild_prng[dev->random_len - 1];
	memset(&rebuild_prng[0], 0, sizeof(rebuild_prng));

	dev->macroblock_ref = ecalloc(dev->no_macroblocks, sizeof(uint16_t));

	tmp_no_macroblocks = dev->no_macroblocks;
	dev->no_macroblocks = 0;

	/* check all macroblocks in the map */
	for (i = 0; i < b->no_macroblocks; i++) {
		blockio_info_t *bi = &b->blockio_infos[i];
		is_select_block = 0;
		switch(bitmap_getbits(&dev->status, i<<1, 2)) {
			case NOT_ALLOCATED:
				if (bi->dev == dev) {
					/* this block doesn't belong
					 * to us anymore */
					bi->dev = NULL;
					free(bi->indices);
				}
				//VERBOSE("block %d: NOT_ALLOCATED", i);
				break;

			case HAS_DATA:
				if (bi->dev != dev || !bi->no_indices) {
					/* no data was detected, there must
					 * however be data, so we have
					 * a bug */
					ecch_throw(ECCH_DEFAULT, "unable to open partition, datablock %s", (bi->dev != dev)?"missing":"is empty");
				}
				assert(tmp_no_macroblocks > dev->no_macroblocks);
				dev->macroblock_ref[dev->no_macroblocks++] = i;
				add_to_used(&dev->used_blocks, bi);
				add_to_ordered(&dev->ordered, bi);
				break;

			case SELECTFROM:
				if (bi->seqno == highest_seqno) dev->keep_revisions--;
				//VERBOSE("found selectblock %d %d", tmp2, i);
				assert(tmp2 < dev->random_len - 1);
				rebuild_prng[tmp2++] = dev->no_macroblocks;
				is_select_block = 1;
			case FREE:
				/* check if someone has taken it over */
				if (bi->dev != dev) {
					if (bi->dev) ecch_throw(ECCH_DEFAULT, "unable to open partition, datablock %d is claimed by partition \"%s\"", bi - dev->b->blockio_infos, bi->dev->name);
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

				assert(tmp_no_macroblocks > dev->no_macroblocks);
				dev->macroblock_ref[dev->no_macroblocks++] = i;
				if (is_select_block) {
					//bi->seqno = 0;
					bi->next_seqno = 0;
					//bi->layout_revision = 0;
					dllist_append(&dev->selected_blocks,
							&bi->elt);
				} else {
					dllist_append(&dev->free_blocks, &bi->elt);
				}
				add_to_ordered(&dev->ordered, bi);
				break;
		}
	}
	
	assert(tmp_no_macroblocks == dev->no_macroblocks);

	verbose_ordered(&dev->ordered);

	/* find internal number of tail_macroblock */
	while (dev->macroblock_ref[tail_macroblock] !=
			dev->tail_macroblock_global)  tail_macroblock++;

	assert(tail_macroblock < dev->no_macroblocks);

	assert(tmp2 <= dev->random_len - 1);
	//VERBOSE("we have %d different blocks to rebuild prng", tmp2);
	dev->keep_revisions += tmp2 + 1;
	//VERBOSE("keep_revisions = %d", dev->keep_revisions);
	no_different_selectblocks = tmp2;
	random_rescale(&dev->r, dev->no_macroblocks);
	
	/* first: fill in the blanks in rebuild_prng (if any) with random
	 * choices of the filled in argument */
	while (tmp2 < dev->random_len - 1)
		rebuild_prng[tmp2++] = rebuild_prng[random_custom(&dev->r, no_different_selectblocks)];
	
	/* shuffle */
	for (i = 0; i < tmp2 - 1; i++) {
		uint16_t xchg = rebuild_prng[i];
		uint16_t index;
		index = random_custom(&dev->r, tmp2 - i) + i;
		rebuild_prng[i] = rebuild_prng[index];
		rebuild_prng[index] = xchg;
		//VERBOSE("swapped %d and %d", i, index);
	}

#if 1
	uint64_t walking_seqno = dev->next_seqno;
	for (i = dev->random_len - 2; i >= 0; i--) {
		blockio_info_t *bi = &dev->b->blockio_infos[dev->macroblock_ref[rebuild_prng[i]]];
		if (bi->next_seqno == 0) {
			VERBOSE("assign seqno");
			bi->next_seqno = walking_seqno;
		} else {
			VERBOSE("not assign seqno");
		}
		VERBOSE("reconstruct %3d(%3d) %4lld",
				dev->macroblock_ref[rebuild_prng[i]],
				rebuild_prng[i],
				walking_seqno);
		walking_seqno++;
	}

	verbose_ordered(&dev->ordered);

	uint16_t next;
	uint32_t index = 0;
	blockio_info_t *next_ordered;
	
	next = dev->tail_macroblock_global;
	next_ordered = find_ordered_equal_or_first_after(&dev->ordered, 1, walking_seqno, &index);
	if (next_ordered) {
		assert(index < dev->no_macroblocks);
		//VERBOSE("next ordered %3d %4lld rev=%d, choose from %d", next_ordered - dev->b->blockio_infos, next_ordered->next_seqno, next_ordered->layout_revision, index);
	} else {
		assert(index == dev->no_macroblocks);
		//VERBOSE("there is no next ordered");
	}

	if (next_ordered == &dev->b->blockio_infos[dev->tail_macroblock_global]) {
		assert(next_ordered->next_seqno == walking_seqno);
		next_ordered = (void*)next_ordered->elt2.next;
		index++;
		if ((dllist_elt_t*)next_ordered != &dev->ordered.tail) {
			assert(index < dev->no_macroblocks);
			next_ordered = (void*)next_ordered - offsetof(blockio_info_t, elt2);
		} else {
			assert(index == dev->no_macroblocks);
		}
	} else {
		if (dev->b->blockio_infos[dev->tail_macroblock_global].next_seqno == 0) {
			VERBOSE("assign seqno");
			dev->b->blockio_infos[dev->tail_macroblock_global].next_seqno = walking_seqno;
		} else {
			VERBOSE("not assign seqno");
		}
	}

	VERBOSE("reconstruct %3d(%3d) %4lld",
			dev->tail_macroblock_global, tail_macroblock, walking_seqno);
	walking_seqno++;


	void *temper = dllist_get_tail(&dev->ordered);
	uint64_t temperder = walking_seqno - 1;
	if (temper) {
		temperder = ((blockio_info_t*)(temper - offsetof(blockio_info_t, elt2)))->next_seqno;
	}

	VERBOSE("highest next_seqno=%lld", temperder);
	int generated = 0;
	uint32_t to_generate = (uint32_t)(temperder - highest_seqno) - dev->random_len;
	VERBOSE("to generate=%d", to_generate);
	uint16_t more_data[to_generate];
	while (next_ordered) {
		uint16_t internal = 0;
		if (walking_seqno == next_ordered->next_seqno) {
			while (dev->macroblock_ref[internal] !=
				next_ordered - dev->b->blockio_infos) internal++;
			more_data[--to_generate] = internal;
			VERBOSE("recon (list) %3d(%3d) %4lld",
					next_ordered - dev->b->blockio_infos,
					internal,
					next_ordered->next_seqno);
			next_ordered = (void*)next_ordered->elt2.next;
			index++;
			if ((dllist_elt_t*)next_ordered != &dev->ordered.tail) {
				assert(index < dev->no_macroblocks);
				next_ordered = (void*)next_ordered - offsetof(blockio_info_t, elt2);
			} else {
				assert(index == dev->no_macroblocks);
				next_ordered = NULL;
			}
		} else {
			// select next random allowed block
			void *bla;
			blockio_info_t *bi;
			bla = dllist_get_nth(&dev->ordered, random_custom(&dev->r, index));
			bi = bla - offsetof(blockio_info_t, elt2);
			while (dev->macroblock_ref[internal] !=
				bi - dev->b->blockio_infos) internal++;
			more_data[--to_generate] = internal;
		if (bi->next_seqno == 0) {
			VERBOSE("assign seqno %lld, prevnext=%lld", walking_seqno,
					bi->next_seqno);
			bi->next_seqno = walking_seqno;
		} else {
			VERBOSE("not assign seqno %lld, we already have %lld",
					walking_seqno, bi->next_seqno);
		}
			VERBOSE("recon (rand) %3d(%3d) %4lld",
					bi - dev->b->blockio_infos,
					internal,
					walking_seqno);

		}
		walking_seqno++;
		generated++;
	}
	assert(to_generate == 0);
	VERBOSE("generated %d %lld %lld %d", generated, highest_seqno, walking_seqno - 1, (uint32_t)(walking_seqno - highest_seqno) - 1 - dev->random_len);

	/* push all generated data to the PRNG */
	for (i = 0; i < generated; i++) {
		//VERBOSE("pushing %d", more_data[i]);
		random_push(&dev->r, more_data[i]);
	}
#endif
	random_push(&dev->r, tail_macroblock);
	for (i = 0; i < tmp2; i++) random_push(&dev->r, rebuild_prng[i]);

	blockio_info_t *latest = dllist_get_tail(&dev->used_blocks);
	if (latest) {
		hash_seqnos(dev, hash, latest);
		if (memcmp(hash, latest->seqnos_hash, 32)) {
			ecch_throw(ECCH_DEFAULT, "hash of seqnos of required datablocks faild, revision %lld corrupt", latest->seqno);
		}
	}

	VERBOSE("we have %d macroblocks in partition \"%s\", "
			"latest revision %lld OK",
			dev->no_macroblocks, dev->name, (latest)?latest->seqno:0);

	/* select next block */
	blockio_dev_select_next_macroblock(dev, 0);
}

void blockio_dev_read_header(blockio_dev_t *dev, uint32_t no,
		uint64_t *highest_seqno) {
	assert(dev && dev->b && no < dev->b->no_macroblocks);
	blockio_info_t /**tmp,*/ *bi = &dev->b->blockio_infos[no];
	int i;
	char sha256[32];
	//VERBOSE("reading macroblock %d", no);
	bi = &dev->b->blockio_infos[no];

	if (bi->dev) {
		//VERBOSE("macroblock %d already taken", no);
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
	cipher_dec(dev->c, BASE, BASE, 0, 0);

	/* check magic */
	if (memcmp(magic, MAGIC, sizeof(magic))) {
		//DEBUG("magic \"%.*s\" not found in macroblock %u",
	//			sizeof(magic), magic, no);
		return;
	}

	/* check indexblock hash */
	gcry_md_hash_buffer(GCRY_MD_SHA256, sha256, BASE + sizeof(sha256),
			(1<<dev->b->mesoblk_log) - sizeof(sha256));
	if (memcmp(INDEXBLOCK_HASH, sha256, sizeof(sha256))) {
		DEBUG("sha256 hash of index block %d failed", no);
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
	bi->next_seqno = bi->seqno + binio_read_uint32_be(NEXT_SEQNO_DIFF);
	bi->layout_revision = binio_read_uint16_be(LAYOUT_REVISION);
	if (bi->seqno > *highest_seqno) {
		*highest_seqno = bi->seqno;
		dev->layout_revision = bi->layout_revision;
		dev->tail_macroblock_global =
			binio_read_uint16_be(TAIL_MACROBLOCK_GLOBAL);
		dev->no_macroblocks = binio_read_uint16_be(NO_MACROBLOCKS);
		dev->reserved_macroblocks =
			binio_read_uint16_be(RESERVED_MACROBLOCKS);
		dev->random_len = binio_read_uint32_be(RANDOM_LEN);
		bitmap_read(&dev->status, (uint32_t*)BITMAP);
	}

	bi->no_nonobsolete = bi->no_indices = binio_read_uint32_be(NO_INDICES);
	bi->indices = ecalloc(dev->mmpm, sizeof(uint32_t));
	for (i = 1; i <= bi->no_indices; i++)
		bi->indices[i-1] = *(((uint32_t*)NO_INDICES) + i);
	
	VERBOSE("block %u (seqno=%llu) belongs to \"%s\" and has %d indices, "
			"next_seqno=%llu",
			no, bi->seqno, dev->name, bi->no_indices,
			bi->next_seqno);

	bi->dev = dev;

	return;
}

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
	dev->b->read(dev->io, buf, (id<<dev->b->macroblock_log) +
			((no + 1)<<dev->b->mesoblk_log) +
			0, 1<<dev->b->mesoblk_log);
	cipher_dec(dev->c, buf, buf, dev->b->blockio_infos[id].seqno,
			no + 1);
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
	assert(dev->bi && id < dev->b->no_macroblocks);

	/* the current block is not on any list, put it on the correct one */
	if (dev->bi->no_indices) {
		blockio_dev_change_macroblock_status(dev,
				id, FREE, HAS_DATA);
		dllist_append(&dev->used_blocks, &dev->bi->elt);
		dev->wasted_gc += dev->bi->no_indices_gc;
		dev->useful += (dev->bi->no_indices - dev->bi->no_indices_gc);
		dev->wasted_empty += dev->mmpm - dev->bi->no_indices;
	} else {
		if (blockio_dev_get_macroblock_status(dev, id) != SELECTFROM)
			dllist_append(&dev->free_blocks, &dev->bi->elt);
		dev->wasted_keep += dev->mmpm;
	}

	//DEBUG("write block %u (seqno=%llu)", id, dev->bi->seqno);
	dev->writes++;

	/* encrypt datablocks (also the unused ones) */
	for (i = 1; i <= dev->mmpm; i++)
		cipher_enc(dev->c, BASE + (i<<dev->b->mesoblk_log),
			BASE + (i<<dev->b->mesoblk_log), dev->bi->seqno, i);

	/* calculate hash of data, store in index */
	gcry_md_hash_buffer(GCRY_MD_SHA256, DATABLOCKS_HASH,
			BASE + (1<<dev->b->mesoblk_log),
			dev->mmpm<<dev->b->mesoblk_log);
	//verbose_buffer("sha256_data", DATABLOCKS_HASH, 32);

	/* calculate hash of seqnos */
	hash_seqnos(dev, SEQNOS_HASH, dev->bi);

	/* write static data */
	binio_write_uint64_be(SEQNO, dev->bi->seqno);
	binio_write_uint32_be(NEXT_SEQNO_DIFF, dev->bi->next_seqno -
			dev->bi->seqno);
	memcpy(MAGIC, magic, sizeof(magic));
	binio_write_uint16_be(TAIL_MACROBLOCK_GLOBAL, dev->tail_macroblock_global);
	binio_write_uint16_be(LAYOUT_REVISION, dev->layout_revision);
	binio_write_uint16_be(NO_MACROBLOCKS, dev->no_macroblocks);
	binio_write_uint16_be(RESERVED_MACROBLOCKS, dev->reserved_macroblocks);
	binio_write_uint32_be(RANDOM_LEN, dev->random_len);
	binio_write_uint8(MACROBLOCK_LOG, dev->b->macroblock_log);
	binio_write_uint8(MESOBLOCK_LOG, dev->b->mesoblk_log);

	dev->bi->no_nonobsolete = dev->bi->no_indices;
	binio_write_uint32_be(NO_INDICES, dev->bi->no_indices);
	for (i = 1; i <= dev->bi->no_indices; i++)
		*(((uint32_t*)NO_INDICES) + i) = dev->bi->indices[i-1];

	bitmap_write((uint32_t*)BITMAP, &dev->status);

	/* calculate hash of indexblock */
	gcry_md_hash_buffer(GCRY_MD_SHA256, INDEXBLOCK_HASH,
			dev->tmp_macroblock + 32 /* size of hash */,
			(1<<dev->b->mesoblk_log) - 32 /* size of hash */);

	/* encrypt index */
	cipher_enc(dev->c, BASE, BASE, 0, 0);

	dev->b->write(dev->io, BASE, id<<dev->b->macroblock_log,
			1<<dev->b->macroblock_log);

	dev->next_seqno = dev->bi->seqno + 1;
	dev->bi = NULL; /* there is no current block */
}

static void blockio_dev_set_macroblock_status(blockio_dev_t *dev, uint32_t raw_no,
		blockio_dev_macroblock_status_t status) {
	assert(dev);
	assert(raw_no < dev->b->no_macroblocks);

	if (status&1) bitmap_setbit_safe(&dev->status, raw_no<<1);
	else bitmap_clearbit_safe(&dev->status, raw_no<<1);

	if (status&2) bitmap_setbit_safe(&dev->status, (raw_no<<1) + 1);
	else bitmap_clearbit_safe(&dev->status, (raw_no<<1) + 1);
}

blockio_dev_macroblock_status_t blockio_dev_get_macroblock_status(
		blockio_dev_t *dev, uint32_t raw_no) {
	assert(dev);
	assert(raw_no < dev->b->no_macroblocks);

	return bitmap_getbits(&dev->status, raw_no<<1, 2);
}

void blockio_dev_change_macroblock_status(blockio_dev_t *dev, uint32_t no,
		blockio_dev_macroblock_status_t old,
		blockio_dev_macroblock_status_t new) {
	assert(blockio_dev_get_macroblock_status(dev, no) == old);
	blockio_dev_set_macroblock_status(dev, no, new);
}

int blockio_dev_allocate_macroblocks(blockio_dev_t *dev, uint16_t size) {
	// 16 bit arithmetic
	assert(dev->no_macroblocks < dev->no_macroblocks + size); 

	int i, no_freeb = 0;
	uint16_t freeb[dev->b->no_macroblocks];
	uint16_t no, select = 0;
	void *tmp;
	assert(dev->b->no_macroblocks < 65536);
	for (i = 0; i < dev->b->no_macroblocks; i++)
		if (!dev->b->blockio_infos[i].dev) freeb[no_freeb++] = i;

	VERBOSE("we have %d free blocks to choose from", no_freeb);

	if (size > no_freeb) return -1; // not enough blocks

	tmp = realloc(dev->macroblock_ref,
			sizeof(uint16_t)*(size + dev->no_macroblocks));
	if (!tmp) return -1; // out of memory
	dev->macroblock_ref = tmp;

	while (size) {
		blockio_info_t *bi;
		no = random_custom(&dev->b->r, no_freeb);
		select = freeb[no];
		//VERBOSE("select nr %d, %d", no, select);
		bi = &dev->b->blockio_infos[select];
		bi->dev = dev;

		// add to our array
		dev->macroblock_ref[dev->no_macroblocks++] = select;

		// mark as FREE and assert() that is was NOT_ALLOCATED
		blockio_dev_change_macroblock_status(dev, select,
				NOT_ALLOCATED, FREE);
		dllist_append(&dev->free_blocks, &bi->elt);

		no_freeb--;
		memmove(&freeb[no], &freeb[no+1], sizeof(freeb[0])*(no_freeb - no));
		bi->no_indices = 0;
		bi->indices = ecalloc(dev->mmpm, sizeof(uint32_t));
		size--;
	}

	random_rescale(&dev->r, dev->no_macroblocks);

	return 0;
}

