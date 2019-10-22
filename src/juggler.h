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

typedef struct juggler_block_s {
	struct juggler_block_s **devblock;
	uint32_t lifespan; // INT_MAX means: lifespan not yet known
} juggler_block_t;

typedef struct juggler_s {
	juggler_block_t *cache;        // cache of blocks
	juggler_block_t **devblocks;   // view of the disk
	uint32_t devblocks_size, cache_size, cache_len;
	FILE *urandom;
} juggler_t;

void juggler_init_fresh(juggler_t*, uint32_t);

void juggler_free(juggler_t*);

#endif /* INCLUDE_SCUBED3_JUGGLER_H */
