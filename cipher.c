#include <assert.h>
#include "cipher.h"
#include "verbose.h"

const cipher_spec_t *cipher_specs[] = {
	&cipher_abl4
};

#define NO_CIPHERS (sizeof(cipher_specs)/sizeof(cipher_specs[0]))

void cipher_init(cipher_t *w, const char *name, size_t size,
		const uint8_t *key, size_t key_len) {
	int i = 0, algo;
	size_t block_size, bkey_len;
	const cipher_spec_t *mode_spec = NULL;
	assert(w && name && size > 0);

	/* parse name of the form MODE(PRIM) */
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

	gcry_call(cipher_open, &w->hd, GCRY_CIPHER_AES256,
			GCRY_CIPHER_MODE_ECB, 0);
	gcry_call(cipher_setkey, w->hd, key, 32);

	w->spec = mode_spec;
	w->ctx = w->spec->init(w->hd, size);
}

void cipher_enc(cipher_t *w, uint8_t *out,
		const uint8_t *in, const uint8_t *iv) {
	assert(w && w->spec && w->spec->enc && w->ctx);
	w->spec->enc(w->ctx, out, in, iv);
}

void cipher_dec(cipher_t *w, uint8_t *out,
		const uint8_t *in, const uint8_t *iv) {
	assert(w && w->spec && w->spec->dec && w->ctx);
	w->spec->dec(w->ctx, out, in, iv);
}

void cipher_free(cipher_t *w) {
	assert(w && w->spec && w->spec->free && w->ctx);
	w->spec->free(w->ctx);
	gcry_cipher_close(w->hd);
}
