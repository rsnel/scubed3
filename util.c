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
