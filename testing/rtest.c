#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <assert.h>

#include "verbose.h"
#include "random.h"

#define COUNT 16
#define STEPS 1000000

int main(int argc, char *argv[]) {
	random_t r;
	int freq[COUNT] = { };

	verbose_init(argv[0]);

	random_init(&r);

	for (int i = 0; i < STEPS; i++) {
		uint32_t rand = random_custom(&r, COUNT);
//		VERBOSE("random_custom=%u", rand);
		freq[rand]++;
	}

	random_free(&r);

	for (int i = 0; i < COUNT; i++) {
		VERBOSE("freq[%d]=%d", i, freq[i]);
	}

	exit(0);
}
