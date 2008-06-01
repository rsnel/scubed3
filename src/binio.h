/* binio.h - binary I/O functions
 *
 * Copyright (C) 2006 Rik Snel <rsnel@cube.dyndns.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */
#ifndef INCLUDE_LSBD_BINIO_H
#define INCLUDE_LSBD_BINIO_H 1

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

#endif /* INCLUDE_LSBD_BINIO_H */
