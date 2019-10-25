#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <assert.h>

#include "verbose.h"
#include "random.h"
#include "blockio.h"
#include "juggler.h"

#define NO_DEVBLOCKS 6

void show_disk_header(uint32_t no_devblocks) {
	printf("  seq   nseq ");
	for (int i = 0; i < no_devblocks; i++) {
		printf(" %2d", i);
	}
	printf("\n");
}

void show_disk(blockio_info_t *disk, uint32_t no_devblocks, blockio_info_t *next,
		blockio_info_t *obsoleted) {
	printf("%5lu->%5lu: ", next->seqno, next->next_seqno);
	for (int i = 0; i < no_devblocks; i++) {
		blockio_info_t *b = &disk[i];
		if (b->seqno == b->next_seqno) {		// empty
			printf(" - ");
		} else if (b->seqno + 1 == b->next_seqno) {	// filler
			printf("[!]");
		} else if (b == obsoleted) {			// obsoleted
			printf(" ^ ");
		} else if (b != next) {				// old
			printf(" %c ", 'a' + b->id);
		} else {					// new	
			printf("[%c]", 'a' + b->id);
		}
	}
	printf("\n");
}

int main(int argc, char *argv[]) {
	blockio_info_t disk[NO_DEVBLOCKS] = { };
	int next_id = 0;
	random_t r;
	juggler_t j;

	verbose_init(argv[0]);

	random_init(&r);

	juggler_init(&j, &r);

	for (uint32_t i = 0; i < NO_DEVBLOCKS; i++)
		juggler_add_macroblock(&j, disk + i);

	show_disk_header(NO_DEVBLOCKS);

	for (int i = 0; i < 24; i++) {
		blockio_info_t *next = juggler_get_devblock(&j, 0);
		blockio_info_t *obsoleted = juggler_get_obsoleted(&j);

		if (next != obsoleted) {
			if (obsoleted) next->id = obsoleted->id;
			else next->id = next_id++;
		}

		show_disk(disk, NO_DEVBLOCKS, next, obsoleted);
	}

	juggler_free(&j);

	random_free(&r);

	exit(0);
}

#if 0
	uint32_t getnum(blockio_info_t *b, void *priv) {
		return b - (blockio_info_t*)priv;
	}
	juggler_verbose(&j, getnum, disk);

	juggler_free(&j);

	juggler_init(&j, &r);

	for (uint32_t i = 0; i < NO_DEVBLOCKS; i++)
		juggler_add_macroblock(&j, disk + i);

	assert(juggler_get_devblock(&j, 1));

	juggler_verbose(&j, getnum, disk);

	for (int i = 0; i < 24; i++) {
		blockio_info_t *next = juggler_get_devblock(&j, 0);
		blockio_info_t *obsoleted = juggler_get_obsoleted(&j);

		if (next != obsoleted) {
			if (obsoleted) next->id = obsoleted->id;
			else next->id = next_id++;
		}

		show_disk(disk, NO_DEVBLOCKS, next, obsoleted);
	}
#endif
