#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

#include "verbose.h"
#include "random.h"

#define NO_BLOCKS 6
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
	int safe_block = 0;
	for (int i = 0; i < NO_BLOCKS; i++) {
		if (!disk[i]) printf("[    ]"); 
		else {
			printf("[%4ld]", disk[i] - blocks);
			safe_block++;
		}
	}
	printf(" %d\n", safe_block);
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

int push_block(int block) {
	//recurse_data_t r;
	int max_backtrack = 0;//, backtrack;
	block_t *b = &blocks[no_blocks];

	assert(no_blocks < STEPS);

	b->block = block;
	b->previnfo = cache[b->block];
	b->datablock = 1;
	cache[b->block].index = no_blocks;
	cache[b->block].distance = 0;
	iterate_cache(&increment, NULL);

	//r.highest_affected = no_blocks;
	//r.lowest_affected = no_blocks;
	no_blocks++;

//	while ((backtrack = sanitize(&r))) {
//		if (backtrack > max_backtrack) max_backtrack = backtrack;
//	}

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
//	checkdistance();
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
