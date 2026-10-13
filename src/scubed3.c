/* scubed3.c - deniable encryption resistant to surface analysis
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
#include <sys/poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/user.h>
#undef NDEBUG /* we need sanity checking */
#include <fuse/fuse_opt.h>

#include <assert.h>
#include "config.h"
#include "scubed3.h"
#include "gcry.h"
#include "blockio.h"
#include "verbose.h"
#include "dllarr.h"
#include "util.h"
#include "cipher.h"
#include "hashtbl.h"
#include "fuse_io.h"
#include "control.h"

#define ID	(index>>l->mesobits)
#define NO	(index&l->mesomask)

void obsolete_mesoblk(scubed3_t *l, blockio_info_t *bi, uint32_t no) {
	assert(bi);
	assert(bi->no_nonobsolete);
	bi->no_nonobsolete--;

	/* if the whole block is obsolete, remove it from the active list */
	if (!bi->no_nonobsolete) {
		//WARNING("move from active list is not implemented");
		bi->no_indices = 0;
	}
}

void obsolete_mesoblk_byidx(scubed3_t *l, uint32_t index) {
	if (index != 0xFFFFFFFF)
		obsolete_mesoblk(l, &l->dev->b->blockio_infos[ID], NO);
}

static inline void update_block_indices(scubed3_t *l, uint32_t offset,
		uint32_t id, uint16_t  no) {
	l->block_indices[offset] = (id<<l->mesobits) + no;
}

/* add a reference to the mesoblock that was put in the next free slot of
 * the current block, corrupt is the mask of its corrupt parts (they stay
 * marked as corrupt), the hash is computed when the block is written */
static void add_blockref(scubed3_t *l, uint32_t offset, int corrupt) {
	l->dev->bi->indices[l->dev->bi->no_indices] = offset;
	l->dev->bi->hashes[l->dev->bi->no_indices] = corrupt;
	update_block_indices(l, offset, id(l->dev->bi),
			l->dev->bi->no_indices);
	l->dev->bi->no_indices++;
}

static inline char *mesoblk(scubed3_t *l, uint16_t no) {
	assert(no < l->dev->b->mmpm);
	return l->dev->tmp_macroblock + ((no+1)<<l->dev->b->mesoblk_log);
}

/* is a tombstone for logical mesoblock L needed? it is if L is discarded
 * and a version of L is in the index of a block on disk */
static int tomb_needed(scubed3_t *l, uint32_t L) {
	return L < l->no_block_indices &&
		l->block_indices[L] == 0xFFFFFFFF &&
		l->dev->refcount[L];
}

/* carry the tombstones in slot k of bi to the current block (if they
 * are still needed), returns -1 if they don't fit */
static int absorb_tombs(scubed3_t *l, blockio_info_t *bi, uint32_t k) {
	uint32_t tombs[l->dev->tombs_per_mesoblk], n, i, needed = 0;

	n = blockio_dev_read_tombs(l->dev, bi, k, tombs);
	for (i = 0; i < n; i++)
		if (tomb_needed(l, tombs[i])) tombs[needed++] = tombs[i];

	if (!blockio_dev_tomb_fits(l->dev, needed)) return -1;

	for (i = 0; i < needed; i++) blockio_dev_add_tomb(l->dev, tombs[i]);

	/* the slot is done, it does not count as live anymore (the value
	 * in the index is not used for the refcounts) */
	bi->indices[k] = BLOCKIO_DEAD;
	obsolete_mesoblk(l, bi, k);

	return 0;
}

/* copy the live mesoblocks and the needed tombstones of bi to the current
 * block, until the current block is full */
static void absorb_block(scubed3_t *l, blockio_info_t *bi) {
	int k;
	uint32_t index;
	/* the current block can be its own tail (it is rescheduled for
	 * the next seqno), its data is in RAM and travels with it */
	if (bi && bi != l->dev->bi &&
			blockio_dev_get_macroblock_status(bi) == USED) {

		for (k = 0; k < bi->no_indices; k++) {
			if (bi->indices[k] == BLOCKIO_TOMBSTONES) {
				if (absorb_tombs(l, bi, k)) break;
				continue;
			}

			if (bi->indices[k] >= l->no_block_indices) continue;

			if (!blockio_dev_free_slots(l->dev)) break;

			index = l->block_indices[bi->indices[k]];
			if (index != 0xFFFFFFFF &&
					&l->dev->b->blockio_infos[ID] == bi) {
				/* a corrupt mesoblock is copied as well, but
				 * its corrupt parts stay marked as corrupt */
				add_blockref(l, bi->indices[k],
						blockio_dev_read_mesoblk(l->dev,
						mesoblk(l, l->dev->bi->no_indices),
						id(bi), k));

				obsolete_mesoblk(l, bi, k);
			}
		}
	}
}

void copy_old_block_to_current(scubed3_t *l) {
	blockio_info_t *victim, *tail = l->dev->tail_macroblock;

	/* the tail is always absorbed completely, it is overwritten next
	 * (the current block is empty, and the tombstones that are carried
	 * do not need more slots than they had in the tail) */
	absorb_block(l, tail);
	if (tail && tail != l->dev->bi && tail->no_nonobsolete)
		FATAL("tail of current block does not fit in it");

	/* if we are shrinking, use the remaining room to drain the block
	 * with the least live data (the victim); when it is empty, it is
	 * removed from the partition; blocks are filled completely,
	 * otherwise a partition that is completely written cannot reach
	 * its target size (if the current block is filled, a pending write
	 * waits in select_new_macroblock until there is room) */
	if (!l->dev->pending_removals) return;

	/* a block that is its own next block removes nothing: if its next
	 * write is interrupted, the state falls back to the block before it
	 * (see blockio_dev_release_removed) */
	if (l->dev->self_tail) return;

	if (!(victim = blockio_dev_find_victim(l->dev))) return;

	absorb_block(l, victim);

	if (!victim->no_nonobsolete) blockio_dev_remove_block(l->dev, victim);
}

/*
static void pre_emptive_gc(scubed3_t *l) {
	WARNING("pre_emptive_gc not implemented");
#if 0
	blockio_info_t *bi = dllarr_first(&l->dev->ordered);
	assert(l->output_initialized);
	assert(!l->dev->bi->no_indices_preempt);

	while ((bi = dllarr_next(&l->dev->ordered, bi)) &&
			l->dev->bi->no_indices < l->dev->b->mmpm) {
		uint32_t ctr = 0, index;

		if (bi == l->dev->bi) break;

		//VERBOSE("searching %d %lu %d", bi - l->dev->b->blockio_infos,
		//		bi->seqno, bi->no_nonobsolete);

		if (!bi->no_nonobsolete) continue;

		while (ctr < bi->no_indices &&
				l->dev->bi->no_indices < l->dev->b->mmpm) {
			index = l->block_indices[bi->indices[ctr]];
			if (ID != id(bi)) {
				ctr++;
				continue;
			}

			blockio_dev_read_mesoblk(l->dev, mesoblk(l,
					l->dev->bi->no_indices), id(bi), ctr);

			add_blockref(l, bi->indices[ctr]);

			obsolete_mesoblk(l, bi, ctr);

			//VERBOSE("pre-emptive gc of %u of %d", index, id(bi));
			l->dev->bi->no_indices_preempt++;

			ctr++;
		}
	}

	if (l->dev->bi->no_indices_preempt) l->dev->updated = 1;

	//DEBUG("saved %d additional indices", l->dev->bi->no_indices_preempt);
#endif
}
*/

// FIXME the following two functions look almost the same...  
/* select new blocks until the current block has room, or until the GC
 * has moved mesoblock mesoff (that is about to be written) to the current
 * block (then it can be updated in place, this guarantees progress if all
 * blocks are completely filled with live data) */
void select_new_macroblock(scubed3_t *l, uint32_t mesoff) {
	uint32_t index;
	assert(l->output_initialized);
	//pre_emptive_gc(l);
	do {
		blockio_dev_write_current_and_select_next_macroblock(l->dev);
		copy_old_block_to_current(l);
		if (l->dev->tail_macroblock) DEBUG("new block %lu (seqno=%lu) has %u mesoblocks due "
				"to GC of block %u",
				id(l->dev->bi), l->dev->bi->seqno,
				l->dev->bi->no_indices,
				blockio_get_macroblock_index(
					l->dev->tail_macroblock));
		index = l->block_indices[mesoff];
	} while (!blockio_dev_free_slots(l->dev) &&
			(index == 0xFFFFFFFF || ID != id(l->dev->bi)));
}

void initialize_output(scubed3_t *l) {
	if (!l->output_initialized) {
		copy_old_block_to_current(l);
		if (l->dev->tail_macroblock) DEBUG("new block %lu (seqno=%lu) has %u mesoblocks due "
				"to GC of block %u",
				id(l->dev->bi), l->dev->bi->seqno,
				l->dev->bi->no_indices,
				blockio_get_macroblock_index(
					l->dev->tail_macroblock));

		l->output_initialized = 1;
	}
}

/* if the current block is its own next block (drawn twice), it is written out
 * empty first; that write must not carry a change of the state of the
 * partition (like a resize): if the next write (of the same block) is
 * interrupted, the state falls back to the block before it, so it is done
 * before such a change (the write is the same as the one that was done at
 * once when the block was selected, before writes were one per turn) */
void scubed3_settle(scubed3_t *l) {
	while (l->dev->bi && l->dev->self_tail) scubed3_cycle(l);
}

void scubed3_cycle(scubed3_t *l) {
	/* there is no current block if all blocks were removed */
	if (!l->dev->bi) return;

	/* output ONE block, run GC if possible and useful */
	if (l->output_initialized) { /* output is initialized */
		//pre_emptive_gc(l);
		blockio_dev_write_current_macroblock(l->dev);
		blockio_dev_select_next_macroblock(l->dev);
		l->output_initialized = 0;
	} else {
		copy_old_block_to_current(l);
		l->output_initialized = 1;
		//if (!l->cycle_goal) pre_emptive_gc(l);
		blockio_dev_write_current_macroblock(l->dev);
		blockio_dev_select_next_macroblock(l->dev);
		l->output_initialized = 0;
	}
}

void *replay(blockio_info_t *bi, scubed3_t *l) {
	uint32_t k, index, n, i, tombs[l->dev->tombs_per_mesoblk];

	VERBOSE("replay at seqno=%ld (%d indices)", bi->seqno, bi->no_indices);

	/* first the tombstones: they discard the versions in older blocks
	 * (data in this block is newer than the tombstones in it), the
	 * slots of tombstones stay live until the GC carries them */
	for (k = 0; k < bi->no_indices; k++) {
		if (bi->indices[k] != BLOCKIO_TOMBSTONES) continue;
		n = blockio_dev_read_tombs(l->dev, bi, k, tombs);
		for (i = 0; i < n; i++) {
			if (tombs[i] >= l->no_block_indices ||
					blockio_dev_is_trimmed(l->dev,
						bi->seqno, tombs[i])) continue;
			obsolete_mesoblk_byidx(l, l->block_indices[tombs[i]]);
			l->block_indices[tombs[i]] = 0xFFFFFFFF;
		}
	}

	for (k = 0; k < bi->no_indices; k++) {
		if (bi->indices[k] == BLOCKIO_TOMBSTONES) continue;
		//VERBOSE("k=%u, bi->indices[k]=%u", k, bi->indices[k]);
		/* references past the end of the partition, or to mesoblocks
		 * that were discarded by shrinking after this block was
		 * written, are dead */
		if (bi->indices[k] >= l->no_block_indices ||
				blockio_dev_is_trimmed(l->dev, bi->seqno,
					bi->indices[k])) {
			obsolete_mesoblk(l, bi, k);
			continue;
		}
		index = l->block_indices[bi->indices[k]];
		assert(id(bi) != ID);
		obsolete_mesoblk_byidx(l, index);
		update_block_indices(l, bi->indices[k], id(bi), k);
	}

	return NULL;
}

#if 0
void debug_stuff(scubed3_t *l) {
	void *que(blockio_info_t *b) {
		VERBOSE("block %lu owned by \"%s\" (seqno %lu) has %u used "
				"mesoblocks", id(b), b->dev->name,
				b->seqno, b->no_nonobsolete);
		return NULL;
	}
	VERBOSE("debug stuff-----");
	dllarr_iterate(&l->dev->used_blocks, (dllarr_iterator_t)que, NULL);
	VERBOSE("end-------------");
}
#endif

void scubed3_free(scubed3_t *l) {
	//VERBOSE("freeing scubed3 partition");
	//if (l->output_initialized) pre_emptive_gc(l);

	/* the current block will be written if it is updated (this
	 * happens also after a resize), the tail must be absorbed first */
	/* write out the current block; removed blocks are released after a
	 * write, when the next block is drawn without them (see
	 * blockio_dev_release_removed), they are given away when the
	 * partition is closed, so write until they are released */
	while (l->dev && l->dev->bi && (l->dev->updated ||
				dllarr_count(&l->dev->release)))
		scubed3_cycle(l);

	free(l->block_indices);
}

/* make everything that was written to the partition durable: write out
 * the current block if it contains new data (the write is followed by a
 * sync of the base device) and select the next block */
void scubed3_fsync(scubed3_t *l) {
	if (!l->dev->bi || !l->dev->updated) return;

	initialize_output(l);
	blockio_dev_write_current_macroblock(l->dev);
	blockio_dev_select_next_macroblock(l->dev);
	l->output_initialized = 0;
}

/* write out the current block (if it contains anything), afterwards
 * there is no current block */
void scubed3_flush(scubed3_t *l) {
	if (!l->dev->bi) return;

	if (!l->dev->updated && !l->dev->bi->no_indices) return;

	initialize_output(l);
	blockio_dev_write_current_macroblock(l->dev);
	l->output_initialized = 0;
}

void scubed3_reinit(scubed3_t *l) {
	uint32_t old_no_block_indices;

	old_no_block_indices = l->no_block_indices;

	l->no_block_indices = blockio_dev_usable_macroblocks(l->dev)*
		l->dev->b->mmpm;

	if (old_no_block_indices == l->no_block_indices) return;

	/* obsolete all mesoblocks beyond end of device (if shrinking),
	 * the current block must have been flushed, otherwise the
	 * obsoleted references from the current block would be counted
	 * as live when it is written */
	assert(old_no_block_indices < l->no_block_indices ||
			!l->dev->bi || !l->dev->bi->no_indices);
	while (old_no_block_indices > l->no_block_indices)
		obsolete_mesoblk_byidx(l,
				l->block_indices[--old_no_block_indices]);

	VERBOSE("scubed3_reinit %s", old_no_block_indices < l->no_block_indices?
			"enlarge":"shrink");

	/* realloc of 0 elements may return NULL */
	l->block_indices = erealloc(l->block_indices,
			l->no_block_indices?l->no_block_indices:1,
			sizeof(uint32_t));

	/* mark all mesoblocks beyond old end of device free */
	while (old_no_block_indices < l->no_block_indices)
		l->block_indices[old_no_block_indices++] = 0xFFFFFFFF;

	blockio_dev_resize_refcount(l->dev, l->no_block_indices);
}

void scubed3_init(scubed3_t *l, blockio_dev_t *dev) {
	int i;
	blockio_info_t *bi;
	assert(l && dev);

	l->dev = dev;

	l->mesobits = (dev->b->macroblock_log - dev->b->mesoblk_log);
	l->mesomask = 0xFFFFFFFF>>(32 - l->mesobits);

	l->no_block_indices = blockio_dev_usable_macroblocks(dev)*
		dev->b->mmpm;

	l->block_indices = ecalloc(l->no_block_indices?l->no_block_indices:1,
			sizeof(uint32_t));

	for (i = 0; i < l->no_block_indices; i++)
		l->block_indices[i] = 0xFFFFFFFF;

	/* count the indices on disk (all blocks in the pool) */
	blockio_dev_resize_refcount(dev, l->no_block_indices);

	if (!dev->no_macroblocks) return;

	VERBOSE("%d block(s) to replay", dllarr_count(&dev->replay));

	/* the most recent block absorbed the block that is written next (its
	 * tail), also the tombstones that were still needed: the slots of
	 * tombstones in the tail are done (if they would count as live, the
	 * tail would look like a block with live data; and if the write of
	 * the tail was interrupted, its old index may still be there while
	 * the slots are partly overwritten); replay obsoletes them */
	bi = dllarr_last(&dev->replay);
	if (dev->j.next && dev->j.next != bi) {
		uint32_t k;
		bi = dev->j.next;
		for (k = 0; k < bi->no_indices; k++)
			if (bi->indices[k] == BLOCKIO_TOMBSTONES)
				bi->indices[k] = BLOCKIO_DEAD;
	}

	dllarr_iterate(&dev->replay, (dllarr_iterator_t)replay, l);

	/* the replayed blocks are in the pool of the juggler already */
	while ((bi = dllarr_last(&dev->replay)))
		dllarr_remove(&dev->replay, bi);
}

/* check the hashes of all live mesoblocks of the partition, report() is
 * called for every corrupt one (with its offset in mesoblocks and the mask
 * of its corrupt parts), returns the number of live mesoblocks that were
 * checked */
uint32_t scubed3_check(scubed3_t *l, void (*report)(void*, uint32_t, int),
		void *priv) {
	blockio_info_t *bi;
	char *live = ecalloc(l->dev->b->mmpm, 1);
	uint32_t i, k, index, checked = 0, no_live;

	for (i = 0; i < juggler_count(&l->dev->j); i++) {
		bi = juggler_nth(&l->dev->j, i);

		/* which mesoblocks of this block are live? */
		memset(live, 0, l->dev->b->mmpm);
		no_live = 0;
		for (k = 0; k < bi->no_indices; k++) {
			if (bi->indices[k] >= l->no_block_indices) continue;
			index = l->block_indices[bi->indices[k]];
			if (index == 0xFFFFFFFF || ID != id(bi) || NO != k)
				continue;
			live[k] = 1;
			no_live++;
		}
		if (!no_live) continue;
		checked += no_live;

		/* the current block is in RAM, only the corrupt parts of
		 * the mesoblocks that were copied into it are known */
		if (bi == l->dev->bi) {
			for (k = 0; k < bi->no_indices; k++)
				if (live[k] && bi->hashes[k]&BLOCKIO_CORRUPT_MASK)
					live[k] = 2 + (bi->hashes[k]&
						BLOCKIO_CORRUPT_MASK);
		} else blockio_check_data_hashes(bi, live);

		for (k = 0; k < bi->no_indices; k++)
			if (live[k] >= 2) report(priv, bi->indices[k],
					live[k] - 2);
	}

	free(live);

	return checked;
}

int blockio_dev_fake_mesoblk_part(blockio_dev_t *dev, void *addr,
		uint32_t id, uint32_t no, uint32_t offset, uint32_t size) {
	memset(addr, 0, size);
	return 0;
}

/* returns the mask of corrupt parts of the old version of the mesoblock
 * (if it was read) */
int do_cow(scubed3_t *l, uint32_t index, uint32_t muoff,
		uint32_t size, void *addr) {
	int corrupt = 0;
	int (*readorfake)(blockio_dev_t*, void*, uint32_t, uint32_t,
			uint32_t, uint32_t) = blockio_dev_fake_mesoblk_part;

	if (index != 0xFFFFFFFF) readorfake = blockio_dev_read_mesoblk_part;

	/* read the parts of the  mesoblock we don't modify from disk
	 * otherwise, set it to zero */
	if (muoff) corrupt |= readorfake(l->dev, addr, ID, NO, 0, muoff);
	if (1<<l->dev->b->mesoblk_log > size + muoff)
		corrupt |= readorfake(l->dev, addr + muoff + size, ID, NO,
				muoff + size, (1<<l->dev->b->mesoblk_log) -
				size - muoff);

	return corrupt;
}

/* the mask of the parts of a mesoblock that are touched by [off, off+size),
 * and of the parts that are completely covered by it */
static int parts_touched(scubed3_t *l, uint32_t off, uint32_t size) {
	uint8_t log = l->dev->b->mesoblk_log - 2; /* BLOCKIO_PARTS == 4 */

	return ((1<<(((off + size - 1)>>log) + 1)) - 1)&~((1<<(off>>log)) - 1);
}

static int parts_covered(scubed3_t *l, uint32_t off, uint32_t size) {
	uint8_t log = l->dev->b->mesoblk_log - 2; /* BLOCKIO_PARTS == 4 */
	uint32_t first = (off + (1<<log) - 1)>>log, end = (off + size)>>log;

	if (end <= first) return 0;

	return ((1<<end) - 1)&~((1<<first) - 1);
}

int do_write(scubed3_t *l, uint32_t mesoff, uint32_t muoff, uint32_t size,
		char *in) {
	uint32_t index;
	int touched, covered, corrupt;
	assert(muoff + size <= 1<<l->dev->b->mesoblk_log);
	void *addr;
	/* three possibilities:
	 * 1. the block is currently in RAM, we update it
	 * 2. the block was never written, we add it to RAM
	 * 3. the block is on disk, we obsolete it and add it to RAM
	 * the current block is marked as updated after the data is in it,
	 * selecting a new block resets the updated flag */

	initialize_output(l);

	/* read the index after initialize_output, it may have moved
	 * the mesoblock from the tail to the current block */
	index = l->block_indices[mesoff];

	if (ID != id(l->dev->bi)) {
		/* could be that the new block is full after
		 * garbage collecting (depends on the way the
		 * to-be-freed block is selected); the old version of
		 * the mesoblock stays live until the new version is
		 * in the current block, so one of them is always on disk */
		if (!blockio_dev_free_slots(l->dev))
			select_new_macroblock(l, mesoff);

		index = l->block_indices[mesoff];
	}

	/* a corrupt part of a mesoblock is repaired by writing it
	 * completely (as a disk remaps an unreadable sector when it is
	 * written), a write that covers it partially would need its
	 * contents, it fails with EIO */
	touched = parts_touched(l, muoff, size);
	covered = parts_covered(l, muoff, size);
	if (ID == id(l->dev->bi)) { /* in RAM */
		if (l->dev->bi->hashes[NO]&touched&~covered) return -EIO;
		l->dev->bi->hashes[NO] &= ~(uint64_t)covered;
	}

	addr = mesoblk(l, (ID == id(l->dev->bi))?NO:l->dev->bi->no_indices);
	memcpy(addr + muoff, in, size);

	/* if not, the mesoblock is in RAM */
	if (ID != id(l->dev->bi)) { /* we are on disk */
		/* if we do not write the complete mesoblock, read the
		 * other parts from disk (if possible) or zero them, the
		 * old version is still on disk: its block can only have
		 * been reused after it was moved to another block; the
		 * slot we wrote to is not used yet, so if we fail
		 * nothing has changed */
		corrupt = do_cow(l, index, muoff, size, addr);
		if (corrupt&touched&~covered) return -EIO;

		/* mark old mesoblock obsolete, if there is one */
		obsolete_mesoblk_byidx(l, index);

		/* add new reference, the parts that were not written
		 * may still be corrupt */
		add_blockref(l, mesoff, corrupt&~covered);
	}

	l->dev->updated = 1;

	return 0;
}

/* make room for one tombstone in the current block, this may write the
 * current block (like a write when the current block is full) */
static void tomb_room(scubed3_t *l) {
	while (!blockio_dev_tomb_fits(l->dev, 1)) {
		blockio_dev_write_current_and_select_next_macroblock(l->dev);
		copy_old_block_to_current(l);
	}
}

/* remove slot no from the current block (its mesoblock was discarded
 * before the block was written), the last slot takes its place */
static void remove_slot(scubed3_t *l, uint32_t no) {
	blockio_info_t *bi = l->dev->bi;
	uint32_t last = bi->no_indices - 1;

	assert(no <= last);
	if (no != last) {
		memcpy(mesoblk(l, no), mesoblk(l, last),
				1<<l->dev->b->mesoblk_log);
		bi->indices[no] = bi->indices[last];
		bi->hashes[no] = bi->hashes[last];
		if (bi->indices[no] < l->no_block_indices)
			update_block_indices(l, bi->indices[no], id(bi), no);
	}
	bi->no_indices--;
}

/* the versions of L on disk that are not in the current block (the current
 * block replaces its own old version when it is written) */
static uint32_t refs_elsewhere(scubed3_t *l, uint32_t L) {
	uint32_t refs = l->dev->refcount[L], k;

	if (refs == 1) for (k = 0; k < l->dev->no_old_ondisk; k++)
		if (l->dev->old_ondisk[k] == L) return 0;

	return refs;
}

/* discard logical mesoblock L: it reads as zeroes from now on; versions of
 * L on disk get a tombstone in the current block, so that they are not
 * replayed when the partition is opened again (if the tombstone would not
 * fit, the current block is written first: the discard and its tombstone
 * must be written together, otherwise an older version of L could become
 * the most recent one on disk) */
static void discard_mesoblk(scubed3_t *l, uint32_t L) {
	uint32_t index;

	if (l->block_indices[L] == 0xFFFFFFFF) return;

	/* make room for the tombstone; if L is (or gets, by the GC) in the
	 * current block, its slot makes room (a full partition makes
	 * progress this way) */
	for (;;) {
		index = l->block_indices[L];
		if (ID == id(l->dev->bi) ||
				blockio_dev_tomb_fits(l->dev, 1)) break;
		blockio_dev_write_current_and_select_next_macroblock(l->dev);
		copy_old_block_to_current(l);
	}

	if (ID == id(l->dev->bi)) remove_slot(l, NO);
	else obsolete_mesoblk(l, &l->dev->b->blockio_infos[ID], NO);
	l->block_indices[L] = 0xFFFFFFFF;

	if (refs_elsewhere(l, L)) blockio_dev_add_tomb(l->dev, L);

	l->dev->updated = 1;
}

/* write zeroes to [off, end) */
static int zero_range(scubed3_t *l, uint64_t off, uint64_t end) {
	char zero[1<<l->dev->b->mesoblk_log];
	uint64_t n;
	int ret;

	memset(zero, 0, sizeof(zero));

	for (; off < end; off += n) {
		n = sizeof(zero) - (off&(sizeof(zero) - 1));
		if (n > end - off) n = end - off;
		if ((ret = do_req(l, SCUBED3_WRITE, off, n, zero)) < 0)
			return ret;
	}

	return 0;
}

/* discard (punch a hole in) a range of the partition: whole mesoblocks are
 * discarded, the parts of the mesoblocks at the edges are zeroed */
int scubed3_discard(scubed3_t *l, uint64_t offset, uint64_t len) {
	uint8_t log = l->dev->b->mesoblk_log;
	uint64_t end = ((uint64_t)l->no_block_indices)<<log;
	uint64_t first, last;
	int ret;

	if (offset >= end || !len) return 0;
	if (len > end - offset) len = end - offset;

	initialize_output(l);

	/* the whole mesoblocks in the range are [first, last) */
	first = (offset + (1<<log) - 1)>>log;
	last = (offset + len)>>log;

	if (first >= last) return zero_range(l, offset, offset + len);

	if ((ret = zero_range(l, offset, first<<log)) ||
			(ret = zero_range(l, last<<log, offset + len)))
		return ret;

	for (; first < last; first++) discard_mesoblk(l, first);

	return 0;
}

/* blocks that were added to the partition with an index of ours on disk
 * (see blockio_dev_check_readded) need tombstones for the discarded
 * mesoblocks in them */
void scubed3_readded(scubed3_t *l) {
	blockio_info_t *bi;
	uint32_t i, k, L;

	if (!l->dev->bi) return;

	initialize_output(l);

	for (i = 0; i < juggler_count(&l->dev->j); i++) {
		bi = juggler_nth(&l->dev->j, i);
		for (k = 0; bi->readded && k < bi->no_ondisk; k++) {
			/* the indices on disk of the current block are in
			 * old_ondisk */
			L = (bi == l->dev->bi)?l->dev->old_ondisk[k]:
				bi->indices[k];
			if (!tomb_needed(l, L)) continue;
			tomb_room(l);
			/* if bi was written meanwhile, its index is new */
			if (!bi->readded) break;
			blockio_dev_add_tomb(l->dev, L);
			l->dev->updated = 1;
		}
		bi->readded = 0;
	}
}

int do_read(scubed3_t *l, uint32_t mesoff, uint32_t muoff, uint32_t size,
		char *out) {
	uint32_t index = l->block_indices[mesoff];
	/* three possibilities:
	 * 1. the block is currently in RAM
	 * 2. the block was never written, we return zeroes
	 * 3. the block is on disk */

	if (index == 0xFFFFFFFF) /* never written */
		memset(out, 0, size);
	else if (ID == id(l->dev->bi)) { /* in RAM */
		if (l->dev->bi->hashes[NO]&parts_touched(l, muoff, size))
			return -EIO;
		memcpy(out, mesoblk(l, NO) + muoff, size);
	} else if (blockio_dev_read_mesoblk_part(l->dev, /* on disk */
				out, ID, NO, muoff, size)&
			parts_touched(l, muoff, size)) return -EIO;

	return 0;
}

/* returns the number of bytes read or written, or -errno; a request that
 * extends past the end of the partition is shortened (so reading at the end
 * returns 0, end of file), a write that starts at the end fails with ENOSPC
 * (the partition cannot grow by writing to it); if an error occurs, the
 * whole request fails (a short count would be taken for end of file) */
int do_req(scubed3_t *l, scubed3_io_t cmd, uint64_t r_offset, size_t size,
		char *buf) {
	assert(cmd == SCUBED3_READ || cmd == SCUBED3_WRITE);
	uint64_t end = ((uint64_t)l->no_block_indices)<<l->dev->b->mesoblk_log;
	uint32_t meso, inmeso, ooff = 0, reqsz;
	int ret;
	int (*action)(scubed3_t*, uint32_t, uint32_t, uint32_t, char*) =
		(cmd == SCUBED3_WRITE)?do_write:do_read;

	//VERBOSE("do_req: %s offset=%ld size=%ld on \"%s\"",
	//		(cmd == SCUBED3_WRITE)?"write":"read",
	//		r_offset, size, l->dev->name);

	if (r_offset >= end) {
		if (cmd == SCUBED3_WRITE && size) {
			DEBUG("write past end of device \"%s\"", l->dev->name);
			return -ENOSPC;
		}
		return 0;
	}

	if (size > end - r_offset) size = end - r_offset;

	/* a request is at most a few hundred kilobytes, the return value
	 * must fit in an int */
	assert(size <= INT32_MAX);

	meso = r_offset>>l->dev->b->mesoblk_log;
	inmeso = r_offset%(1<<l->dev->b->mesoblk_log);

	if (inmeso) {
		if (inmeso + size <= 1<<l->dev->b->mesoblk_log) reqsz = size;
		else reqsz = (1<<l->dev->b->mesoblk_log) - inmeso;

		if ((ret = action(l, meso, inmeso, reqsz, buf + ooff)))
			return ret;
		meso++;
		size -= reqsz;
		ooff += reqsz;
	}

	while (size >= 1<<l->dev->b->mesoblk_log) {
		if ((ret = action(l, meso, 0, 1<<l->dev->b->mesoblk_log,
						buf + ooff))) return ret;
		meso++;
		size -= 1<<l->dev->b->mesoblk_log;
		ooff += 1<<l->dev->b->mesoblk_log;
	}

	if (size > 0 && (ret = action(l, meso, 0, size, buf + ooff)))
		return ret;

	return ooff + size;
}

#define SCUBED3_OPT_KEY(a,b,c) { a, offsetof(struct options, b), c }

int main(int argc, char **argv) {
	struct options {
		char *base;
		char *control;
		/* fuse_opt_parse stores %d in an int */
		int mesoblock_log;
		int macroblock_log;
	} options = {
		.base = NULL,
		.control = NULL,
		.mesoblock_log = 14,
		.macroblock_log = 22
	};
	struct fuse_opt scubed3_opts[] = {
		SCUBED3_OPT_KEY("-b %s", base, 0),
		SCUBED3_OPT_KEY("-a %s", control, 0),
		SCUBED3_OPT_KEY("-m %d", mesoblock_log, 0),
		SCUBED3_OPT_KEY("-M %d", macroblock_log, 0),
		FUSE_OPT_END
	};
	int ret;
	struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
	blockio_t b = { };

	verbose_init(argv[0]);

	VERBOSE("version %s Copyright (C) 2019, Rik Snel <rik@snel.it>",
			PACKAGE_VERSION);

	if (fuse_opt_parse(&args, &options, scubed3_opts, NULL) == -1)
		FATAL("error parsing options");

	if (!options.base) FATAL("argument -b FILE is required");

	/* a mesoblock has 4 parts (see BLOCKIO_PARTS) and is in RAM (on the
	 * stack) a few times, there are at most 65535 data mesoblocks in a
	 * macroblock (uint16_t mmpm) */
	if (options.mesoblock_log < 12 || options.mesoblock_log > 20)
		FATAL("-m %d: the mesoblock size must be between 2^12 and 2^20 "
				"bytes", options.mesoblock_log);
	if (options.macroblock_log <= options.mesoblock_log ||
			options.macroblock_log > 30 ||
			options.macroblock_log - options.mesoblock_log > 16)
		FATAL("-M %d: the macroblock size must be between 2 and 2^16 "
				"mesoblocks, and at most 2^30 bytes",
				options.macroblock_log);

	/* lock me into memory; don't leak info to swap */
	if (mlockall(MCL_CURRENT|MCL_FUTURE)<0)
		WARNING("failed locking process in RAM: %s",
				strerror(errno));

	gcry_global_init();

	blockio_init_file(&b, options.base,
			options.macroblock_log, options.mesoblock_log);

	ret = fuse_io_start(args.argc, args.argv, &b,
			options.control?options.control:CONTROL_SOCKET);

	blockio_free(&b);

	free(options.base);
	free(options.control);
	fuse_opt_free_args(&args);

	exit(ret);
}

