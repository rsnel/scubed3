/* bitmap.h - bitmap handler
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
#ifndef INCLUDE_SCUBED3_BITMAP_H
#define INCLUDE_SCUBED3_BITMAP_H 1

#include <stdint.h>

typedef struct {
	uint32_t no_bits;
	uint32_t no_set; /* is redundant but useful */
	uint32_t *bits;
} bitmap_t;

void bitmap_init(bitmap_t*, uint32_t);

void bitmap_setbits(bitmap_t*, uint32_t);

int bitmap_getbit(bitmap_t*, uint32_t);

uint32_t bitmap_getbits(bitmap_t*, uint32_t, uint8_t);

uint32_t bitmap_size(uint32_t);

void bitmap_setbit(bitmap_t*, uint32_t);

void bitmap_clearbit(bitmap_t*, uint32_t);

void bitmap_clearbit_safe(bitmap_t*, uint32_t);

void bitmap_free(bitmap_t*);

uint32_t bitmap_count(bitmap_t*);

void bitmap_read(bitmap_t*, const uint32_t*);

void bitmap_write(uint32_t*, bitmap_t*);

#endif /* INCLUDE_SCUBED3_BIT_H */
