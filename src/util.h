/* util.h - some utility functions
 *
 * Copyright (C) 2009  Rik Snel <rik@snel.it>
 *
 * Parts Copyright (C) 1999-2006 Monty <monty@xiph.org>
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
#ifndef INCLUDE_SCUBED3_UTIL_H
#define INCLUDE_SCUBED3_UTIL_H 1

#include <stdlib.h>
#include <stdint.h>

void *ecalloc(size_t, size_t);

char *estrdup(const char*);

uint32_t deterministic(uint32_t);

int unbase16(char *buf, size_t len);

/* the stuff below is stolen from /usr/include/utils.h
 * (in Debian) which is not a standard include, but part
 * of libcdparanoia (which is under GPL v2 or later and is
 * Copyright (C) 1999-2006 Monty <monty@xiph.org>) */
static inline int32_t swap32(int32_t x){
  return((((u_int32_t)x & 0x000000ffU) << 24) |
	 (((u_int32_t)x & 0x0000ff00U) <<  8) |
	 (((u_int32_t)x & 0x00ff0000U) >>  8) |
	 (((u_int32_t)x & 0xff000000U) >> 24));
}

#if BYTE_ORDER == LITTLE_ENDIAN

static inline int32_t be32_to_cpu(int32_t x) {
	return swap32(x);
}

static inline int32_t le32_to_cpu(int32_t x) {
	return x;
}

#else

static inline int32_t be32_to_cpu(int32_t x) {
	return x;
}

static inline int32_t le32_to_cpu(int32_t x) {
	return swap32(x);
}


#endif

static inline int32_t cpu_to_be32(int32_t x) {
	return be32_to_cpu(x);
}

static inline int32_t cpu_to_le32(int32_t x) {
	return le32_to_cpu(x);
}

#endif /* INCLUDE_SCUBED3_UTIL_H */
