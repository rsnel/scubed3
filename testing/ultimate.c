#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

#include "verbose.h"

#define NO_BLOCKS 12
#define SIMULTANEOUS_BLOCKS 6
#define STEPS 34

typedef struct previnfo_s {
	int index;    // index == -1 means that there is no previnfo
	int distance; // if (index == -1) the value of distance has no meaning
} previnfo_t;

typedef struct block_s {
	int datablock;       // bool: true if datablock, false if fillerblock
	previnfo_t previnfo; // previnfo is only valid if datablock is true
	int block;           // block number
} block_t;

typedef struct recurse_data_s {
	int highest_affected;
	int lowest_affected;
} recurse_data_t;


int input[] = {
	11, 6,  9, 4, 8, 1, 7, 3, 11, 10, 0, 4, 2,  6, 3, 9,
	 1, 5, 11, 0, 8, 7, 4, 9,  2,  3, 1, 8, 7, 11, 4, 2, 3, 8
};

previnfo_t cache[NO_BLOCKS] = { };
block_t blocks[STEPS];
int no_blocks = 0;

void iterate_cache(void (*func)(previnfo_t*)) {
	for (int i = 0; i < NO_BLOCKS; i++) func(&cache[i]);
}

void show_cache() {
	void show(previnfo_t *p) {
		VERBOSE("index=%d distance=%d", p->index, p->distance);
	}
	VERBOSE("status of cache at no_blocks=%d", no_blocks);
	iterate_cache(&show);
}

void show_blocks() {
	VERBOSE("status of blocks");
	for (int i = 0; i < no_blocks; i++) {
		block_t *b = &blocks[i];
		if (b->datablock) {
			if (b->previnfo.index == -1) VERBOSE("%d: %d DATA (no previnfo)", i, b->block);
			else VERBOSE("%d: %d DATA index=%d distance=%d", i, b->block, b->previnfo.index, b->previnfo.distance);
		}
		else VERBOSE("%d: %d FILLER", i, b->block);
	}
}

static void increment(previnfo_t *p) {
	p->distance++;
}
void decrease_distance(recurse_data_t *r, int removed) {
	for (int i = r->lowest_affected; i < no_blocks; i++) {
		block_t *b = &blocks[i];
		if (!b->datablock || b->previnfo.index == -1) {
			if (r->lowest_affected == 0) r->lowest_affected++;
			continue;
		}
		assert(i != removed);
		if (b->previnfo.index < removed) {
			b->previnfo.distance--;
			r->highest_affected = i;
		}
	}
}

int sanitize(recurse_data_t *r) {
	for (int i = r->highest_affected; i >= r->lowest_affected; i--) {
		block_t *b = &blocks[i], *prev;
		if (!b->datablock) continue;
		if (b->previnfo.index == -1) continue;
		if (b->previnfo.distance > SIMULTANEOUS_BLOCKS) continue;
		VERBOSE("problem at %d mark %d as invalid increase distance of this block and decrease distances", i, b->previnfo.index);
		prev = &blocks[b->previnfo.index];

		/* mark offending block as filler */
		prev->datablock = 0;

		/* increase distance of current block and update index */
		b->previnfo.distance += prev->previnfo.distance;
		b->previnfo.index = prev->previnfo.index;

		/* decrease distance by 1 of all following blocks that have a 
		 * prev block below the removed block */
		r->highest_affected = r->lowest_affected = prev - blocks + 1;
		decrease_distance(r, prev - blocks);
		VERBOSE("new lowest_affected=%d, highest_affected=%d", r->lowest_affected, r->highest_affected);

		return 1;
	}

	return 0; //done 
}

int main(int argc, char *argv[]) {
	int i;
	recurse_data_t r;

	verbose_init(argv[0]);

	for (i = 0; i < NO_BLOCKS; i++) cache[i].index = -1;

	show_cache();

	for (i = 0; i < sizeof(input)/sizeof(*input); i++) {
		block_t *b = blocks + no_blocks;

		b->block = input[i];
		b->previnfo = cache[b->block];
		b->datablock = 1;
		assert(i == 33 || b->previnfo.index == -1 || b->previnfo.distance > SIMULTANEOUS_BLOCKS);
		cache[b->block].index = i;
		cache[b->block].distance = 0;
		iterate_cache(&increment);
		no_blocks++;
	}

	show_cache();
	show_blocks();

	r.highest_affected = 33;
	r.lowest_affected = 33;
	while (sanitize(&r)) {
		VERBOSE("BLA");
	}
	show_blocks();
	exit(0);
}
