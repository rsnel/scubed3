/* juggler.h - block provider
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
#ifndef INCLUDE_SCUBED3_JUGGLER_H
#define INCLUDE_SCUBED3_JUGGLER_H 1

#include <stdint.h>
#include "blockio.h"
#include "random.h"
#include "dllarr.h"

/* the juggler decides which block of a partition is written next: every
 * write goes to a block that is drawn uniformly at random from all blocks
 * that currently belong to the partition (the pool), so the order of the
 * writes is i.i.d. uniform over the pool, also when the pool changes;
 * the block that will be written next is drawn one write in advance, so
 * that its live data can be copied to the block that is written now;
 *
 * the juggler remembers from which blocks the current block and the next
 * block were drawn (their draw sets): the draws are numbered, a block has
 * the number of the draws when it was added and when it was removed, it is
 * in the draw set of draw m if added < m <= removed; the scheduler of
 * paranoia level 2 needs the draw set of the current block (the write of
 * the partition lands uniformly in it), a block that is removed stays in it
 * until the next block is drawn without it (see blockio_dev_release_removed)
 * and blocks that are added are put in the draw set of the next block;
 *
 * in paranoia level 0 (greedy is set) the next block is not drawn, it is the
 * block with the least live data (not the block that is written now, that
 * block is empty when the next block is chosen) */
typedef struct juggler_s {
	blockio_info_t **pool;
	uint32_t no_pool, size_pool;
	blockio_info_t *next; /* the block that will be written next */
	uint64_t seqno; /* seqno of the most recent write */
	uint64_t draws; /* the number of draws, the last draw is next */
	uint64_t cur_draw; /* the draw of the current block */
	uint32_t next_pool, cur_pool; /* sizes of their draw sets */
	int greedy; /* paranoia level 0: the next block is the emptiest */
	random_t *r;
} juggler_t;

uint32_t juggler_count(juggler_t*);

void juggler_init(juggler_t*, random_t*);

void juggler_add_macroblock(juggler_t*, blockio_info_t*);

void juggler_remove_macroblock(juggler_t*, blockio_info_t*);

void juggler_take_back(juggler_t*, blockio_info_t*);

blockio_info_t *juggler_nth(juggler_t*, uint32_t);

void juggler_set_next(juggler_t*, blockio_info_t*);

void juggler_drawn_next(juggler_t*, blockio_info_t*);

int juggler_in_current(juggler_t*, blockio_info_t*);

blockio_info_t *juggler_get_devblock(juggler_t*);

void juggler_verbose(juggler_t*, uint32_t (*getnum)(blockio_info_t*, void*),
		void*);

void juggler_free_and_empty_into(juggler_t*,
		void *(*append)(dllarr_t*, void*), dllarr_t*);

void juggler_free(juggler_t*);

#endif /* INCLUDE_SCUBED3_JUGGLER_H */
