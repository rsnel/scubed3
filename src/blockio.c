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
		ecch_throw(ECCH_DEFAULT, "fopening %s: %s", path, strerror(errno));

	return ret;
}

/* end stream stuff */

#define BASE			(dev->tmp_macroblock)
#define INDEXBLOCK_HASH		(BASE + 0)
#define DATABLOCKS_HASH		(BASE + 32)
#define SEQNOS_HASH		(BASE + 64)
#define SEQNO			(BASE + 96)
#define MAGIC			(BASE + 104)
#define RANDOM_LEN		(BASE + 112)
#define TAIL_MACROBLOCK		(BASE + 116)
#define RESERVED_MACROBLOCKS	(BASE + 118)
#define LAYOUT_REVISION		(BASE + 120)
#define MACROBLOCK_LOG		(BASE + 122)
#define MESOBLOCK_LOG		(BASE + 123)
#define NEXT_SEQNO_DIFF		(BASE + 124)
#define REV_SEQNOS		(BASE + 128)
#define NO_MACROBLOCKS		(BASE + 192)
#define REVISION_WORK		(BASE + 208)
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

        VERBOSE("mesoblock size %d bytes, macroblock size %d bytes",
			1<<b->mesoblk_log, b->macroblock_size);

	VERBOSE("%ld mesoblocks per macroblock, including index", 1L<<(macroblock_log - mesoblk_log));

        size_t indexblock_size = 96 + (4<<(macroblock_log - mesoblk_log));
	if (indexblock_size >= 1<<mesoblk_log)
		FATAL("not enough room for indexblock in mesoblock");
        VERBOSE("required minimumsize of indexblock excluding macroblock index "
			"is %ld bytes", indexblock_size);

        size_t max_macroblocks = ((1L<<mesoblk_log) - indexblock_size)<<3;
        VERBOSE("maximum amount of macroblocks supported %ld", max_macroblocks);

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

	} else FATAL("%s is not a regular file or a block device", path);

	DEBUG("backing size in bytes %ld having %ld macroblocks of size %u",
			tmp, tmp>>b->macroblock_log, b->macroblock_size);

	/* check if the device or file is not too large */
	tmp >>= b->macroblock_log;
	if (tmp > max_macroblocks) FATAL("device is too large");
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

	for (i = 0; i < dev->rev[0].no_macroblocks; i++) {
		blockio_info_t *bi =
			&dev->b->blockio_infos[dev->macroblock_ref[i]];
		free(bi->indices);
		bi->dev = NULL;
	}

	dllarr_free(&dev->used_blocks);
	dllarr_free(&dev->free_blocks);
	dllarr_free(&dev->selected_blocks);
	dllarr_free(&dev->ordered);
	free(dev->tmp_macroblock);
	free(dev->macroblock_ref);
	free(dev->name);
	if (dev->b && dev->b->close) dev->b->close(dev->io);
}

void blockio_verbose_ordered(blockio_dev_t *dev) {
	void *verbose_ding(blockio_info_t *bi) {
		int id = bi - dev->b->blockio_infos;
		char *state;
		char buf[16];
		switch (blockio_dev_get_macroblock_status(dev, id)) {
			case HAS_DATA:
				//state = "DATA";
				snprintf(buf, 16, "DATA %d", bi->no_nonobsolete);
				state = buf;
				break;
			case FREE:
				if (bi->seqno) state = "FREE";
				else state = "UNUSED";
				break;
			case SELECTFROM:
				state = "SELECT";
				break;
			default:
				assert(0);
		}

		VERBOSE("i=%3d, id=%3d, rev=%d/%d, seq=%4ld, nseq=%4ld %s%s",
				bi->ord.index, id, bi->layout_revision,
				bi->hard_layout_revision,
				bi->seqno, bi->next_seqno, state,
				(id == dev->tail_macroblock)?
				" >TAIL<":(dev->next_seqno - 1 == bi->seqno)?
				" >NEWEST<":"");
		return NULL;
	}

	dllarr_iterate(&dev->ordered, (dllarr_iterator_t)verbose_ding, NULL);
}

blockio_info_t *find_ordered_equal_or_first_after(
		blockio_dev_t *dev, uint16_t rev, uint64_t seqno) {
	void *compare_rev_nseq(blockio_info_t *bi) {
		if (bi->layout_revision < rev ||
				(bi->layout_revision == rev &&
				 bi->next_seqno < seqno)) return NULL;
		return bi;
	}

	return dllarr_iterate(&dev->ordered,
			(dllarr_iterator_t)compare_rev_nseq, NULL);
}

void add_to_ordered(blockio_info_t *bi) {
	void *compare_rev_nseq(blockio_info_t *old, blockio_info_t *new) {
		if (old->layout_revision == new->layout_revision &&
				old->next_seqno < new->next_seqno) return NULL;
		if (old->layout_revision < new->layout_revision) return NULL;
		return old;
	}

	void *tmp = dllarr_iterate(&bi->dev->ordered,
			(dllarr_iterator_t)compare_rev_nseq, bi);

	//VERBOSE("insert %p %d seq=%lld, nseq=%lld in ordered, at %p",
	//		bi, bi - bi->dev->b->blockio_infos, bi->seqno,
	//		bi->next_seqno, tmp);

	dllarr_insert(&bi->dev->ordered, bi, tmp);
}

/* put block in the 'in use' list, sort with sequence number */
static void add_to_used(blockio_info_t *bi) {
	void *compare_seqno(blockio_info_t *bi, uint64_t *seqno) {
		//VERBOSE("checking %d: goal %llu <= %llu",
		//		bi - bi->dev->b->blockio_infos,
		//		*seqno, bi->seqno);
		if (*seqno > bi->seqno) return NULL;
		else return bi;
	}
	assert(bi->no_indices);

	dllarr_insert(&bi->dev->used_blocks, bi,
			dllarr_iterate(&bi->dev->used_blocks,
				(dllarr_iterator_t)compare_seqno,
				&bi->seqno));
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

static void update_ordered(blockio_dev_t *dev, blockio_info_t *bi,
		uint64_t next_seqno, uint16_t layout_revision) {
	bi->next_seqno = next_seqno;
	bi->layout_revision = dev->layout_revision;
	add_to_ordered(dllarr_remove(&dev->ordered, bi));
}

void blockio_dev_select_next_macroblock(blockio_dev_t *dev, int first) {
	int number, different = 1, tmp2 = 0, tmp3;
	uint32_t next_seqno_diff = 0;

	assert(!dev->bi);

	dev->valid = 1;

	number = dev->macroblock_ref[random_peek(&dev->r, 0)];
	dev->bi = &dev->b->blockio_infos[number];
	dev->updated = 0;
	//VERBOSE("select next: id=%d next_seqno=%lld, bi->seqno=%lld,
	//	 	bi->next_seqno=%lld", number, dev->next_seqno,
	//	 	dev->bi->seqno, dev->bi->next_seqno);
	assert(dev->bi->next_seqno == 0 || dev->next_seqno == dev->bi->next_seqno);
	dev->bi->seqno = dev->next_seqno++;
	dev->bi->layout_revision = dev->layout_revision;

	do next_seqno_diff++;
	while (random_peek(&dev->r, next_seqno_diff) != random_peek(&dev->r, 0));

	update_ordered(dev, dev->bi, dev->bi->seqno +
			next_seqno_diff, dev->layout_revision);

#if 0
	VERBOSE("new block %d (local_id=%d) seqno=%lld next_seqno=%lld",
			number, random_peek(&dev->r, 0),
			dev->bi->seqno, dev->bi->next_seqno);
#endif

	if (first) {
		assert(blockio_dev_get_macroblock_status(dev, number) == FREE);
		dllarr_remove(&dev->free_blocks, dev->bi);
	} else {
		blockio_dev_change_macroblock_status(dev, number, SELECTFROM, FREE);
		dllarr_remove(&dev->selected_blocks, dev->bi);
	}

	while (different <= dev->keep_revisions) {
		tmp2++;
		tmp3 = dev->macroblock_ref[random_peek(&dev->r, tmp2)];
		if (last_diff(&dev->r, tmp2, &dev->valid)) {
			if (different != dev->keep_revisions) {
				if (tmp2 + 1 >= dev->random_len) {
					blockio_dev_change_macroblock_status(dev,
							tmp3,  FREE, SELECTFROM);
					dllarr_move(&dev->selected_blocks,
							&dev->free_blocks,
							&dev->b->blockio_infos[
							tmp3]);
				}
				assert(blockio_dev_get_macroblock_status(dev,
							tmp3) == SELECTFROM);
			}
			different++;
		}
		//VERBOSE("bla different=%d, tmp2=%d, tmp3=%d, status=%d, "
		//		"rl=%d", different, tmp2, tmp3,
		//		blockio_dev_get_macroblock_status(dev, tmp3),
		//		dev->random_len);
	}

	if (!dev->valid) {
		blockio_dev_change_macroblock_status(dev, number, FREE, SELECTFROM);
		dllarr_append(&dev->selected_blocks, dev->bi);
	}

	dev->tail_macroblock =
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
                return (priv->last == bi);
        }

        //printf("sns:");
        gcry_call(md_open, &priv.hd, GCRY_MD_SHA256, 0);
        dllarr_iterate(&dev->used_blocks, (dllarr_iterator_t)add_seqno, &priv);
        //printf("\n");
        memcpy(hash_res, gcry_md_read(priv.hd, 0), 32);
        gcry_md_close(priv.hd);

        return hash_res;
}

/*static */void recon(blockio_dev_t *dev, const char *id,
		uint16_t internal, uint64_t seqno) {
	VERBOSE("recon (%s) %3d(%3d) %4ld", id,
			dev->macroblock_ref[internal],
			internal, seqno);
}

static void load_tail_of_ordered_in_prng(blockio_dev_t *dev, uint64_t walking_seqno) {
	blockio_info_t *bi = dllarr_last(&dev->ordered);

	if (bi->next_seqno < walking_seqno) return;

	while (bi) {
		//VERBOSE("iterate id=%u c=%llu n=%llu r=%d", bi - dev->b->blockio_infos,
		//		bi->seqno, bi->next_seqno, bi->layout_revision);
		uint32_t index = dllarr_index(&dev->ordered, bi);
		uint64_t count = bi->next_seqno;

		//VERBOSE("next_seqno of current %llu", bi->next_seqno);
		random_push(&dev->r, bi->internal);

		bi = dllarr_prev(&dev->ordered, bi);

		if (bi->next_seqno < walking_seqno) {
			count -= walking_seqno;
			bi = NULL;
		} else {
			//VERBOSE("next_seqno of previous %llu", bi->next_seqno);
			count -= bi->next_seqno + 1;
		}

		while (count--) random_push(&dev->r,
				((blockio_info_t*)dllarr_nth(&dev->ordered,
					random_custom(&dev->r,
					       	index)))->internal);
	}
}

#if 0
static void initialize_resize_prng(mt_state *state,
	       	blockio_dev_t *dev, uint16_t rev) {
	uint8_t mesoblk[1<<dev->b->mesoblk_log];
	memset(state, 0, sizeof(*state));
	memset(mesoblk, 0, sizeof(mesoblk));
	assert(sizeof(state->statevec) <= sizeof(mesoblk));
	cipher_enc(dev->c, mesoblk, mesoblk, 0, rev, 0);
	mts_seedfull(state, (uint32_t*)mesoblk);
}

blockio_info_t *uptranslate(blockio_info_t *bi) {
	assert(bi);
	uint8_t newrev = bi->layout_revision + 1;
	blockio_dev_t *dev = bi->dev;
	blockio_dev_rev_t *rev = &dev->rev[dev->layout_revision - newrev];
	blockio_info_t *next, *test;
	mt_state state;
	int work = rev->work;
	uint64_t seqno = rev->seqno - 1; // allow easy entry in while loop

	initialize_resize_prng(&state, dev, newrev);

	//VERBOSE("%p %d %d %llu %d %d", bi, newrev, dev->layout_revision - newrev,
	//	       	seqno, rev->no_macroblocks, work);
	VERBOSE("translating from rev %d to rev %d, starting at %ld",
		       	bi->layout_revision, newrev, bi - dev->b->blockio_infos);

	next = find_ordered_equal_or_first_after(dev,
			bi->layout_revision, ULLONG_MAX);
	int count = (dllarr_index(&dev->ordered, next) -
			dllarr_index(&dev->ordered, bi));
	uint64_t reindex[count];

	assert(count);

	while (work) {
		do seqno++;
		while (rds_iuniform(&state, 0, rev->no_macroblocks) >= work);

		work--;

		test = find_ordered_equal_or_first_after(dev, newrev, seqno);
		if (test && test->next_seqno == seqno &&
				test->layout_revision == newrev) continue;

		/* keep the last 'count' values */
		memmove(&reindex[1], &reindex[0], sizeof(reindex[0])*(count - 1));
		reindex[0] = seqno;
	}

	count--;
	goto entry;

	while (count--) {
		bi = next;
entry:
		//VERBOSE("update %u from %llu to %llu", bi - dev->b->blockio_infos,
		//		bi->next_seqno, reindex[count]);
		next = dllarr_next(&dev->ordered, bi);
		assert(next);

		bi->next_seqno = reindex[count];
		bi->layout_revision++;
		add_to_ordered(dllarr_remove(&dev->ordered, bi));
	}

	//blockio_verbose_ordered(dev);

	/* return the first block of the next revision, usually this is
	 * 'next' but in some cases this is bi (because bi was the only
	 * one to be updated) */
	return ((next->layout_revision == bi->layout_revision &&
			next->next_seqno > bi->next_seqno) || next->layout_revision > bi->layout_revision)?bi:next;
}
#endif

void blockio_dev_init(blockio_dev_t *dev, blockio_t *b, cipher_t *c,
		const char *name) {
	int i, tmp_no_macroblocks = 0, tmp2 = 0;
	int no_different_selectblocks;
	uint64_t highest_seqno = 0;
	uint8_t hash[32];

	assert(b && c);
	assert(b->mesoblk_log < b->macroblock_log);
	bitmap_init(&dev->status, 2*b->no_macroblocks);
	dllarr_init(&dev->used_blocks, offsetof(blockio_info_t, ufs));
	dllarr_init(&dev->free_blocks, offsetof(blockio_info_t, ufs));
	dllarr_init(&dev->selected_blocks, offsetof(blockio_info_t, ufs));
	dllarr_init(&dev->ordered, offsetof(blockio_info_t, ord));

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
		VERBOSE("device \"%s\" is empty; no macroblocks found",
				dev->name);
		return;
	}

	uint16_t rebuild_prng[dev->random_len - 1];
	memset(&rebuild_prng[0], 0, sizeof(rebuild_prng));
	dllarr_t *fos;

	dev->macroblock_ref = ecalloc(dev->rev[0].no_macroblocks, sizeof(uint16_t));

	tmp_no_macroblocks = dev->rev[0].no_macroblocks;
	dev->rev[0].no_macroblocks = 0;

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

	assert(tmp_no_macroblocks == dev->rev[0].no_macroblocks);

	assert(tmp2 <= dev->random_len - 1);
	//VERBOSE("we have %d different blocks to rebuild prng", tmp2);
	dev->keep_revisions += tmp2 + 1;
	//VERBOSE("keep_revisions = %d", dev->keep_revisions);
	no_different_selectblocks = tmp2;
	random_rescale(&dev->r, dev->rev[0].no_macroblocks);

	/* first: fill in the blanks in rebuild_prng (if any) with random
	 * choices of the filled in argument */
	while (tmp2 < dev->random_len - 1)
		rebuild_prng[tmp2++] = rebuild_prng[
			random_custom(&dev->r, no_different_selectblocks)];

	/* shuffle */
	for (i = 0; i < tmp2 - 1; i++) {
		uint16_t xchg = rebuild_prng[i];
		uint16_t index;
		index = random_custom(&dev->r, tmp2 - i) + i;
		rebuild_prng[i] = rebuild_prng[index];
		rebuild_prng[index] = xchg;
		//VERBOSE("swapped %d and %d", i, index);
	}

	uint64_t walking_seqno = dev->next_seqno + dev->random_len;

	/* find first nonzero revision */
	blockio_info_t *bi = find_ordered_equal_or_first_after(dev, 0,
			ULLONG_MAX);

	if (bi->layout_revision + MACROBLOCK_HISTORY < dev->layout_revision)
		ecch_throw(ECCH_DEFAULT, "not enough information available "
				"to rebuild prng");

#if 0
	blockio_verbose_ordered(dev);

	for (i = 0; i < MACROBLOCK_HISTORY; i++)
		VERBOSE("bla %llu %u %u", dev->rev[i].seqno,
				dev->rev[i].no_macroblocks,
				dev->rev[i].work);
#endif

	/*while (bi->layout_revision < dev->layout_revision) bi = uptranslate(bi);*/

	load_tail_of_ordered_in_prng(dev, walking_seqno);

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
}

void blockio_dev_read_header(blockio_dev_t *dev, uint32_t no,
		uint64_t *highest_seqno) {
	assert(dev && dev->b && no < dev->b->no_macroblocks);
	blockio_info_t *bi = &dev->b->blockio_infos[no];
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
	cipher_dec(dev->c, BASE, BASE, 0, 0, 0);

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
	bi->hard_layout_revision =
		bi->layout_revision = binio_read_uint16_be(LAYOUT_REVISION);
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

	bi->no_nonobsolete = bi->no_indices = binio_read_uint32_be(NO_INDICES);
	bi->indices = ecalloc(dev->mmpm, sizeof(uint32_t));
	for (i = 1; i <= bi->no_indices; i++)
		bi->indices[i-1] = *(((uint32_t*)NO_INDICES) + i);

	VERBOSE("block %u (seqno=%lu) of \"%s\" has %d indices, "
			"next_seqno=%lu",
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
			no + 1, 0);
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

void blockio_dev_write_nonsense_macroblock(blockio_dev_t *dev, uint16_t no) {
	cipher_t c;
	int i;
	FILE *fp = fopen("/dev/urandom", "r");
	uint8_t mesoblk_in[1<<dev->b->mesoblk_log];
	uint8_t macroblock[1<<dev->b->macroblock_log];
	uint8_t key[32];
	if (!fp) ecch_throw(ECCH_DEFAULT, "unable to open /dev/urandom");
	if (fread(key, 32, 1, fp) != 1) ecch_throw(ECCH_DEFAULT, "unable to "
			"read random data");
	gcry_md_hash_buffer(GCRY_MD_SHA256, mesoblk_in, key, 32);
	cipher_init(&c, "CBC_ESSIV(AES256)", 1024, key, 32);
	fclose(fp);


	cipher_enc(&c, macroblock, mesoblk_in, 0, 0, 0);

	for (i = 0; i < dev->mmpm; i++)
		cipher_enc(&c, macroblock + ((i+1)<<dev->b->mesoblk_log),
				mesoblk_in, 0, i + 1, 0);

	cipher_free(&c);

	dev->b->write(dev->io, macroblock, no<<dev->b->macroblock_log,
			1<<dev->b->macroblock_log);
}

void blockio_dev_write_current_macroblock(blockio_dev_t *dev) {
	uint32_t id = dev->bi - dev->b->blockio_infos;
	int i;
	assert(dev->bi && id < dev->b->no_macroblocks);

	/* the current block is not on any list, put it on the correct one */
	if (dev->bi->no_indices) {
		blockio_dev_change_macroblock_status(dev,
				id, FREE, HAS_DATA);
		dllarr_append(&dev->used_blocks, dev->bi);
		dev->wasted_gc += dev->bi->no_indices_gc;
		dev->pre_emptive_gc += dev->bi->no_indices_preempt;
		dev->useful += (dev->bi->no_indices - dev->bi->no_indices_gc);
		dev->wasted_empty += dev->mmpm - dev->bi->no_indices;
	} else {
		dev->wasted_keep += dev->mmpm;
		if (blockio_dev_get_macroblock_status(dev, id) != SELECTFROM) {
			dllarr_append(&dev->free_blocks, dev->bi);
			if (!dev->updated) {
				DEBUG("zerstr block %u (seqno=%lu, "
						"next_seqno=%lu)", id,
		       				dev->bi->seqno,
					       	dev->bi->next_seqno);
				blockio_dev_write_nonsense_macroblock(dev, id);
				assert(!dev->bi->no_indices);
				assert(!dev->bi->no_nonobsolete);
				assert(!dev->bi->no_indices_gc);
				assert(!dev->bi->no_indices_preempt);
				dev->bi->seqno = 0;
				dev->bi->next_seqno = 0;
				dev->bi->layout_revision = 0;
				dev->bi->hard_layout_revision = 0;
				add_to_ordered(dllarr_remove(&dev->ordered,
							dev->bi));
				dev->bi = NULL;
				return;
			}
		}
	}

	DEBUG("write block %u (seqno=%lu, next_seqno=%lu)", id,
		       	dev->bi->seqno, dev->bi->next_seqno);
	dev->writes++;

	/* encrypt datablocks (also the unused ones) */
	for (i = 1; i <= dev->mmpm; i++)
		cipher_enc(dev->c, BASE + (i<<dev->b->mesoblk_log),
			BASE + (i<<dev->b->mesoblk_log), dev->bi->seqno, i, 0);

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
	binio_write_uint16_be(TAIL_MACROBLOCK, dev->tail_macroblock);
	binio_write_uint16_be(LAYOUT_REVISION, dev->layout_revision);
	dev->bi->hard_layout_revision = dev->layout_revision;
	binio_write_uint16_be(RESERVED_MACROBLOCKS, dev->reserved_macroblocks);
	binio_write_uint32_be(RANDOM_LEN, dev->random_len);
	binio_write_uint8(MACROBLOCK_LOG, dev->b->macroblock_log);
	binio_write_uint8(MESOBLOCK_LOG, dev->b->mesoblk_log);

	for (i = 0; i < MACROBLOCK_HISTORY; i++) {
		binio_write_uint64_be(REV_SEQNOS + 8*i,
				dev->rev[i].seqno);
		binio_write_uint16_be(NO_MACROBLOCKS + 2*i,
				dev->rev[i].no_macroblocks);
		binio_write_uint16_be(REVISION_WORK + 2*i,
				dev->rev[i].work);
	}

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
	cipher_enc(dev->c, BASE, BASE, 0, 0, 0);

	dev->b->write(dev->io, BASE, id<<dev->b->macroblock_log,
			1<<dev->b->macroblock_log);

	//dev->next_seqno = dev->bi->seqno + 1;
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

void blockio_dev_update_statistics(blockio_dev_t *dev) {
	FATAL("not implemented");
#if 0
	blockio_info_t *bi, *next;
	uint64_t walking_seqno, stop_below;
	int work;
	//mt_state state;


	stop_below = walking_seqno = dev->next_seqno + dev->random_len;
	VERBOSE("reconstruction starts at seqno %lu", walking_seqno);
	dev->rev[0].seqno = walking_seqno;

	bi = find_ordered_equal_or_first_after(dev, dev->layout_revision - 1,
                	walking_seqno);

	work = dllarr_count(&dev->ordered) - dllarr_index(&dev->ordered, bi);
	VERBOSE("work=%d", work);
	dev->rev[0].work = work;

	/* produce seed for PRNG, these actions must be reproduced
 	 * by the loader */

	//initialize_resize_prng(&state, dev, dev->layout_revision);

	walking_seqno--;

	while (work) {
        	next = dllarr_next(&dev->ordered, bi);

		do walking_seqno++;
        	while (rds_iuniform(&state, 0,
					dev->rev[0].no_macroblocks) >= work);

		work--;

        	bi->next_seqno = walking_seqno;
        	bi->layout_revision++;

        	add_to_ordered(dllarr_remove(&dev->ordered, bi));

        	bi = next;
	}

	// safeguard important contents of prng
	uint16_t random_store[dev->random_len];
	while (dev->random_len--)
		random_store[dev->random_len] = random_pop(&dev->r);

	// load PRNG with just created numbers
	load_tail_of_ordered_in_prng(dev, stop_below);

	// restore PRNG
	while (2*dev->random_len < sizeof(random_store))
		random_push(&dev->r, random_store[dev->random_len++]);
#endif
}

int blockio_dev_free_macroblocks(blockio_dev_t *dev, uint16_t size) {
	blockio_info_t *bi = dllarr_first(&dev->free_blocks);
	int count = 0;
	VERBOSE("remove %d blocks", size);
	while (bi) {
		uint16_t id = bi - dev->b->blockio_infos;
		VERBOSE("%u %lu %s", id, bi->seqno,
			       	(id == bi->dev->tail_macroblock)?" TAIL":"");
		if (bi->seqno == 0 && id != dev->tail_macroblock) count++;

		bi = dllarr_next(&dev->free_blocks, bi);
	}
	VERBOSE("%d blocks available for removal, request to remove %d", count,
			size);

	if (size > count) return -1;

	memmove(&dev->rev[1], &dev->rev[0],
			(MACROBLOCK_HISTORY - 1)*
			sizeof(dev->rev[0]));

	while (size) {
	}

	return 0;
}

int blockio_dev_allocate_macroblocks(blockio_dev_t *dev, uint16_t size) {
	// 16 bit arithmetic
	assert(dev->rev[0].no_macroblocks < dev->rev[0].no_macroblocks + size);

	int i, no_freeb = 0;
	blockio_info_t *bi;
	uint16_t freeb[dev->b->no_macroblocks];
	uint16_t no, select = 0;
	void *tmp;
	assert(dev->b->no_macroblocks < 65536);
	for (i = 0; i < dev->b->no_macroblocks; i++)
		if (!dev->b->blockio_infos[i].dev) freeb[no_freeb++] = i;

	VERBOSE("we have %d free blocks to choose from", no_freeb);

	if (size > no_freeb) return -1; // not enough blocks

	tmp = realloc(dev->macroblock_ref,
			sizeof(uint16_t)*(size + dev->rev[0].no_macroblocks));
	if (!tmp) return -1; // out of memory
	dev->macroblock_ref = tmp;

	memmove(&dev->rev[1], &dev->rev[0],
		       	(MACROBLOCK_HISTORY - 1)*
			sizeof(dev->rev[0]));

	while (size) {
		no = random_custom(&dev->b->r, no_freeb);
		select = freeb[no];
		//VERBOSE("select nr %d, %d", no, select);
		bi = &dev->b->blockio_infos[select];
		bi->dev = dev;

		// add to our array
		bi->internal = dev->rev[0].no_macroblocks;
		dev->macroblock_ref[dev->rev[0].no_macroblocks++] = select;

		// mark as FREE and assert() that is was NOT_ALLOCATED
		blockio_dev_change_macroblock_status(dev, select,
				NOT_ALLOCATED, FREE);
		dllarr_append(&dev->free_blocks, bi);

		add_to_ordered(bi);

		no_freeb--;
		memmove(&freeb[no], &freeb[no+1], sizeof(freeb[0])*(no_freeb - no));
		bi->no_indices = 0;
		bi->no_nonobsolete = 0;
		bi->no_indices_gc = 0;
		bi->no_indices_preempt = 0;
		bi->indices = ecalloc(dev->mmpm, sizeof(uint32_t));
		size--;
	}

	dev->layout_revision++;

	random_rescale(&dev->r, dev->rev[0].no_macroblocks);

	if (dev->layout_revision != 1) blockio_dev_update_statistics(dev);

	return 0;
}

