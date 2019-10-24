#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <assert.h>

#include "verbose.h"
#include "random.h"

int main(int argc, char *argv[]) {
	random_t r;

	verbose_init(argv[0]);

	random_init(&r);

	for (int i = 0; i < 128; i++)
		VERBOSE("random_custom=%u", random_custom(&r, 16));

	random_free(&r);

	exit(0);
}
