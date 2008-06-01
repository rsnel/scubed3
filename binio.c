/* binio.c - binary I/O functions
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
#include <inttypes.h>

void binio_write_uint8(void *b, uint8_t s) {
	unsigned char *buf = b;
        buf[0] = s;
}

void binio_write_uint16_be(void *b, uint16_t s) {
	unsigned char *buf = b;
        buf[0] = (s>>8)&0xFF;
        buf[1] = s&0xFF;
}

void binio_write_uint32_be(void *b, uint32_t s) {
	unsigned char *buf = b;
        buf[0] = (s>>24)&0xFF;
        buf[1] = (s>>16)&0xFF;
        buf[2] = (s>>8)&0xFF;
        buf[3] = s&0xFF;
}

void binio_write_uint64_be(void *b, uint64_t s) {
	unsigned char *buf = b;
        buf[0] = (s>>56)&0xFF;
        buf[1] = (s>>48)&0xFF;
        buf[2] = (s>>40)&0xFF;
        buf[3] = (s>>32)&0xFF;
        buf[4] = (s>>24)&0xFF;
        buf[5] = (s>>16)&0xFF;
        buf[6] = (s>>8)&0xFF;
        buf[7] = s&0xFF;
}

void binio_write_uint16_le(void *b, uint16_t s) {
	unsigned char *buf = b;
        buf[1] = (s>>8)&0xFF;
        buf[0] = s&0xFF;
}

void binio_write_uint32_le(void *b, uint32_t s) {
	unsigned char *buf = b;
        buf[3] = (s>>24)&0xFF;
        buf[2] = (s>>16)&0xFF;
        buf[1] = (s>>8)&0xFF;
        buf[0] = s&0xFF;
}

void binio_write_uint64_le(void *b, uint64_t s) {
	unsigned char *buf = b;
        buf[7] = (s>>56)&0xFF;
        buf[6] = (s>>48)&0xFF;
        buf[5] = (s>>40)&0xFF;
        buf[4] = (s>>32)&0xFF;
        buf[3] = (s>>24)&0xFF;
        buf[2] = (s>>16)&0xFF;
        buf[1] = (s>>8)&0xFF;
        buf[0] = s&0xFF;
}

uint8_t binio_read_uint8(const void *b) {
	const unsigned char *buf = b;
        return buf[0];
}

uint16_t binio_read_uint16_be(const void *b) {
	const unsigned char *buf = b;
        return (buf[0]<<8)|buf[1];
}

uint32_t binio_read_uint32_be(const void *b) {
	const unsigned char *buf = b;
        return (buf[0]<<24)|(buf[1]<<16)|(buf[2]<<8)|buf[3];
}

uint64_t binio_read_uint64_be(const void *b) {
	const unsigned char *buf = b;
        return (((uint64_t)buf[0])<<56)|
                (((uint64_t)buf[1]<<48))|
                (((uint64_t)buf[2]<<40))|
                (((uint64_t)buf[3]<<32))|
                (((uint64_t)buf[4]<<24))|
                (((uint64_t)buf[5]<<16))|
                (buf[6]<<8)|buf[7];
}

uint16_t binio_read_uint16_le(const void *b) {
	const unsigned char *buf = b;
        return (buf[1]<<8)|buf[0];
}

uint32_t binio_read_uint32_le(const void *b) {
	const unsigned char *buf = b;
        return (buf[3]<<24)|(buf[2]<<16)|(buf[1]<<8)|buf[0];
}

uint64_t binio_read_uint64_le(const void *b) {
	const unsigned char *buf = b;
        return (((uint64_t)buf[7])<<56)|
                (((uint64_t)buf[6]<<48))|
                (((uint64_t)buf[5]<<40))|
                (((uint64_t)buf[4]<<32))|
                (((uint64_t)buf[3]<<24))|
                (((uint64_t)buf[2]<<16))|
                (buf[1]<<8)|buf[0];
}
