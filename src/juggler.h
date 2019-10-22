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
#include "random.h"

typedef struct juggler_block_s {
	struct juggler_block_s *next;
	uint32_t lifespan; // 0 means unknown lifespan
	uint32_t index;
} juggler_block_t;

typedef struct juggler_s {
	juggler_block_t *devblocks;   // view of the disk
	juggler_block_t *scheduled, *unscheduled;
	uint32_t no_devblocks, no_scheduled, no_unscheduled;
	random_t *r;
} juggler_t;

void juggler_init_fresh(juggler_t*, random_t *r, uint32_t);

uint32_t juggler_get_devblock(juggler_t*, uint32_t*);

void juggler_verbose(juggler_t*);

void juggler_free(juggler_t*);

#endif /* INCLUDE_SCUBED3_JUGGLER_H */
