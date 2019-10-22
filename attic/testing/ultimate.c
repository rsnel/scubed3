#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

#include "verbose.h"
#include "random.h"

/* theory of operation:
 *
 * a scubed3 device acts like normal device on the
 * user side, but is implemented on top of a fixed
 * number of device_blocks, some kind of append-only
 * garbage collecting filesystem is used; device_blocks
 * are selected randomly and the selected device_block
 * is written such that each bit changes with
 * probability 1/2
 *
 * we need SIMULTANEOUS_BLOCKS number of block to
 * store the state of the device
 *
 * this code manages a set of device_blocks
 *
 * suppose a scubed3 device has NO_BLOCKS allocated
 * to it, we will number these blocks from 0 to
 * NO_BLOCKS - 1
 *
 * furthermore, a random number generator
 * generates block numbers
 *
 * a block must be completely rewritten once
 * its number comes up
 * 
 * each write is either a FILLER block (a block
 * with no data) or a DATA block (which has data)
 *
 * The role of a block is chosen such that each write
 * keeps the last SIMULTANEOUS_BLOCKS intact
 *
 * Let's look at a simple example. NO_BLOCKS=2
 * SIMULTANEOUS_BLOCKS=1. The RNG generates
 *
 * 001110101001101
 *
 * So the first device_block 0 must be written, it also 
 * will be immediately overwritten, so we make it a FILLER
 * block, the next write to 0 will be DATA.
 *
 * The complete sequence will look like
 *
 * 001110101001101.....
 * FDFFDDDDDFDFDD......
 *
 * This scheme ensures that:
 * - the blocks are written in random order
 * - the previous state of the device is not destroyed by a write
 */
#define NO_BLOCKS 12
#define SIMULTANEOUS_BLOCKS 3
#define STEPS 200

// example of reducing b6948173ba04263915b0874923187b4238
// to                  _69_817_ba_4263__5_08__9__1_7b4238
int input[] = {
	/*
	11, 6,  9, 4, 8, 1, 7, 3, 11, 10, 0, 4, 2,  6, 3, 9,
	 1, 5, 11, 0, 8, 7, 4, 9,  2,  3, 1, 8, 7, 11, 4, 2, 3, 8
	 */
	8, 10, 5, 0, 4, 8, 6, 8, 11, 5, 7, 6, 10, 7, 11, 1, 2, 1, 5, 6, 2, 6, 8, 7, 8, 9, 3, 7, 5, 3, 5, 3, 5, 1
};

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

previnfo_t cache[NO_BLOCKS] = { };
block_t blocks[STEPS];
int no_blocks = 0;
int no_filler = 0;
int matrix[NO_BLOCKS][NO_BLOCKS] = { };
block_t *disk[NO_BLOCKS] = { };
int disk_count = 0; // write count

void iterate_cache(void (*func)(previnfo_t*, void*), void *priv) {
	for (int i = 0; i < NO_BLOCKS; i++) func(&cache[i], priv);
}

void show_disk() {
	for (int i = 0; i < NO_BLOCKS; i++) {
		if (!disk[i]) printf("[EMPTY]"); 
		else printf("[%c%4ld]", disk[i]->datablock?'D':'F', disk[i] - blocks);
	}
	printf("\n");
}

void show_cache() {
	void show(previnfo_t *p, void *priv) {
		VERBOSE("block=%ld index=%d distance=%d", p - cache, p->index, p->distance);
	}
	VERBOSE("status of cache at no_blocks=%d", no_blocks);
	iterate_cache(&show, NULL);
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

static void setminusone(previnfo_t *p, void *priv) {
	p->index = -1;
}

static void increment(previnfo_t *p, void *priv) {
	p->distance++;
}

void decrease_distance(recurse_data_t *r, int removed) {
	void decrease_cache(previnfo_t *p, void *priv) {
		if (p->index < *((int*)priv)) p->distance--;
	}

	for (int i = removed + 1; i < no_blocks; i++) {
		block_t *b = &blocks[i];
		if (!b->datablock || b->previnfo.index == -1) {
			if (r->lowest_affected == i) r->lowest_affected++;
			continue;
		}
		if (b->previnfo.index < removed) {
			b->previnfo.distance--;
			r->highest_affected = i;
		}
	}

	iterate_cache(&decrease_cache, &removed);
}

void checkvalid() {
	int lastseen[NO_BLOCKS], lastdistance[NO_BLOCKS] = { }, i;

	for (i = 0; i < NO_BLOCKS; i++) {
		lastseen[i] = -1;
	}

	for (i = 0; i < no_blocks; i++) {
		block_t *b = &blocks[i];
		//VERBOSE("at index %d we have block %d as %d", i, b->block, b->datablock);
		if (b->datablock) {
			if (lastseen[b->block] != b->previnfo.index)
				FATAL("block %d non-matching previnfo.index=%d "
						"with lastseen=%d",
						i, b->previnfo.index, lastseen[b->block]);
			if (b->previnfo.index != -1) {
				if (lastdistance[b->block] != b->previnfo.distance)
				       	FATAL("block %d nonmatching "
							"previnfo.distance=%d "
							"with distance=%d",
							i, b->previnfo.distance,
							lastdistance[b->block]);
			}
			lastseen[b->block] = i;
			lastdistance[b->block] = 0;
			//VERBOSE("increment lastdistance");
			for (int j = 0; j < NO_BLOCKS; j++) lastdistance[j]++;
		}
	}
}

void checkdistance() {
	for (int i = 0; i < no_blocks; i++) {
		block_t *b = &blocks[i];
		if (b->datablock && b->previnfo.index != -1) {
			if (b->previnfo.distance <= SIMULTANEOUS_BLOCKS)
				FATAL("block %d, distance too low %d <= %d",
						i, b->previnfo.distance,
						SIMULTANEOUS_BLOCKS);
		}
	}
}

int sanitize(recurse_data_t *r) {
	for (int i = r->highest_affected; i >= r->lowest_affected; i--) {
		int backtrack;
		block_t *b = &blocks[i], *prev;
		if (!b->datablock) continue;
		if (b->previnfo.index == -1) continue;
		if (b->previnfo.distance > SIMULTANEOUS_BLOCKS) continue;
		//VERBOSE("problem at %d mark %d as invalid increase distance "
		//	"of this block and decrease distances", i, b->previnfo.index);
		prev = &blocks[b->previnfo.index];

		// no_blocks - 1 is the index of the newest block
		backtrack = no_blocks - 1 - b->previnfo.index;

		/* mark offending block as filler */
		prev->datablock = 0;
		no_filler++;

		/* increase distance of current block and update index */
		b->previnfo.distance += prev->previnfo.distance;
		b->previnfo.index = prev->previnfo.index;

		/* decrease distance by 1 of all following blocks that have a 
		 * prev block below the removed block */
		if (r->lowest_affected > prev - blocks + 1)
			r->lowest_affected = prev - blocks + 1;

		r->highest_affected = r->lowest_affected;

		decrease_distance(r, prev - blocks);
		//VERBOSE("new lowest_affected=%d, highest_affected=%d",
		//		r->lowest_affected, r->highest_affected);

		return backtrack;
	}

	return 0; //done 
}

int push_block(int block) {
	recurse_data_t r;
	int max_backtrack = 0, backtrack;
	block_t *b = &blocks[no_blocks];

	assert(no_blocks < STEPS);

	b->block = block;
	b->previnfo = cache[b->block];
	b->datablock = 1;
	cache[b->block].index = no_blocks;
	cache[b->block].distance = 0;
	iterate_cache(&increment, NULL);

	r.highest_affected = no_blocks;
	r.lowest_affected = no_blocks;
	no_blocks++;

	while ((backtrack = sanitize(&r))) {
		if (backtrack > max_backtrack) max_backtrack = backtrack;
	}

	return max_backtrack;
}

int main(int argc, char *argv[]) {
	random_t r;
	int backtrack, max_backtrack = 0;
	verbose_init(argv[0]);
	random_init(&r, NO_BLOCKS);

	iterate_cache(&setminusone, NULL);

	/*
	for (int i = sizeof(input)/sizeof(*input) - 1; i >= 0; i--) {
		random_push(&r, input[i]);
	}
	*/

	for (int i = 0; i < STEPS; i++) {
		if ((backtrack = push_block(random_pop(&r))) > max_backtrack)
			max_backtrack = backtrack;
	}

	//show_cache();
	//show_blocks();
	checkvalid();
	checkdistance();
	/*
	int last = -1;
	for (int i = 0; i < STEPS; i++) {
		if (blocks[i].datablock) {
			int cur = blocks[i].block;
			if (last != -1) {
				matrix[last][cur]++;
			}
			last = cur;
		}
	}

	for (int i = 0; i < NO_BLOCKS; i++) {
		for (int j = 0; j < NO_BLOCKS; j++) {
			printf(" %6d", matrix[i][j]);
		}
		printf("\n");
	}
	*/
	show_disk();
	for (int i = 0; i < 50; i++) {
		block_t *b = &blocks[i];
		disk[b->block] = b;
		disk_count++;
		show_disk();
	}
	VERBOSE("max_backtrack=%d, no_filler=%d, used=%f", max_backtrack, no_filler, (STEPS - no_filler)/((float)STEPS));
	exit(0);
}
