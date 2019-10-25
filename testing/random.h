/* random.h - rng based on /dev/urandom (not cryptographically secure!)
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
#ifndef INCLUDE_SCUBED3_RANDOM_H
#define INCLUDE_SCUBED3_RANDOM_H 1

#include <stdio.h>
#include <stdint.h>

typedef struct random_s {
	FILE *fp;
} random_t;

void random_init(random_t*);

uint32_t random_uint32(random_t*);

uint32_t random_custom(random_t*, uint32_t);

void random_free(random_t*);

#endif /* INCLUDE_SCUBED3_RANDOM_H */
