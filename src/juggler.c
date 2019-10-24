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
#include "macroblock.h"
#include "juggler.h"

void juggler_init(juggler_t *j, random_t *r) {
	assert(j && r);

	j->unscheduled = j->scheduled = NULL;
	j->no_scheduled = j->no_unscheduled = 0;
	j->r = r;
	j->seqno = 0;
}

void juggler_add_macroblock(juggler_t *j, macroblock_t *b) {
	assert(j && b);
	assert(!b->next);
	if (b->seqno == b->next_seqno) { // empty block
		assert(!b->next_seqno);
		j->no_unscheduled++;
		b->next = j->unscheduled;
		j->unscheduled = b;
	} else if (b->seqno < b->next_seqno) { // block that is in use
		macroblock_t **iterate = &j->scheduled;
		assert(b->next_seqno > b->seqno);
		while (*iterate && (*iterate)->next_seqno < b->next_seqno)
			iterate = &((*iterate)->next);
		assert(!(*iterate) || (*iterate)->next_seqno > b->next_seqno);
		b->next = *iterate;
		*iterate = b;
		j->no_scheduled++;
		if (b->seqno > j->seqno) j->seqno = b->seqno;
	} else assert(0); // nonsensical block 
}
		
macroblock_t *juggler_get_obsoleted(juggler_t *j) {
	assert(j);
	macroblock_t *ret = j->scheduled;

	if (ret && ret->next_seqno == j->seqno + 1) return ret;
	else return NULL;
}
	
macroblock_t *juggler_get_devblock(juggler_t *j) {
	macroblock_t *next, **iterate;

	if ((next = juggler_get_obsoleted(j))) {
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
	}

	j->seqno++;

	/* decide when we will see the chosen block again */
	/* it is now at time 0, so the first time at which
	 * this block can reappear is time 1 */

	uint32_t available_blocks = j->no_unscheduled + 1; // unscheduled blocks + selected block
	iterate = &j->scheduled;
	next->seqno = next->next_seqno = j->seqno;

	do {
		next->next_seqno++;
		if ((*iterate) && next->next_seqno == (*iterate)->next_seqno) {
			/* our block will certainly not appear here
			 * because another block is scheduled to appear */
			iterate = &((*iterate)->next);
			available_blocks++;
		} else {
			if (!random_custom(j->r, available_blocks)) {
				//VERBOSE("(re)schedule block %u at %lu", next->index, time);
				next->next = *iterate;
				*iterate = next;
				j->no_scheduled++;
				break;
			}
		}
	} while (1);

	return next;
}

static void show_list(const char *name, macroblock_t *list, macroblock_t *disk) {
	macroblock_t *b = list;
	int i = 0;
	VERBOSE("%s", name);
	while (b) {
		VERBOSE("%d [%lu %5lu]", i++, b - disk, b->next_seqno);
		b = b->next;
	}
}

void juggler_verbose(juggler_t *j, macroblock_t *disk) {
	VERBOSE("--juggler--");
	VERBOSE("no_scheduled=%u, no_unscheduled=%u, seqno=%lu",
			j->no_scheduled, j->no_unscheduled, j->seqno);
	show_list("scheduled", j->scheduled, disk);
	show_list("unscheduled", j->unscheduled, disk);
}

static void empty_list(macroblock_t *head) {
	macroblock_t *next;

	while (head) {
		next = head->next;
		head->next = NULL;
		head = next;
	}
}

void juggler_free(juggler_t *j) {
	empty_list(j->scheduled);
	empty_list(j->unscheduled);
}

