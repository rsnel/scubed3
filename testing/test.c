#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <limits.h>

#include "verbose.h"
#include "random.h"

#define NO_DEVBLOCKS 6
#define NO_SEQBLOCKS 64

typedef struct block_s {
	struct block_s **devblock; 
	int lifespan;
} block_t;

void show_dev(block_t **devblocks, int no_devblocks, block_t *seqblocks) {
	int safe_block = 0;
	for (int i = 0; i < no_devblocks; i++) {
		if (!devblocks[i]) printf("[    ]"); 
		else {
			printf("[%4ld]", devblocks[i] - seqblocks);
			safe_block++;
		}
	}
	printf(" %d\n", safe_block);
}

void show_blocks(block_t *seqblocks, int no_seqblocks, block_t **devblocks) {
	VERBOSE("                              0 1 2 3 4 5");
	for (int i = 0; i < no_seqblocks; i++) {
		block_t *b = &seqblocks[i];
		VERBOSE("seqno=%3d lifespan=%2d devno=%ld            ", i, b->lifespan, b->devblock - devblocks);
	}
}

void decrement_array(int *array, int no_array) {
	for (int i = 0; i < no_array; i++) {
		if (array[i] == -1) continue;
		if (array[i] == 0) FATAL("impossible");
		array[i]--;
	}
}

void check_lifespans(block_t *seqblocks, int no_seqblocks, block_t **devblocks, int no_devblocks) {
	int i, expecto[no_devblocks];

	for (i = 0; i < no_devblocks; i++) expecto[i] = -1;

	for (i = 0; i < no_seqblocks; i++) {
		block_t *b = &seqblocks[i];

		if (expecto[b->devblock - devblocks] > 0)
			FATAL("seqblock %d appears at wrong time", i);
		expecto[b->devblock - devblocks] = b->lifespan;
		decrement_array(expecto, no_devblocks);
	}
}

void push_block(block_t *seqblock, block_t *seqblocks, block_t **devblock) {
	seqblock->lifespan = INT_MAX;
	seqblock->devblock = devblock;
	if (*devblock) (*devblock)->lifespan = seqblock - *devblock;
	*devblock = seqblock;
}

int main(int argc, char *argv[]) {
	block_t *devblocks[NO_DEVBLOCKS] = { };
	block_t seqblocks[NO_SEQBLOCKS];
	random_t r;

	verbose_init(argv[0]);
	random_init(&r, NO_DEVBLOCKS);

	for (int i = 0; i < NO_SEQBLOCKS; i++) {
		push_block(&seqblocks[i], seqblocks, &devblocks[random_pop(&r)]);
	}

	show_blocks(seqblocks, NO_SEQBLOCKS, devblocks);
	check_lifespans(seqblocks, NO_SEQBLOCKS, devblocks, NO_DEVBLOCKS);

	exit(0);
}
