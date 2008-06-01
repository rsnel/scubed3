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
 * of 16 bytes, amount of cipherblocks in wide blocks must be fixed */

typedef struct cipher_spec_s {
	void *(*init)(gcry_cipher_hd_t, size_t);
	void (*enc)(void*, uint8_t*, const uint8_t*, const uint8_t*);
	void (*dec)(void*, uint8_t*, const uint8_t*, const uint8_t*);
	void (*free)(void*);
	const char *name;
} cipher_spec_t;

extern const cipher_spec_t cipher_abl4;

typedef struct cipher_s {
	const cipher_spec_t *spec;
	void *ctx;
	gcry_cipher_hd_t hd;
} cipher_t;

void cipher_init(cipher_t*, const char*, size_t, const uint8_t*, size_t);

void cipher_enc(cipher_t*, uint8_t*, const uint8_t*, const uint8_t*);

void cipher_dec(cipher_t*, uint8_t*, const uint8_t*, const uint8_t*);

void cipher_free(cipher_t*);

#endif /* INCLUDE_SCUBED3_CIPHER_H */
