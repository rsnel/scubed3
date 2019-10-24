#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <assert.h>

#include "verbose.h"
#include "random.h"
#include "macroblock.h"
#include "juggler.h"

#define NO_DEVBLOCKS 8

void show_disk_header(uint32_t no_devblocks) {
	printf("                   ");
	for (int i = 0; i < no_devblocks; i++) {
		printf(" %x     ", i);
	}
	printf("\n");
}

void show_disk(macroblock_t *disk, uint32_t no_devblocks, uint64_t seqno, uint32_t new) {
	printf("new=%2u seqno=%5lu:", new, seqno);
	for (int i = 0; i < no_devblocks; i++) {
		macroblock_t *b = &disk[i];
		switch (b->state) {
			case MACROBLOCK_STATE_EMPTY_E:
				printf(" -     ");
				break;
			case MACROBLOCK_STATE_FILLER_E:
				printf(" !     ");
				break;
			case MACROBLOCK_STATE_OBSOLETED_E:
				printf(" \"     ");
				break;
			case MACROBLOCK_STATE_OK_E:
				printf(" %c (%2ld)", 'a' + b->id, b->lifespan2);
				break;
		}
	}
	printf("\n");
}

int main(int argc, char *argv[]) {
	macroblock_t disk[NO_DEVBLOCKS] = { };
	int next_id = 0;
	uint64_t seqno;
	random_t r;
	juggler_t j;

	verbose_init(argv[0]);

	random_init(&r);

	juggler_init(&j, &r, disk);

	for (uint32_t i = 0; i < NO_DEVBLOCKS; i++)
		juggler_add_macroblock(&j, disk + i);

	juggler_verbose(&j);

	show_disk_header(NO_DEVBLOCKS);

	for (seqno = 1; seqno <= 24; seqno++) {
		macroblock_t *next = juggler_get_devblock(&j);
		assert(next->state != MACROBLOCK_STATE_OK_E);

		if (next->lifespan2 == 1) {
			next->state = MACROBLOCK_STATE_FILLER_E;
		} else {
			next->state = MACROBLOCK_STATE_OK_E;
			macroblock_t *obsoleted = juggler_get_obsoleted(&j);
			if (obsoleted) { 
				obsoleted->state = MACROBLOCK_STATE_OBSOLETED_E;
				assert(obsoleted != next);
				next->id = obsoleted->id;
			} else {
				next->id = next_id++;
			}
		}

		show_disk(disk, NO_DEVBLOCKS, seqno, next - disk);
	}

	juggler_verbose(&j);

	juggler_free(&j);

	exit(0);
}
