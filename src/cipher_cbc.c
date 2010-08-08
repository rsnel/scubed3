/* cipher_cbc.c - Cipher Block Chaining mode (chain up to mesoblock size)
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
#include <string.h>
#include "assert.h"
#include "verbose.h"
#include "util.h"
#include "gcry.h"
#include "binio.h"
#include "cipher.h"

typedef unsigned char block_t[16];

typedef struct cbc_plain_s {
	gcry_cipher_hd_t hd;
	size_t no_blocks;
} cbc_plain_t;

typedef struct cbc_essiv_s {
	cbc_plain_t *plain;
	gcry_cipher_hd_t hd;
} cbc_essiv_t;

static void xor_block(block_t out, const block_t in1, const block_t in2) {
	//int i;
	//for (i = 0; i < 16; i++) out[i] = in1[i] ^ in2[i];
	*((uint64_t*)out) = *((uint64_t*)in1) ^ *((uint64_t*)in2);
	*((uint64_t*)out + 1) = *((uint64_t*)in1 + 1) ^ *((uint64_t*)in2 + 1);
}

static void *init_plain(const char *name, size_t no_blocks,
		const void *key, size_t key_len) {
	cbc_plain_t *priv;
	assert(no_blocks > 0);
	priv = ecalloc(1, sizeof(cbc_plain_t));

	priv->no_blocks = no_blocks;
	cipher_open_set_and_destroy_key(&priv->hd, name, key, key_len);

	return priv;
}

#define ESSIV_HASH GCRY_MD_SHA256

static void *init_essiv(const char *name, size_t no_blocks,
		const void *key, size_t key_len) {
	gcry_md_hd_t hd;
	cbc_essiv_t *priv;

	priv = ecalloc(1, sizeof(cbc_essiv_t));

	gcry_call(md_open, &hd, ESSIV_HASH, GCRY_MD_FLAG_SECURE);
	assert(gcry_md_is_secure(hd));

	gcry_md_write(hd, key, key_len);

	cipher_open_set_and_destroy_key(&priv->hd, name,
			gcry_md_read(hd, ESSIV_HASH),
			gcry_md_get_algo_dlen(ESSIV_HASH));

	gcry_md_close(hd);
	
	priv->plain = init_plain(name, no_blocks, key, key_len);

	return priv;
}

static void enc_plain(void *priv, uint8_t *out,
		const uint8_t *in, const uint8_t *iv) {
	cbc_plain_t *ctx = priv;
	int i = ctx->no_blocks;

	xor_block(out, in, iv);
	do {
		gcry_call(cipher_encrypt, ctx->hd, out, 16, NULL, 0);
		if (!(--i)) return;

		out += 16;
		in += 16;
		xor_block(out, in, out - 16);
	} while (1);
}

static void enc_essiv(void *priv, uint8_t *out,
		const uint8_t *in, const uint8_t *iv) {
	block_t newiv;
	cbc_essiv_t *ctx = priv;

	gcry_call(cipher_encrypt, ctx->hd, newiv, 16, iv, 16);

	enc_plain(ctx->plain, out, in, newiv);
}

static void dec_plain(void *priv, uint8_t *out,
		const uint8_t *in, const uint8_t *iv) {
	cbc_plain_t *ctx = priv;
	block_t tmp, tmp2;
	const uint8_t *gcry_in = in;
	size_t gcry_inlen = 16;
	int i = ctx->no_blocks;

	if (out == in) {
		gcry_in = NULL;
		gcry_inlen = 0;
	}

	memcpy(tmp, iv, 16);

	while (i--) {
		memcpy(tmp2, in, 16);
		gcry_call(cipher_decrypt, ctx->hd, out, 16,
				gcry_in, gcry_inlen);
		xor_block(out, out, tmp);
		memcpy(tmp, tmp2, 16);
		out += 16;
		in += 16;
	}

	wipememory(tmp, 16);
	wipememory(tmp2, 16);
}

static void dec_essiv(void *priv, uint8_t *out,
		const uint8_t *in, const uint8_t *iv) {
	block_t newiv;
	cbc_essiv_t *ctx = priv;

	gcry_call(cipher_encrypt, ctx->hd, newiv, 16, iv, 16);
	//verbose_md5((const char*)newiv);
	dec_plain(ctx->plain, out, in, newiv);
}

static void free_plain(void *priv) {
	gcry_cipher_close(((cbc_plain_t*)priv)->hd);
	wipememory(priv, sizeof(cbc_plain_t));
	free(priv);
}

static void free_essiv(void *priv) {
	free_plain(((cbc_essiv_t*)priv)->plain);
	gcry_cipher_close(((cbc_essiv_t*)priv)->hd);
	wipememory(priv, sizeof(cbc_essiv_t));
	free(priv);
}

const cipher_spec_t cipher_cbc_plain = {
	.init = init_plain,
	.enc = enc_plain,
	.dec = dec_plain,
	.free = free_plain,
	.name = "CBC_PLAIN"
};

const cipher_spec_t cipher_cbc_essiv = {
	.init = init_essiv,
	.enc = enc_essiv,
	.dec = dec_essiv,
	.free = free_essiv,
	.name = "CBC_ESSIV"
};
