/* juggler.c - block provider
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
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "verbose.h"
#include "util.h"
#include "blockio.h"
#include "juggler.h"

void juggler_init(juggler_t *j, random_t *r) {
	assert(j && r);

	j->pool = NULL;
	j->no_pool = j->size_pool = 0;
	j->next = NULL;
	j->seqno = 0;
	j->draws = j->cur_draw = 0;
	j->next_pool = j->cur_pool = 0;
	j->r = r;
}

uint32_t juggler_count(juggler_t *j) {
	return j->no_pool;
}

/* put block b in the draw set of the next block (see below) */
static void juggler_into_next(juggler_t *j, blockio_info_t *b) {
	if (!random_custom(j->r, ++j->next_pool)) j->next = b;
}

/* if the next block is already drawn, the new block is put in its draw set:
 * it becomes the next block with probability 1/(size of the draw set), so
 * the next block is uniform over the draw set with the new block in it
 * (reservoir sampling); this is safe: the new block is empty, and the old
 * next block keeps its live data (or a copy of it is in the current block,
 * that does not matter) */
void juggler_add_macroblock(juggler_t *j, blockio_info_t *b) {
	assert(j && b);

	if (j->no_pool == j->size_pool) {
		j->size_pool = j->size_pool?2*j->size_pool:16;
		j->pool = erealloc(j->pool, j->size_pool, sizeof(*j->pool));
	}

	b->pool_index = j->no_pool;
	j->pool[j->no_pool++] = b;
	b->removed = UINT64_MAX;

	if (j->next) {
		assert(j->draws);
		b->added = j->draws - 1;
		juggler_into_next(j, b);
	} else b->added = j->draws;
}

/* a removed block comes back (grow), it was not released yet: it is still in
 * the draw set of the current block, and if it is not in the draw set of
 * the next block (it was removed before the next block was drawn), it is
 * put in it, like a block that is added */
void juggler_take_back(juggler_t *j, blockio_info_t *b) {
	int in_next = j->draws <= b->removed;
	assert(b->removed != UINT64_MAX);

	if (j->no_pool == j->size_pool) {
		j->size_pool = j->size_pool?2*j->size_pool:16;
		j->pool = erealloc(j->pool, j->size_pool, sizeof(*j->pool));
	}

	b->pool_index = j->no_pool;
	j->pool[j->no_pool++] = b;
	b->removed = UINT64_MAX;

	if (j->next && !in_next) juggler_into_next(j, b);
}

/* the block that is drawn to be written next cannot be removed, the
 * block stays in the draw sets of the current and the next block */
void juggler_remove_macroblock(juggler_t *j, blockio_info_t *b) {
	assert(j && b && b != j->next);
	assert(b->pool_index < j->no_pool && j->pool[b->pool_index] == b);

	j->pool[b->pool_index] = j->pool[--j->no_pool];
	j->pool[b->pool_index]->pool_index = b->pool_index;
	b->removed = j->draws;
}

/* is the block in the draw set of the current block? */
int juggler_in_current(juggler_t *j, blockio_info_t *b) {
	return b->added < j->cur_draw && j->cur_draw <= b->removed;
}

blockio_info_t *juggler_nth(juggler_t *j, uint32_t n) {
	assert(n < j->no_pool);
	return j->pool[n];
}

/* there is no next block (the last block is removed) */
void juggler_set_next(juggler_t *j, blockio_info_t *b) {
	assert(!b);
	j->next = b;
}

/* used when the partition is opened: the most recent block says which
 * block is written next, it was drawn from all blocks of the partition
 * (and from the blocks that are removed but not released, they are put
 * in its draw set by the caller) */
void juggler_drawn_next(juggler_t *j, blockio_info_t *b) {
	uint32_t i;
	assert(b && j->pool[b->pool_index] == b && !j->next && !j->draws);

	for (i = 0; i < j->no_pool; i++) assert(j->pool[i]->added == 0);
	j->next = b;
	j->draws = 1;
	j->next_pool = j->no_pool;
}

/* returns the block that must be written now (it gets the next seqno)
 * and draws the block that will be written after it; the new block
 * may be drawn again (with probability 1/N), just like in any
 * uniformly random sequence */
blockio_info_t *juggler_get_devblock(juggler_t *j) {
	blockio_info_t *ret;
	assert(j->no_pool);

	if (!(ret = j->next)) {
		ret = j->pool[random_custom(j->r, j->no_pool)];
		j->draws++;
		j->next_pool = j->no_pool;
	}
	j->cur_draw = j->draws;
	j->cur_pool = j->next_pool;

	ret->seqno = ++j->seqno;
	j->next = j->pool[random_custom(j->r, j->no_pool)];
	j->draws++;
	j->next_pool = j->no_pool;

	return ret;
}

void juggler_verbose(juggler_t *j, uint32_t (*getnum)(blockio_info_t*, void*),
		void *priv) {
	uint32_t i;
	VERBOSE("--juggler--");
	VERBOSE("no_pool=%u, seqno=%lu, next=%d", j->no_pool, j->seqno,
			j->next?(int)getnum(j->next, priv):-1);
	for (i = 0; i < j->no_pool; i++)
		VERBOSE("%u [%u %5lu %u]", i, getnum(j->pool[i], priv),
				j->pool[i]->seqno, j->pool[i]->no_nonobsolete);
}

void juggler_free_and_empty_into(juggler_t *j,
		void *(*append)(dllarr_t*, void*), dllarr_t *list) {
	uint32_t i;

	for (i = 0; i < j->no_pool; i++)
		if (append) append(list, j->pool[i]);

	free(j->pool);
	j->pool = NULL;
	j->no_pool = j->size_pool = 0;
	j->next = NULL;
}

void juggler_free(juggler_t *j) {
	juggler_free_and_empty_into(j, NULL, NULL);
}
