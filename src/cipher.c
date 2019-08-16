/* cipher.c - defines functions to use blockcipher modes
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
#include <assert.h>
#include "cipher.h"
#include "verbose.h"
#include "binio.h"
#include "ecch.h"

const cipher_spec_t *cipher_specs[] = {
	&cipher_null,
	&cipher_cbc_plain,
	&cipher_cbc_essiv,
};

#define NO_CIPHERS (sizeof(cipher_specs)/sizeof(cipher_specs[0]))

void cipher_open_set_and_destroy_key(gcry_cipher_hd_t *hd, const char *name,
		const void *key, size_t key_len) {
	size_t tmp, algo;

	algo = gcry_cipher_map_name(name);
	if (!algo) ecch_throw(ECCH_DEFAULT, "blockcipher %s not supported", name);

	gcry_call(cipher_algo_info, algo,
			GCRYCTL_GET_BLKLEN, NULL, &tmp);
	if (tmp != 16) ecch_throw(ECCH_DEFAULT, "cipher has wrong block size");

	gcry_call(cipher_algo_info, algo,
			GCRYCTL_GET_KEYLEN, NULL, &tmp);
	if (key_len != tmp) ecch_throw(ECCH_DEFAULT,
			"supplied key has wrong length");

	gcry_call(cipher_open, hd, algo,
			GCRY_CIPHER_MODE_ECB, GCRY_CIPHER_SECURE);

	gcry_call(cipher_setkey, *hd, key, key_len);
}

void cipher_init(cipher_t *w, const char *name, size_t size,
		const void *key, size_t key_len) {
	int i = 0;
	const cipher_spec_t *mode_spec = NULL;
	assert(w && name && size > 0);

	/* parse name of the form MODE(PRIMITIVE) */
	char mode[strlen(name) + 1], *prim;
	strcpy(mode, name);
	prim = strchr(mode, ')');
	if (!prim) ecch_throw(ECCH_DEFAULT,
			"no closing paren found in cipher name");
	if (*(prim + 1) != '\0')
		ecch_throw(ECCH_DEFAULT, "data after closing paren "
				"in cipher name");
	*prim = '\0';
	prim = strchr(mode, '(');
	if (!prim) ecch_throw(ECCH_DEFAULT, "no closing paren found "
			"in cipher name");
	*prim = '\0';
	prim++;

	while (!mode_spec) {
		if (i >= NO_CIPHERS)
			ecch_throw(ECCH_DEFAULT, "ciphermode %s "
					"not supported", mode);

		if (!strcmp(mode, cipher_specs[i]->name))
			mode_spec = cipher_specs[i];

		i++;
	}

	w->spec = mode_spec;
	w->ctx = w->spec->init(prim, size, key, key_len);

	wipememory(key, key_len);
}

static void set_iv(unsigned char *iv, uint64_t iv0, uint32_t iv1, uint32_t iv2) {
	binio_write_uint64_be(iv, iv0);
	binio_write_uint32_be(iv + 8, iv1);
	binio_write_uint32_be(iv + 12, iv2);
}

void cipher_enc(cipher_t *w, uint8_t *out,
		const uint8_t *in, uint64_t iv0, uint32_t iv1,
		uint32_t iv2) {
	unsigned char iv[16];

	assert(w && w->spec && w->spec->enc && w->ctx);
	set_iv(iv, iv0, iv1, iv2);
	w->spec->enc(w->ctx, out, in, iv);
}

void cipher_dec(cipher_t *w, uint8_t *out,
		const uint8_t *in, uint64_t iv0, uint32_t iv1,
		uint32_t iv2) {
	unsigned char iv[16];

	assert(w && w->spec && w->spec->dec && w->ctx);
	set_iv(iv, iv0, iv1, iv2);
	w->spec->dec(w->ctx, out, in, iv);
}

void cipher_free(cipher_t *w) {
	assert(w);
	if (w->spec && w->spec->free && w->ctx)
		w->spec->free(w->ctx);
}
