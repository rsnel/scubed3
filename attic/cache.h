/* cache.h - cache and delayed writer
 *
 * Copyright (C) 2009  Rik Snel <rik@snel.it>
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
#ifndef INCLUDE_SCUBED3_CACHE_H
#define INCLUDE_SCUBED3_CACHE_H 1

#include "blockio.h"

typedef struct cache_thread_priv_s {
	blockio_t *b;
} cache_thread_priv_t;

void *cache_thread(void *arg);

#endif /* INCLUDE_SCUBED3_CACHE_H */
