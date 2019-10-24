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

void juggler_init(juggler_t *j, random_t *r, macroblock_t *disk) {
	assert(r && j && disk);

	j->disk = disk;
	j->unscheduled = j->scheduled = NULL;
	j->no_scheduled = j->no_unscheduled = 0;
	j->r = r;
}

void juggler_add_macroblock(juggler_t *j, macroblock_t *b) {
	assert(j && b);
	assert(b->state == MACROBLOCK_STATE_EMPTY_E);
	j->no_unscheduled++;
	b->next = j->unscheduled;
	j->unscheduled = b;
}

static void decrease_lifespan(macroblock_t *list) {
	uint64_t seen = 1;
	while (list) {
		assert(list->lifespan2 > seen);
		seen = list->lifespan2--;
		list = list->next;
	}
}

macroblock_t *juggler_get_obsoleted(juggler_t *j) {
	assert(j);
	macroblock_t *ret = j->scheduled;

	if (ret && ret->lifespan2 == 1) return ret;
	else return NULL;
}
	
macroblock_t *juggler_get_devblock(juggler_t *j) {
	uint64_t time = 1;
	uint32_t available_blocks;
	macroblock_t *next, **iterate;
	assert(j->unscheduled || (j->scheduled && j->scheduled->lifespan2));
	if ((next = juggler_get_obsoleted(j))) {
		//VERBOSE("already scheduled block must be output");
		next->lifespan2 = 0;
		j->scheduled = next->next;
		j->no_scheduled--;
	} else {
		//VERBOSE("no scheduled block to output, select from unscheduled blocks");
		assert(j->unscheduled && j->no_unscheduled != 0);
		uint32_t index = random_custom(j->r, j->no_unscheduled - 1);
		//VERBOSE("requested index is %u", index);
		iterate = &j->unscheduled;
		while (index--) iterate = &((*iterate)->next);
		assert(index == UINT32_MAX);
		next = *iterate;
		*iterate =  next->next;
		j->no_unscheduled--;
	}

	decrease_lifespan(j->scheduled);

	/* decide when we will see the chosen block again */
	/* it is now at time 0, so the first time at which
	 * this block can reappear is time 1 */

	available_blocks = j->no_unscheduled + 1; // unscheduled blocks + selected block
	iterate = &j->scheduled;
	do {
		if ((*iterate) && time == (*iterate)->lifespan2) {
			/* our block will certainly not appear here
			 * because another block is scheduled to appear */
			iterate = &((*iterate)->next);
			available_blocks++;
		} else {
			if (!random_custom(j->r, available_blocks - 1)) {
				//VERBOSE("(re)schedule block %u at %lu", next->index, time);
				/* *lifespan = */ next->lifespan2 = time;
				next->next = *iterate;
				j->no_scheduled++;
				*iterate = next;
				break;
			}
		}
		time++;
	} while (1);

	return next;
}

static void show_list(const char *name, macroblock_t *list, macroblock_t *disk) {
	macroblock_t *b = list;
	int i = 0;
	VERBOSE("%s", name);
	while (b) {
		VERBOSE("%d [%lu %lu %lu]", i++, b - disk, b->lifespan2, b->lifespan);
		b = b->next;
	}
}

void juggler_verbose(juggler_t *j) {
	VERBOSE("--juggler--");
	VERBOSE("no_scheduled=%u, no_unscheduled=%u", j->no_scheduled, j->no_unscheduled);
	show_list("scheduled", j->scheduled, j->disk);
	show_list("unscheduled", j->unscheduled, j->disk);
}

void juggler_free(juggler_t *j) {
#if 0
	macroblock_t *cur;

	cur = j->scheduled;
	while (cur) {
		macroblock_t *next = cur->next;
		free(cur);
		cur = next;
	}

	cur = j->unscheduled;
	while (cur) {
		macroblock_t *next = cur->next;
		free(cur);
		cur = next;
	}
#endif

}

