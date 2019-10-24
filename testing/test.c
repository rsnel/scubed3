#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <assert.h>

#include "verbose.h"
#include "random.h"
#include "macroblock.h"
#include "juggler.h"

#define NO_DEVBLOCKS 6

void show_disk_header(uint32_t no_devblocks) {
	printf("             ");
	for (int i = 0; i < no_devblocks; i++) {
		printf("  %x", i);
	}
	printf("\n");
}

void show_disk(macroblock_t *disk, uint32_t no_devblocks, macroblock_t *next,
		macroblock_t *obsoleted) {
	printf("%5lu->%5lu: ", next->seqno, next->next_seqno);
	for (int i = 0; i < no_devblocks; i++) {
		macroblock_t *b = &disk[i];
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
	macroblock_t disk[NO_DEVBLOCKS] = { };
	int next_id = 0;
	random_t r;
	juggler_t j;

	verbose_init(argv[0]);

	random_init(&r);

	juggler_init(&j, &r);

	for (uint32_t i = 0; i < NO_DEVBLOCKS; i++)
		juggler_add_macroblock(&j, disk + i);

	show_disk_header(NO_DEVBLOCKS);

	assert(juggler_get_devblock(&j, 1));

	for (int i = 0; i < 24; i++) {
		macroblock_t *next = juggler_get_devblock(&j, 0);
		macroblock_t *obsoleted = juggler_get_obsoleted(&j);

		if (next != obsoleted) {
			if (obsoleted) next->id = obsoleted->id;
			else next->id = next_id++;
		}

		show_disk(disk, NO_DEVBLOCKS, next, obsoleted);
	}

	juggler_verbose(&j, disk);

	juggler_free(&j);

	juggler_init(&j, &r);

	for (uint32_t i = 0; i < NO_DEVBLOCKS; i++)
		juggler_add_macroblock(&j, disk + i);

	juggler_verbose(&j, disk);

	for (int i = 0; i < 24; i++) {
		macroblock_t *next = juggler_get_devblock(&j, 0);
		macroblock_t *obsoleted = juggler_get_obsoleted(&j);

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
