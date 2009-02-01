/* cipher_null.c - null cipher, for testing
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
#include "cipher.h"
#include "gcry.h"

static void *null_init(gcry_cipher_hd_t hd, size_t no_blocks) {
	assert(no_blocks > 0);
	return (void*)no_blocks;
}

static void null_cipher(void *priv, uint8_t *out,
		const uint8_t *in, const uint8_t *iv) {
	if (in != out) memmove(out, in, (size_t)priv*16);
}

static void null_free(void *priv) {
}

const cipher_spec_t cipher_null = {
	.init = null_init,
	.enc = null_cipher,
	.dec = null_cipher,
	.free = null_free,
	.name = "NULL"
};
