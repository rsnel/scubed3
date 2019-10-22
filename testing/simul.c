#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>

#include "verbose.h"
#include "juggler.h"

#define NO_DEVBLOCKS 12

int main(int argc, char *argv[]) {
	juggler_t j;

	juggler_init_fresh(&j, NO_DEVBLOCKS);

	juggler_free(&j);

	exit(0);
}
