/* gcry.c - some error checking versions of libgcrypt functions
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
#include <errno.h>
#include <stdint.h>
#include <pthread.h>
#include "assert.h"
#include "verbose.h"
#include "gcry.h"
#include "ecch.h"

static int gcry_outofcore_handler(void *arg, size_t size,
		unsigned int que) {
	ecch_throw(ECCH_DEFAULT,
			"libgcrypt out of memory allocating %d bytes", size);
}

/* needed to make libgcrypt thread safe */
GCRY_THREAD_OPTION_PTHREAD_IMPL;

void gcry_global_init(void) {
	const char *version;

	/* FIRST set thread callbacks */
	gcry_control(GCRYCTL_SET_THREAD_CBS, &gcry_threads_pthread);

	/* then init the library */
	version = gcry_check_version(NULL);
	DEBUG("using version %s of libgcrypt", version);

	/* is this needed? */
	gcry_control(GCRYCTL_INITIALIZATION_FINISHED, 0);

	/* make memory allocation failures fatal */
	gcry_set_outofcore_handler(&gcry_outofcore_handler, NULL);
}

void gcry_fatal(int err, const char *postfix) {
	char msg[GCRY_MSG_LEN];
	gpg_strerror_r(err, msg, GCRY_MSG_LEN - 1);
	msg[GCRY_MSG_LEN - 1] = '\0';
	ecch_throw(ECCH_DEFAULT, "gcry_%s: %s", postfix, msg);
}

uint32_t gcry_fastranduint32(uint32_t max) {
	uint64_t rd;
	gcry_create_nonce(&rd, sizeof(rd));

	return (uint32_t)(((double)max)*rd/(UINT64_MAX+1.0));
}

uint32_t gcry_randuint32(uint32_t max) {
	uint64_t rd;
	gcry_randomize(&rd, sizeof(rd), GCRY_VERY_STRONG_RANDOM);

	return (uint32_t)(((double)max)*rd/(UINT64_MAX+1.0));
}

