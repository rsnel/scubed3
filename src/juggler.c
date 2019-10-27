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
#include <limits.h>
#include <errno.h>
#include <string.h>
#include "verbose.h"
#include "util.h"
#include "blockio.h"
#include "binio.h"
#include "juggler.h"

void juggler_init(juggler_t *j, random_t *r) {
	assert(j && r);

	j->unscheduled = j->scheduled = NULL;
	j->no_scheduled = j->no_unscheduled = 0;
	j->r = r;
	j->seqno = 0;
}

void juggler_notify_seqno(juggler_t *j, uint64_t seqno) {
	if (seqno > j->seqno) j->seqno = seqno;
}

void juggler_add_macroblock(juggler_t *j, blockio_info_t *b) {
	assert(j && b);
	assert(!b->next);
	if (b->seqno == b->next_seqno) { // empty block
		assert(!b->next_seqno);
		j->no_unscheduled++;
		b->next = j->unscheduled;
		j->unscheduled = b;
	} else if (b->seqno < b->next_seqno) { // block that is in use
		blockio_info_t **iterate = &j->scheduled;
		assert(b->next_seqno > b->seqno);
		while (*iterate && (*iterate)->next_seqno < b->next_seqno)
			iterate = &((*iterate)->next);
		assert(!(*iterate) || (*iterate)->next_seqno > b->next_seqno);
		b->next = *iterate;
		*iterate = b;
		j->no_scheduled++;
		juggler_notify_seqno(j, b->seqno);
	} else assert(0); // nonsensical block 
}

blockio_info_t *juggler_get_obsoleted(juggler_t *j) {
	assert(j);
	blockio_info_t *ret = j->scheduled;

	if (ret && ret->next_seqno == j->seqno + 1) return ret;
	else return NULL;
}
	
int juggler_discard_possible(juggler_t *j, blockio_info_t *next) {
	/* if a block is scheduled to be written, either another block
	 * must be scheduled to be written after that or an unscheduled
	 * block must be available to fill the hole */
	if (next && (j->no_unscheduled == 0 && (!next->next ||
			next->next->next_seqno != next->next_seqno + 1)))
		return 0;

	/* of no block is scheduled to be written, then we must discard
	 * an unscheduled block and there must be another unscheduled block
	 * available to output the next time */
	if (!next && j->no_unscheduled < 2) return 0;

	return 1;
}

char *juggler_hash_scheduled_seqnos(juggler_t *j, char *hash_res) {
	gcry_md_hd_t hd;
	char buf[sizeof(uint64_t)];

	blockio_info_t *bi = j->scheduled;

	gcry_call(md_open, &hd, GCRY_MD_SHA256, 0);

	if (bi) do {
		binio_write_uint64_be(buf, bi->seqno);
		gcry_md_write(hd, buf, sizeof(uint64_t));
	} while ((bi = bi->next));

	memcpy(hash_res, gcry_md_read(hd, 0), 32);
	gcry_md_close(hd);

	return hash_res;
}

blockio_info_t *juggler_get_devblock(juggler_t *j, int discard) {
	blockio_info_t *next = juggler_get_obsoleted(j), **iterate;

	/* if the user wants to discard the next block, let's
	 * see if that is possible, return NULL if not */
	if (discard && !juggler_discard_possible(j, next)) return NULL;

	if (next) {
		//VERBOSE("already scheduled block must be output");
		j->scheduled = next->next;
		j->no_scheduled--;
	} else {
		//VERBOSE("no scheduled block to output, select from unscheduled blocks");
		assert(j->unscheduled && j->no_unscheduled != 0);
		uint32_t index = random_custom(j->r, j->no_unscheduled);
		//VERBOSE("requested index is %u", index);
		iterate = &j->unscheduled;
		while (index--) iterate = &((*iterate)->next);
		next = *iterate;
		*iterate =  next->next;
		j->no_unscheduled--;

		if (discard) {
			assert(j->no_unscheduled > 0);
			next->seqno = next->next_seqno = 0;
			next->next = NULL;
			/* caller will see seqnos = 0, this means
			 * that the block doens't have to be written
			 * to the disk */
			return next;
		}
	}

	j->seqno++;

	/* decide when we will see the chosen block again */
	/* it is now at time 0, so the first time at which
	 * this block can reappear is time 1 */

	// all the unscheduled blocks are available and the current block
	// is also available to be selected, we are only interested in
	// when our selected block is reselected
	uint32_t available_blocks = j->no_unscheduled + 1; // unscheduled blocks + selected block
	iterate = &j->scheduled;
	next->seqno = next->next_seqno = j->seqno;

	if (discard) {
		assert(j->no_unscheduled > 0 ||
				(j->scheduled &&
				 j->scheduled->next_seqno == next->seqno + 1));
		next->next = NULL;

		/* we don't reschedule this block, since it will be discarded
		 * the block MUST be written to disk (to indicate that it
		 * has been discarded), the fact that this block is discarded
		 * will be visible because seqno and next_seqno are equal */
	} else do {
		next->next_seqno++;
		if ((*iterate) && next->next_seqno == (*iterate)->next_seqno) {
			/* our block will certainly not appear here
			 * because another block is scheduled to appear */
			iterate = &((*iterate)->next);
			available_blocks++;
		} else {
			if (!random_custom(j->r, available_blocks)) {
				next->next = *iterate;
				*iterate = next;
				j->no_scheduled++;
				break;
			}
		}
	} while (1);

	return next;
}

static void show_list(const char *name, blockio_info_t *list,
		uint32_t (*getnum)(blockio_info_t*, void*), void *priv) {
	blockio_info_t *b = list;
	int i = 0;
	VERBOSE("%s", name);
	while (b) {
		VERBOSE("%d [%u %5lu]", i++, getnum(b, priv), b->next_seqno);
		b = b->next;
	}
}

void juggler_verbose(juggler_t *j, uint32_t (*getnum)(blockio_info_t*, void*), void *priv) {
	VERBOSE("--juggler--");
	VERBOSE("no_scheduled=%u, no_unscheduled=%u, seqno=%lu",
			j->no_scheduled, j->no_unscheduled, j->seqno);
	show_list("scheduled", j->scheduled, getnum, priv);
	show_list("unscheduled", j->unscheduled, getnum, priv);
}

static void empty_list(blockio_info_t *head, void *(*append)(dllarr_t*, void*), dllarr_t *list) {
	blockio_info_t *next;

	while (head) {
		next = head->next;
		head->next = NULL;
		if (append) append(list, head);
		head = next;
	}
}

void juggler_free_and_empty_into(juggler_t *j, void *(*append)(dllarr_t*, void*), dllarr_t *list) {
	empty_list(j->scheduled, append, list);
	empty_list(j->unscheduled, append, list);
}

void juggler_free(juggler_t *j) {
	juggler_free_and_empty_into(j, NULL, NULL);
}

