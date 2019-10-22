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
#include "juggler.h"

void juggler_init_fresh(juggler_t *j, random_t *r, uint32_t no_devblocks) {
	assert(r && j && no_devblocks > 0);

	j->unscheduled = j->scheduled = NULL;
	j->no_scheduled = 0;
	j->no_unscheduled = j->no_devblocks = no_devblocks;
	j->r = r;

	for (uint32_t i = 0; i < no_devblocks; i++) {
		juggler_block_t *b = ecalloc(1, sizeof(*b));
		b->next = j->unscheduled;
		j->unscheduled = b;
		b->index = i;
	}
}

static void decrease_lifespan(juggler_block_t *list) {
	uint32_t seen = 1;
	while (list) {
		assert(list->lifespan > seen);
		seen = list->lifespan--;
		list = list->next;
	}
}

uint32_t juggler_get_devblock(juggler_t *j, uint32_t *lifespan) {
	uint32_t time = 1, available_blocks;
	juggler_block_t *next, **iterate;
	if (j->scheduled && j->scheduled->lifespan == 1) {
		VERBOSE("already scheduled block must be output");
		next = j->scheduled;
		next->lifespan = 0;
		j->scheduled = next->next;
		j->no_scheduled--;
	} else {
		VERBOSE("no scheduled block to output, select from unscheduled blocks");
		assert(j->no_unscheduled != 0);
		uint32_t index = random_custom(j->r, j->no_unscheduled - 1);
		VERBOSE("requested index is %u", index);
		iterate = &j->unscheduled;
		while (index--) iterate = &((*iterate)->next);
		assert(index == UINT32_MAX);
		next = *iterate;
		*iterate =  next->next;
		j->no_unscheduled--;
	}

	decrease_lifespan(j->scheduled);

	/* decide when we will see the chosen block again */
	/* it is now at time 0 */

	available_blocks = j->no_unscheduled + 1; // unscheduled + selected block
	iterate = &j->scheduled;
	do {
		if ((*iterate) && time == (*iterate)->lifespan) {
			/* our block will certainly not appear here
			 * because another block is scheduled to appear */
			iterate = &((*iterate)->next);
			available_blocks++;
		} else {
			if (!random_custom(j->r, available_blocks - 1)) {
				VERBOSE("(re)schedule block %u at %u", next->index, time);
				*lifespan = next->lifespan = time;
				next->next = *iterate;
				j->no_scheduled++;
				*iterate = next;
				break;
			}
		}
		time++;
	} while (1);

	return next->index;
}

static void show_list(const char *name, juggler_block_t *list) {
	juggler_block_t *b = list;
	int i = 0;
	VERBOSE("%s", name);
	while (b) {
		VERBOSE("%d [%u %u]", i++, b->index, b->lifespan);
		b = b->next;
	}
}

void juggler_verbose(juggler_t *j) {
	VERBOSE("--juggler--");
	VERBOSE("no_devblocks=%u, no_scheduled=%u, no_unscheduled=%u", j->no_devblocks, j->no_scheduled, j->no_unscheduled);
	show_list("scheduled", j->scheduled);
	show_list("unscheduled", j->unscheduled);
}

void juggler_free(juggler_t *j) {
	juggler_block_t *cur;

	cur = j->scheduled;
	while (cur) {
		juggler_block_t *next = cur->next;
		free(cur);
		cur = next;
	}

	cur = j->unscheduled;
	while (cur) {
		juggler_block_t *next = cur->next;
		free(cur);
		cur = next;
	}

}

