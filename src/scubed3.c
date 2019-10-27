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

#define ID	(index>>l->mesobits)
#define NO	(index&l->mesomask)

void obsolete_mesoblk(scubed3_t *l, blockio_info_t *bi, uint32_t no) {
	assert(bi);
	assert(bi->no_nonobsolete);
	bi->no_nonobsolete--;

	/* if the whole block is obsolete, remove it from the active list */
	if (!bi->no_nonobsolete) {
		WARNING("move from active list is not implemented");
		bi->no_indices = 0;
		//bi->no_indices_gc = 0;
		//bi->no_indices_preempt = 0;
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

static void add_blockref(scubed3_t *l, uint32_t offset) {
	l->dev->bi->indices[l->dev->bi->no_indices] = offset;
	update_block_indices(l, offset, id(l->dev->bi), l->dev->bi->no_indices);
	l->dev->bi->no_indices++;
}

static inline char *mesoblk(scubed3_t *l, uint16_t no) {
	assert(no < l->dev->b->mmpm);
	return l->dev->tmp_macroblock + ((no+1)<<l->dev->b->mesoblk_log);
}

void copy_old_block_to_current(scubed3_t *l) {
	int k;
	uint32_t index;
	if (l->dev->tail_macroblock &&
			blockio_dev_get_macroblock_status(
				l->dev->tail_macroblock) == USED) {
		blockio_info_t *bi = l->dev->tail_macroblock;

		for (k = 0; k < bi->no_indices; k++) {
			if (bi->indices[k] >= l->no_block_indices) continue;

			index = l->block_indices[bi->indices[k]];
			if (index != 0xFFFFFFFF &&
					&l->dev->b->blockio_infos[ID] == bi) {
				blockio_dev_read_mesoblk(l->dev, mesoblk(l,
						l->dev->bi->no_indices), id(bi), k);

				add_blockref(l, bi->indices[k]);

				obsolete_mesoblk(l, bi, k);
			}
		}

	}
}

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

// FIXME the following two functions look almost the same...  
void select_new_macroblock(scubed3_t *l) {
	assert(l->output_initialized);
	pre_emptive_gc(l);
	do {
		blockio_dev_write_current_and_select_next_macroblock(l->dev);
		copy_old_block_to_current(l);
		//l->dev->bi->no_indices_gc = l->dev->bi->no_indices;
		DEBUG("new block %lu (seqno=%lu) has %u mesoblocks due "
				"to GC of block %uFIXME",
				id(l->dev->bi), l->dev->bi->seqno,
				l->dev->bi->no_indices,
				blockio_get_macroblock_index(
					l->dev->tail_macroblock));
	} while (l->dev->bi->no_indices == l->dev->b->mmpm);
}

void initialize_output(scubed3_t *l) {
	if (!l->output_initialized) {
		copy_old_block_to_current(l);
		DEBUG("new block %lu (seqno=%lu) has %u mesoblocks due "
				"to GC of block %u",
				id(l->dev->bi), l->dev->bi->seqno,
				l->dev->bi->no_indices,
				blockio_get_macroblock_index(
					l->dev->tail_macroblock));

		l->output_initialized = 1;
	}
}

void scubed3_cycle(scubed3_t *l) {
	/* output ONE block, run GC if possible and useful */
	if (l->output_initialized) { /* output is initialized */
		pre_emptive_gc(l);
		blockio_dev_write_current_macroblock(l->dev);
		blockio_dev_select_next_macroblock(l->dev);
		l->output_initialized = 0;
	} else {
		copy_old_block_to_current(l);
		l->output_initialized = 1;
		if (!l->cycle_goal) pre_emptive_gc(l);
		blockio_dev_write_current_macroblock(l->dev);
		blockio_dev_select_next_macroblock(l->dev);
		l->output_initialized = 0;
	}
}

void *replay(blockio_info_t *bi, scubed3_t *l) {
	uint32_t k, index;

	VERBOSE("replay at seqno=%ld (%d indices)", bi->seqno, bi->no_indices);
	for (k = 0; k < bi->no_indices; k++) {
		//VERBOSE("k=%u, bi->indices[k]=%u", k, bi->indices[k]);
		if (bi->indices[k] >= l->no_block_indices) continue;
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
	if (l->output_initialized) pre_emptive_gc(l);
	free(l->block_indices);
}

void scubed3_reinit(scubed3_t *l) {
	uint32_t old_no_block_indices;

	old_no_block_indices = l->no_block_indices;

	l->no_block_indices = (l->dev->no_macroblocks -
			l->dev->reserved_macroblocks)*l->dev->b->mmpm;

	if (old_no_block_indices == l->no_block_indices) return;

	if (old_no_block_indices > l->no_block_indices) FATAL("scubed3_reinit shrinking not supported");

#if 0
	/* obsolete all mesoblocks beyond end of device (if shrinking) */
	while (old_no_block_indices > l->no_block_indices)
		obsolete_mesoblk_byidx(l, l->block_indices[--old_no_block_indices]);
#endif
	VERBOSE("scubed3_reinit enlarge");

	l->block_indices = erealloc(l->block_indices, 
			l->no_block_indices, sizeof(uint32_t));

	/* mark all mesoblocks beyond old end of device free */
	while (old_no_block_indices < l->no_block_indices)
		l->block_indices[old_no_block_indices++] = 0xFFFFFFFF;
}

void scubed3_init(scubed3_t *l, blockio_dev_t *dev) {
	int i;
	assert(l && dev);

	l->dev = dev;

	l->mesobits = (dev->b->macroblock_log - dev->b->mesoblk_log);
	l->mesomask = 0xFFFFFFFF>>(32 - l->mesobits);

	if (dev->no_macroblocks <= dev->reserved_macroblocks)
		l->no_block_indices = 0;
	else l->no_block_indices = (dev->no_macroblocks -
			dev->reserved_macroblocks)*dev->b->mmpm;

	VERBOSE("l->no_block_indices=%d", l->no_block_indices);
	l->block_indices = ecalloc(l->no_block_indices, sizeof(uint32_t));

	for (i = 0; i < l->no_block_indices; i++) l->block_indices[i] = 0xFFFFFFFF;

	if (!dev->no_macroblocks) return;

	VERBOSE("%d block(s) to replay", dllarr_count(&dev->replay));

	dllarr_iterate(&dev->replay, (dllarr_iterator_t)replay, l);

	blockio_info_t *bi;

	while ((bi = dllarr_last(&dev->replay))) {
		dllarr_remove(&dev->replay, bi);
		juggler_add_macroblock(&dev->j, bi);
	}
}

void blockio_dev_fake_mesoblk_part(blockio_dev_t *dev, void *addr,
		uint32_t id, uint32_t no, uint32_t offset, uint32_t size) {
	memset(addr, 0, size);
}

void do_cow(scubed3_t *l, uint32_t index, uint32_t muoff,
		uint32_t size, void *addr) {
	void (*readorfake)(blockio_dev_t*, void*, uint32_t, uint32_t,
			uint32_t, uint32_t) = blockio_dev_fake_mesoblk_part;

	if (index != 0xFFFFFFFF) readorfake = blockio_dev_read_mesoblk_part;

	/* read the parts of the  mesoblock we don't modify from disk
	 * otherwise, set it to zero */
	if (muoff) readorfake(l->dev, addr, ID, NO, 0, muoff);
	if (1<<l->dev->b->mesoblk_log > size + muoff)
		readorfake(l->dev, addr + muoff + size, ID, NO, muoff + size,
				(1<<l->dev->b->mesoblk_log) - size - muoff);
}

int do_write(scubed3_t *l, uint32_t mesoff, uint32_t muoff, uint32_t size,
		char *in) {
	uint32_t index = l->block_indices[mesoff];
	assert(muoff + size <= 1<<l->dev->b->mesoblk_log);
	void *addr;
	/* three possibilities:
	 * 1. the block is currently in RAM, we update it
	 * 2. the block was never written, we add it to RAM
	 * 3. the block is on disk, we obsolete it and add it to RAM */

	l->dev->updated = 1;

	initialize_output(l);

	if (ID != id(l->dev->bi)) {
		/* could be that the new block is full after
		 * garbage collecting (depends on the way the
		 * to-be-freed block is selected) */
		if (l->dev->bi->no_indices == l->dev->b->mmpm)
			select_new_macroblock(l);

		index = l->block_indices[mesoff];
	}

	addr = mesoblk(l, (ID == id(l->dev->bi))?NO:l->dev->bi->no_indices);
	memcpy(addr + muoff, in, size);

	/* if not, the mesoblock is in RAM */
	if (ID != id(l->dev->bi)) { /* we are on disk */
		/* if we do not write the complete mesoblock, read the
		 * other parts from disk (if possible) or zero them */
		do_cow(l, index, muoff, size, addr);

		/* mark old mesoblock obsolete, if there is one */
		obsolete_mesoblk_byidx(l, index);

		/* add new reference */
		add_blockref(l, mesoff);
	}

	return 0;
}

int do_read(scubed3_t *l, uint32_t mesoff, uint32_t muoff, uint32_t size,
		char *out) {
	uint32_t index = l->block_indices[mesoff];
	/* three possibilities:
	 * 1. the block is currently in RAM
	 * 2. the block was never written, we return zeroes
	 * 3. the block is on disk */

	if (ID == id(l->dev->bi)) /* in RAM */
		memcpy(out, mesoblk(l, NO) + muoff, size);
	else if (index == 0xFFFFFFFF) /* never written */
		memset(out, 0, size);
	else /* we are on disk */
		blockio_dev_read_mesoblk_part(l->dev, out, ID, NO, muoff, size);
	return 0;
}

int do_req(scubed3_t *l, scubed3_io_t cmd, uint64_t r_offset, size_t size,
		char *buf) {
	assert(cmd == SCUBED3_READ || cmd == SCUBED3_WRITE);
	uint32_t meso = r_offset>>l->dev->b->mesoblk_log;
	uint32_t inmeso = r_offset%(1<<l->dev->b->mesoblk_log);
	uint32_t ooff = 0, reqsz;
	int (*action)(scubed3_t*, uint32_t, uint32_t, uint32_t, char*) =
		(cmd == SCUBED3_WRITE)?do_write:do_read;

//	VERBOSE("do_req: %s offset=%lld size=%d on \"%s\"",
//			(cmd == SCUBED3_WRITE)?"write":"read",
//			r_offset, size, l->dev->name);

	if ((r_offset + size - 1)>>l->dev->b->mesoblk_log >= l->no_block_indices) {
		WARNING("%s access past end of device \"%s\"", 
				(cmd == SCUBED3_WRITE)?"write":"read",
				l->dev->name);
		return 1;
	}

	if (inmeso) {
		if (inmeso + size <= 1<<l->dev->b->mesoblk_log) reqsz = size;
		else reqsz = (1<<l->dev->b->mesoblk_log) - inmeso;

		if (action(l, meso, inmeso, reqsz, buf + ooff)) return 0;
		meso++;
		size -= reqsz;
		ooff += reqsz;
	}

	while (size >= 1<<l->dev->b->mesoblk_log) {
		if (action(l, meso, 0, 1<<l->dev->b->mesoblk_log, buf + ooff))
			return 0;
		meso++;
		size -= 1<<l->dev->b->mesoblk_log;
		ooff += 1<<l->dev->b->mesoblk_log;
	}

	if (size > 0 && action(l, meso, 0, size, buf + ooff)) return 0;

	return 1;
}

#define SCUBED3_OPT_KEY(a,b,c) { a, offsetof(struct options, b), c }

int main(int argc, char **argv) {
	struct options {
		char *base;
		uint8_t mesoblock_log;
		uint8_t macroblock_log;
		uint32_t reserved;
	} options = {
		.base = NULL,
		.mesoblock_log = 14,
		.reserved = 2,
		.macroblock_log = 22
	};
	struct fuse_opt scubed3_opts[] = {
		SCUBED3_OPT_KEY("-b %s", base, 0),
		SCUBED3_OPT_KEY("-r %d", reserved, 0),
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

	/* lock me into memory; don't leak info to swap */
	if (mlockall(MCL_CURRENT|MCL_FUTURE)<0)
		WARNING("failed locking process in RAM: %s",
				strerror(errno));

	gcry_global_init();

	blockio_init_file(&b, options.base,
			options.macroblock_log, options.mesoblock_log);

	ret = fuse_io_start(args.argc, args.argv, &b);

	blockio_free(&b);

	free(options.base);
	fuse_opt_free_args(&args);

	exit(ret);
}

