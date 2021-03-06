/* gcry.h - some error checking versions of libgcrypt functions
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
#ifndef INCLUDE_SCUBED3_GCRY_H
#define INCLUDE_SCUBED3_GCRY_H 1

#define GCRY_SEXP_TOKEN_NOT_FOUND	ECCH(GCRY, 0)

#include <gcrypt.h>
#include <stdint.h>

#define GCRY_KEY128_LEN	16
typedef unsigned char gcry_key128_t[GCRY_KEY128_LEN];

#define GCRY_KEY256_LEN	32
typedef unsigned char gcry_key256_t[GCRY_KEY256_LEN];

#define GCRY_MSG_LEN 128
#define gcry_call(a,...) do { \
	int err; \
	if ((err = gcry_##a(__VA_ARGS__))) gcry_fatal(err, #a); \
} while (0)

void gcry_global_init(void);

void gcry_fatal(int, const char*);

#endif /* INCLUDE_SCUBED3_GCRY_H */
