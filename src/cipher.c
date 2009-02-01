/* cipher.c - defines functions to use blockcipher modes
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
#include <assert.h>
#include "cipher.h"
#include "verbose.h"
#include "binio.h"

const cipher_spec_t *cipher_specs[] = {
	&cipher_abl4,
	&cipher_null,
	&cipher_cbc_large
};

#define NO_CIPHERS (sizeof(cipher_specs)/sizeof(cipher_specs[0]))

void cipher_init(cipher_t *w, const char *name, size_t size,
		const uint8_t *key, size_t key_len) {
	int i = 0, algo;
	size_t block_size, bkey_len;
	const cipher_spec_t *mode_spec = NULL;
	assert(w && name && size > 0);

	/* parse name of the form MODE(PRIMITIVE) */
	char mode[strlen(name) + 1], *prim;
	strcpy(mode, name);
	prim = strchr(mode, ')');
	if (!prim) FATAL("no closing paren found in cipher name");
	if (*(prim + 1) != '\0')
		FATAL("data after closing paren in cipher name");
	*prim = '\0';
	prim = strchr(mode, '(');
	if (!prim) FATAL("no closing paren found in cipher name");
	*prim = '\0';
	prim++;

	while (!mode_spec) {
		if (i >= NO_CIPHERS)
			FATAL("ciphermode %s not supported", mode);

		if (!strcmp(mode, cipher_specs[i]->name))
			mode_spec = cipher_specs[i];

		i++;
	}

	algo = gcry_cipher_map_name(prim);
	if (!algo) FATAL("blockcipher %s not supported", prim);

	gcry_call(cipher_algo_info, algo,
			GCRYCTL_GET_BLKLEN, NULL, &block_size);
	if (block_size != 16) FATAL("cipher has wrong block size");

	gcry_call(cipher_algo_info, algo,
			GCRYCTL_GET_KEYLEN, NULL, &bkey_len);
	if (key_len != bkey_len) FATAL("supplied key has wrong lenght");

	VERBOSE("opening %s(%s), with %d cipherblocks per mesoblock",
			mode, prim, size);

	gcry_call(cipher_open, &w->hd, algo, GCRY_CIPHER_MODE_ECB, 0);
	gcry_call(cipher_setkey, w->hd, key, key_len);

	w->spec = mode_spec;
	w->ctx = w->spec->init(w->hd, size);
}

static void set_iv(unsigned char *iv, uint64_t iv0, uint32_t iv1) {
	binio_write_uint64_be(iv, iv0);
	binio_write_uint32_be(iv + 8, iv1);
	memset(iv + 12, 0, 4);
}

void cipher_enc_iv(cipher_t *w, uint8_t *out,
		const uint8_t *in, const uint8_t *iv) {
	assert(w && w->spec && w->spec->enc && w->ctx);
	w->spec->enc(w->ctx, out, in, iv);
}

void cipher_enc(cipher_t *w, uint8_t *out,
		const uint8_t *in, uint64_t iv0, uint32_t iv1) {
	unsigned char iv[16];

	assert(w && w->spec && w->spec->enc && w->ctx);
	set_iv(iv, iv0, iv1);
	w->spec->enc(w->ctx, out, in, iv);
}

void cipher_dec_iv(cipher_t *w, uint8_t *out,
		const uint8_t *in, const uint8_t *iv) {
	assert(w && w->spec && w->spec->dec && w->ctx);
	w->spec->dec(w->ctx, out, in, iv);
}

void cipher_dec(cipher_t *w, uint8_t *out,
		const uint8_t *in, uint64_t iv0, uint32_t iv1) {
	unsigned char iv[16];

	assert(w && w->spec && w->spec->dec && w->ctx);
	set_iv(iv, iv0, iv1);
	w->spec->dec(w->ctx, out, in, iv);
}

void cipher_free(cipher_t *w) {
	assert(w && w->spec && w->spec->free && w->ctx);
	w->spec->free(w->ctx);
	gcry_cipher_close(w->hd);
}
