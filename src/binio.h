/* binio.h - binary I/O functions
 *
 * Copyright (C) 2008  Rik Snel <rik@snel.it>
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
#ifndef INCLUDE_SCUBED3_BINIO_H
#define INCLUDE_SCUBED3_BINIO_H 1

#include <inttypes.h>

void binio_write_uint8(void*, uint16_t);

uint8_t binio_read_uint8(const void*);

void binio_write_uint16_le(void*, uint16_t);

uint16_t binio_read_uint16_le(const void*);

void binio_write_uint32_le(void*, uint32_t);

uint32_t binio_read_uint32_le(const void*);

void binio_write_uint64_le(void*, uint64_t);

uint64_t binio_read_uint64_le(const void*);

void binio_write_uint16_be(void*, uint16_t);

uint16_t binio_read_uint16_be(const void*);

void binio_write_uint32_be(void*, uint32_t);

uint32_t binio_read_uint32_be(const void*);

void binio_write_uint64_be(void*, uint64_t);

uint64_t binio_read_uint64_be(const void*);

#endif /* INCLUDE_SCUBED3_BINIO_H */
