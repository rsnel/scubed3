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

void show_disk(macroblock_t *disk, uint32_t no_devblocks, macroblock_t *next) {
	printf("%5lu->%5lu: ", next->seqno, next->next_seqno);
	for (int i = 0; i < no_devblocks; i++) {
		macroblock_t *b = &disk[i];
		switch (b->state) {
			case MACROBLOCK_STATE_EMPTY_E:
				assert(b != next);
				printf(" - ");
				break;
			case MACROBLOCK_STATE_FILLER_E:
				if (b != next) printf(" ! ");
				else printf("[!]");
				break;
			case MACROBLOCK_STATE_OBSOLETED_E:
				assert(b != next);
				printf(" ^ ");
				break;
			case MACROBLOCK_STATE_OK_E:
				if (b != next) printf(" %c ", 'a' + b->id);
				else printf("[%c]", 'a' + b->id);
				break;
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

	juggler_init(&j, &r, disk);

	for (uint32_t i = 0; i < NO_DEVBLOCKS; i++)
		juggler_add_macroblock(&j, disk + i);

	show_disk_header(NO_DEVBLOCKS);

	for (int i = 0; i < 8192; i++) {
		macroblock_t *next = juggler_get_devblock(&j);
		assert(next->state != MACROBLOCK_STATE_OK_E);

		if (next->next_seqno == 1 + next->seqno) {
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

		show_disk(disk, NO_DEVBLOCKS, next);
	}

	juggler_free(&j);

	exit(0);
}
