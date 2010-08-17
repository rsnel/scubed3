/* cipher.h - defines functions to handle blockcipher modes
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
#ifndef INCLUDE_SCUBED3_CIPHER_H
#define INCLUDE_SCUBED3_CIPHER_H 1

/* To avoid that a compiler optimizes certain memset calls away, these
 * macros may be used instead. */
#define wipememory2(_ptr,_set,_len) do { \
	volatile char *_vptr=(volatile char *)(_ptr); \
	size_t _vlen=(_len); \
	while(_vlen) { *_vptr=(_set); _vptr++; _vlen--; } \
} while(0)
#define wipememory(_ptr,_len) wipememory2(_ptr,0,_len)

#include "gcry.h"

/* wide block cipher modes, blockcipher must have blocksize
 * of 16 bytes, amount of cipherblocks in wide blocks must be fixed
 * the iv is assumed to be one cipherblock */

typedef struct cipher_spec_s {
	void *(*init)(const char*, size_t, const void*, size_t);
	void (*enc)(void*, uint8_t*, const uint8_t*, const uint8_t*);
	void (*dec)(void*, uint8_t*, const uint8_t*, const uint8_t*);
	void (*free)(void*);
	const char *name;
} cipher_spec_t;

extern const cipher_spec_t cipher_abl4;
extern const cipher_spec_t cipher_null;
extern const cipher_spec_t cipher_cbc_plain;
extern const cipher_spec_t cipher_cbc_essiv;

typedef struct cipher_s {
	const cipher_spec_t *spec;
	void *ctx;
} cipher_t;

void cipher_init(cipher_t*, const char*, size_t, const void*, size_t);

void cipher_enc(cipher_t*, uint8_t*, const uint8_t*, uint64_t, uint32_t, uint32_t);

void cipher_dec(cipher_t*, uint8_t*, const uint8_t*, uint64_t, uint32_t, uint32_t);

void cipher_free(cipher_t*);

void cipher_open_set_and_destroy_key(gcry_cipher_hd_t*, const char*,
		const void*, size_t);

#endif /* INCLUDE_SCUBED3_CIPHER_H */
