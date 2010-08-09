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
#include <assert.h>
#include "verbose.h"
#include "cipher.h"
#include "gcry.h"

static void *null_init(const char *name, size_t no_blocks,
		const void *key, size_t key_len) {
	assert(no_blocks > 0);
	WARNING("the NULL cipher mode should only be used for testing");
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
