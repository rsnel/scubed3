/* cipher_abl.c - Arbitrary Block Length, wide blockcipher mode
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
/* This is an attempt of partially implementing ABL(-32-AES) from
 * http://grouper.ieee.org/groups/1619/email/rtf00000.rtf (October 28, 2004)
 * http://grouper.ieee.org/groups/1619/email/pdf00005.pdf (April 15, 2004).
 * Three remarks about the spec:
 * - the order of the letters A, B, C, D, E and F in the decryption equations
 *   is a bit confusing, this decryption routine is correct (at least it can
 *   decrypt() output from the encrypt()...)
 * - the GHASH definition in the October 28 2004 draft differs from the one
 *   in the GCM revised specification and the April 15 2004 draft of ABL while
 *   the latter two agree on the definition of GHASH. See the comment below.
 * - the two drafts disagree on the order of the data and associated data in
 *   GPRF/GHASH, I chose the former convention for now.
 */
#include <assert.h>
#include <string.h>
#include "util.h"
#include "gcry.h"
#include "binio.h"
#include "cipher.h"

typedef unsigned char block_t[16];

typedef struct abl_s {
	block_t H, L0, L1, M0, M1, lens;
	size_t rest_blocks;
	gcry_cipher_hd_t hd;
} abl_t;

#if 0
#include <stdio.h>
static void print_block(char *name, const block_t block) {
	int i;
	fprintf(stderr, "%s\t", name);

	for (i = 0; i < 16; i++) fprintf(stderr, "%02x", block[i]);
	fprintf(stderr, "\n");
}
#endif

static void xor_block(block_t out, const block_t in1, const block_t in2) {
	//int i;
	//for (i = 0; i < 16; i++) out[i] = in1[i] ^ in2[i];
	*((uint64_t*)out) = *((uint64_t*)in1) ^ *((uint64_t*)in2);
	*((uint64_t*)out + 1) = *((uint64_t*)in1 + 1) ^ *((uint64_t*)in2 + 1);
}

/* multiplication in GF(2^128), it uses 16-byte buffers. The msb in the
 * first byte represents coefficient if X^0 and the lsb of the last byte
 * represents the coefficient of X^127. The usual irreducible polynomial
 * x^128 + x^7 + x^2 + x^1 + 1 looks like E1000000 00000000 00000000 00000000 */
static int coeff_xn(const block_t b, int n) {
	int index = n/8;
	return (b[index]<<(n%8))&0x80; /* this must be interpreted as a bool */
}

static void mul_by_x(block_t b) {
	int mod = b[15]&0x01; /* store the coefficient of x^127 */
	b[15] = b[15]>>1 | b[14]<<7; b[14] = b[14]>>1 | b[13]<<7;
	b[13] = b[13]>>1 | b[12]<<7; b[12] = b[12]>>1 | b[11]<<7;
	b[11] = b[11]>>1 | b[10]<<7; b[10] = b[10]>>1 | b[9] <<7;
	b[9]  = b[9] >>1 | b[8] <<7; b[8]  = b[8] >>1 | b[7] <<7;
	b[7]  = b[7] >>1 | b[6] <<7; b[6]  = b[6] >>1 | b[5] <<7;
	b[5]  = b[5] >>1 | b[4] <<7; b[4]  = b[4] >>1 | b[3] <<7;
	b[3]  = b[3] >>1 | b[2] <<7; b[2]  = b[2] >>1 | b[1] <<7;
	b[1]  = b[1] >>1 | b[0] <<7; b[0]  = b[0] >>1;
	if (mod) b[0] ^= 0xE1;
}

static void mult(block_t Z, const block_t Y, const block_t X) {
	int n;
	block_t V;
	memcpy(V, X, 16);
	memset(Z, 0, 16);
	for (n = 0; n < 128; n++) {
		if (coeff_xn(Y, n)) xor_block(Z, Z, V);
		mul_by_x(V);
        }
}

/* end of nonoptimal gf128 operations */

/* increment last uint32_t in count by 1 (in bigendian format)
 * and don't touch the rest of the block */
static void inc(block_t count) {
	uint32_t c = be32_to_cpu(*((uint32_t*)count + 3));
	*((uint32_t*)count + 3) = cpu_to_be32(++c);
}

static void *abl_init(gcry_cipher_hd_t hd, size_t no_blocks) {
	abl_t *priv;
	assert(no_blocks > 0);
	priv = ecalloc(1, sizeof(abl_t));

	priv->hd = hd;
	priv->L0[15] = 1;
	priv->L1[15] = 2;
	priv->M0[15] = 3;
	priv->M1[15] = 4;

	priv->rest_blocks = no_blocks - 1;
	gcry_call(cipher_encrypt, priv->hd, priv->H, 16, NULL, 0);
	gcry_call(cipher_encrypt, priv->hd, priv->L0, 16, NULL, 0);
	gcry_call(cipher_encrypt, priv->hd, priv->L1, 16, NULL, 0);
	gcry_call(cipher_encrypt, priv->hd, priv->M0, 16, NULL, 0);
	gcry_call(cipher_encrypt, priv->hd, priv->M1, 16, NULL, 0);

        binio_write_uint64_be(priv->lens, 0x80);
        binio_write_uint64_be(priv->lens + 8, priv->rest_blocks<<7);

	return priv;
}

static void calc_ghash(abl_t *ctx, block_t out,
		const uint8_t *rest, const block_t iv) {
	int i;
	block_t tmp = { 0, };
	xor_block(tmp, tmp, iv);
	mult(out, tmp, ctx->H);

	for (i = 0; i < ctx->rest_blocks; i++) {
		xor_block(tmp, out, rest + 16*i);
		mult(out, tmp, ctx->H);
	}

	xor_block(tmp, out, ctx->lens);
	mult(out, tmp, ctx->H);
	wipememory(tmp, 16);
}

static void calc_f(abl_t *ctx, block_t out, const uint8_t *rest,
		const block_t iv, const block_t L) {
	calc_ghash(ctx, out, rest, iv);
	xor_block(out, out, L);
	gcry_call(cipher_encrypt, ctx->hd, out, 16, NULL, 0);
}

static void calc_gi(abl_t *ctx, block_t out, block_t count, block_t M) {
	xor_block(out, count, M);
	gcry_call(cipher_encrypt, ctx->hd, out, 16, NULL, 0);
	inc(count);
}

static void abl_enc(void *priv, uint8_t *out,
		const uint8_t *in, const uint8_t *iv) {
	abl_t *ctx = priv;
	int i;
	block_t tmp, tmp2;

	calc_f(ctx, tmp, in + 16, iv, ctx->L0);
	xor_block(out, in, tmp);

	memcpy(tmp, out, 16);
	for (i = 0; i < ctx->rest_blocks; i++) {
		calc_gi(ctx, tmp2, tmp, ctx->M0);
		xor_block(out + (i+1)*16, in + (i+1)*16, tmp2);
	}

	calc_f(ctx, tmp, out + 16, iv, ctx->L1);
	xor_block(out, out, tmp);

	memcpy(tmp, out, 16);
	for (i = 0; i < ctx->rest_blocks; i++) {
		calc_gi(ctx, tmp2, tmp, ctx->M1);
		xor_block(out + (i+1)*16, out + (i+1)*16, tmp2);
	}
	//print_block("enc31", out + 31*16);
	wipememory(tmp, 16);
	wipememory(tmp2, 16);
}

static void abl_dec(void *priv, uint8_t *out,
		const uint8_t *in, const uint8_t *iv) {
	abl_t *ctx = priv;
	int i;
	block_t tmp, tmp2;

	memcpy(tmp, in, 16);
	for (i = 0; i < ctx->rest_blocks; i++) {
		calc_gi(ctx, tmp2, tmp, ctx->M1);
		xor_block(out + (i+1)*16, in + (i+1)*16, tmp2);
	}

	calc_f(ctx, tmp, out + 16, iv, ctx->L1);
	xor_block(out, in, tmp);

	memcpy(tmp, out, 16);
	for (i = 0; i < ctx->rest_blocks; i++) {
		calc_gi(ctx, tmp2, tmp, ctx->M0);
		xor_block(out + (i+1)*16, out + (i+1)*16, tmp2);
	}

	calc_f(ctx, tmp, out + 16, iv, ctx->L0);
	xor_block(out, out, tmp);

	wipememory(tmp, 16);
	wipememory(tmp2, 16);
}

static void abl_free(void *priv) {
	wipememory(priv, sizeof(abl_t));
	free(priv);
}

const cipher_spec_t cipher_abl4 = {
	.init = abl_init,
	.enc = abl_enc,
	.dec = abl_dec,
	.free = abl_free,
	.name = "ABL4"
};
