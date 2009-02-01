/* cipher_cbc_large.c - Cipher Block Chaining mode (chain up to mesoblock size)
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
#include <string.h>
#include "util.h"
#include "gcry.h"
#include "binio.h"
#include "cipher.h"

typedef unsigned char block_t[16];

typedef struct cbc_large_s {
	gcry_cipher_hd_t hd;
	size_t no_blocks;
} cbc_large_t;

static void xor_block(block_t out, const block_t in1, const block_t in2) {
	//int i;
	//for (i = 0; i < 16; i++) out[i] = in1[i] ^ in2[i];
	*((uint64_t*)out) = *((uint64_t*)in1) ^ *((uint64_t*)in2);
	*((uint64_t*)out + 1) = *((uint64_t*)in1 + 1) ^ *((uint64_t*)in2 + 1);
}

static void *cbc_large_init(gcry_cipher_hd_t hd, size_t no_blocks) {
	cbc_large_t *priv;
	assert(no_blocks > 0);
	priv = ecalloc(1, sizeof(cbc_large_t));

	priv->no_blocks = no_blocks;
	priv->hd = hd;

	return priv;
}

static void cbc_large_enc(void *priv, uint8_t *out,
		const uint8_t *in, const uint8_t *iv) {
	cbc_large_t *ctx = priv;
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

static void cbc_large_dec(void *priv, uint8_t *out,
		const uint8_t *in, const uint8_t *iv) {
	cbc_large_t *ctx = priv;
	block_t tmp, tmp2;
	int i = ctx->no_blocks;

	memcpy(tmp, iv, 16);

	while (i--) {
		memcpy(tmp2, in, 16);
		gcry_call(cipher_decrypt, ctx->hd, out, 16, in, 16);
		xor_block(out, out, tmp);
		memcpy(tmp, tmp2, 16);
		out += 16;
		in += 16;
	}
}

static void cbc_large_free(void *priv) {
	wipememory(priv, sizeof(cbc_large_t));
	free(priv);
}

const cipher_spec_t cipher_cbc_large = {
	.init = cbc_large_init,
	.enc = cbc_large_enc,
	.dec = cbc_large_dec,
	.free = cbc_large_free,
	.name = "CBC_LARGE"
};
