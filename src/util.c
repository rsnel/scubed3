/* util.c - some utility functions
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
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <assert.h>

#include "verbose.h"
#include "util.h"

void *ecalloc(size_t nmemb, size_t size) {
	void *res = calloc(nmemb, size);
	if (!res) FATAL("allocating %u blocks of size %u: %s",
			nmemb, size, strerror(errno));
	return res;
}

char *estrdup(const char *str) {
	char *out;
	assert(str);

	out = strdup(str);
	if (!out) FATAL("not enough memory to copy string");

	return out;
}
/* for an explanation of simple deterministic pseudo random number
 * generators, search google for simple PRNG, the constants in this
 * particular one are deduced from Microsoft's "Visual Basic 6.0" */
static int random_state = 0x00A09E86;
static const int cst1 = 0x80FD43FD;
static const int cst2 = 0x00C39EC3;
static const int cst3 = 0x00FFFFFF;

uint32_t deterministic(uint32_t max) {
	uint32_t ret;
	random_state = (random_state*cst1 + cst2)&cst3;
	ret  = ((double)max)*((double)random_state)/((double)(cst3 + 1));
	return ret;
}

int unbase16(char *buf, size_t len) {
	int i;
	assert(!(len%2));

	for (i = 0; i < len; i += 2) {
		if (buf[i] >= '0' && buf[i] <= '9')
			buf[i/2] = 16*(buf[i] - '0');
		else if (buf[i] >= 'A' && buf[i] <= 'F')
			buf[i/2] = 16*(buf[i] - 'A' + 10);
		else if (buf[i] >= 'a' && buf[i] <= 'f')
			buf[i/2] = 16*(buf[i] - 'a' + 10);
		else return -1;
		if (buf[i+1] >= '0' && buf[i+1] <= '9')
			buf[i/2] += buf[i+1] - '0';
		else if (buf[i+1] >= 'A' && buf[i+1] <= 'F')
			buf[i/2] += buf[i+1] - 'A' + 10;
		else if (buf[i+1] >= 'a' && buf[i+1] <= 'f')
			buf[i/2] += buf[i+1] - 'a' + 10;
		else return -1;
	}
	return 0;
}


