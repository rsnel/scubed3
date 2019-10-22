#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>

#include "verbose.h"
#include "random.h"
#include "juggler.h"

#define NO_DEVBLOCKS 100

int main(int argc, char *argv[]) {
	uint32_t lifespan;
	random_t r;
	juggler_t j;

	verbose_init(argv[0]);

	random_init(&r);

	juggler_init_fresh(&j, &r, NO_DEVBLOCKS);

	juggler_verbose(&j);

	for (int i = 0; i < 100; i++) {
		VERBOSE("(%d) block=%d", lifespan, juggler_get_devblock(&j, &lifespan));
		juggler_verbose(&j);
	}

	juggler_free(&j);

	exit(0);
}
