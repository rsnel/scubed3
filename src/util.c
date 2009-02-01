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
