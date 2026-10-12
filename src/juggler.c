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
	j->r = r;
}

uint32_t juggler_count(juggler_t *j) {
	return j->no_pool;
}

void juggler_add_macroblock(juggler_t *j, blockio_info_t *b) {
	assert(j && b);

	if (j->no_pool == j->size_pool) {
		j->size_pool = j->size_pool?2*j->size_pool:16;
		j->pool = erealloc(j->pool, j->size_pool, sizeof(*j->pool));
	}

	b->pool_index = j->no_pool;
	j->pool[j->no_pool++] = b;
}

/* the block that is drawn to be written next cannot be removed */
void juggler_remove_macroblock(juggler_t *j, blockio_info_t *b) {
	assert(j && b && b != j->next);
	assert(b->pool_index < j->no_pool && j->pool[b->pool_index] == b);

	j->pool[b->pool_index] = j->pool[--j->no_pool];
	j->pool[b->pool_index]->pool_index = b->pool_index;
}

blockio_info_t *juggler_nth(juggler_t *j, uint32_t n) {
	assert(n < j->no_pool);
	return j->pool[n];
}

/* used when the partition is opened: the most recent block says which
 * block is written next */
void juggler_set_next(juggler_t *j, blockio_info_t *b) {
	assert(!b || j->pool[b->pool_index] == b);
	j->next = b;
}

/* returns the block that must be written now (it gets the next seqno)
 * and draws the block that will be written after it; the new block
 * may be drawn again (with probability 1/N), just like in any
 * uniformly random sequence */
blockio_info_t *juggler_get_devblock(juggler_t *j) {
	blockio_info_t *ret;
	assert(j->no_pool);

	if (!(ret = j->next))
		ret = j->pool[random_custom(j->r, j->no_pool)];

	ret->seqno = ++j->seqno;
	j->next = j->pool[random_custom(j->r, j->no_pool)];

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
