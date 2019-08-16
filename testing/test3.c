#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include "dllist.h"
#include "verbose.h"
#include "random.h"

#define NO_BLOCKS 8
#define SIMULTANEOUS_BLOCKS 3
#define ANALYSIS_LENGTH 10000

typedef enum blockstate_e { EMPTY, FILLER, DATA } blockstate_t;

typedef struct blockinfo_s {
	uint64_t seqno;
	blockstate_t state;
} blockinfo_t;

typedef struct blockdest_s {
	int block;
	int locked;
	blockstate_t state;
} blockdest_t;

/*
typedef struct info_s {
	int block_no;
	int repeat_in;
	int index;
	int last_occurance;
	int wasted_at;
} info_t;
*/

#if 0
/* return first affected by first with amount up to and including amount, -1 if none */
int remove_first_with_amount(info_t *infos, int no_infos, int max_amount, int start) {
	int i = start;

	assert(start <= no_infos && start >= 0);

	while (i < no_infos && infos[i].index < no_infos && (infos[i].repeat_in > max_amount || infos[i].wasted_at)) i++;

	if (i == no_infos || infos[i].index >= no_infos) return -1;

	int amount = infos[i].repeat_in;

	//VERBOSE("found offender at %d", i);
	int last = infos[i].last_occurance;
	if (last >= 0) {
		assert(!infos[last].wasted_at);
		//VERBOSE("last ocurrance is %d", last);
		infos[last].repeat_in += amount;
		infos[last].index = infos[i].index;
	}
	infos[i].wasted_at = max_amount;
	if (infos[i].index < no_infos) {
		//VERBOSE("next one found at %d", infos[i].index);
		assert(!infos[infos[i].index].wasted_at);
		infos[infos[i].index].last_occurance = infos[i].last_occurance;
	} //else VERBOSE("next one not found");

	int first = i, k;
	for (k = 0; k < i; k++) {
		if (infos[k].index > i) {
			if (k < first) first = k;
			infos[k].repeat_in--;
		}
	}

	return first;
}

void show_blocks(info_t *infos, int no_infos) {
	int wasted[NO_BLOCKS] = { 0 };
	int i, max;
	for (i = 0; i < ANALYSIS_LENGTH; i++) {
		info_t *b = &infos[i];
		if (b->index == 0 || b->index >= ANALYSIS_LENGTH) break;
		if (!b->wasted_at) VERBOSE("repeat_in[%3d]={ %3d, %3d ( %3d, %3d ) }%s", i, b->block_no, b->repeat_in, b->index, b->last_occurance, b->wasted_at?" WASTED":"");
		else wasted[b->wasted_at]++;
	}
	int count = 0;
	max = i;
	VERBOSE("max %d", max);
	for (int i = 0; i < NO_BLOCKS; i++) {
		count += wasted[i];
		VERBOSE("wasted[%d]=%d %d %f", i, wasted[i], count, count*100./max);
	}
}

int main(int argc, char *argv[]) {
	//FILE *fp;
	random_t r;
	//uint64_t seqno;
	//blockinfo_t blockinfos[NO_BLOCKS] = { };

	verbose_init(argv[0]);

	//if (!(fp = fopen("random.test", "w"))) FATAL("error opening \"random test\"");
	//fprintf(fp, "type: d\ncount: %d\nnumbit: 6\n", 1024*64);

	random_init(&r, NO_BLOCKS);
	info_t infos[ANALYSIS_LENGTH];
	int last_index[NO_BLOCKS];
	int i;
	for (i = 0; i < NO_BLOCKS; i++) {
		last_index[i] = -1;
	}

	VERBOSE("number of blocks %d", NO_BLOCKS);
	VERBOSE("Analysis length %d", ANALYSIS_LENGTH);
	int unknown_repeat = 0;
	i = 0;

	for (i = 0; i < ANALYSIS_LENGTH; i++) {
//	do {
		int peek = random_peek(&r, i);
		VERBOSE("next block in sequence at %d is %d", i, peek);
		if (last_index[peek] == -1) {
			VERBOSE("has never been seen before");
		} else {
			if (!infos[last_index[peek]].repeat_in) {
				VERBOSE("has been seen before at %d", last_index[peek]);
				infos[last_index[peek]].repeat_in = i - last_index[peek];
				infos[last_index[peek]].index = i;
				unknown_repeat--;
			}
		}

		if (i < ANALYSIS_LENGTH) {
			infos[i].repeat_in = 0;
			infos[i].block_no = peek;
			infos[i].wasted_at = 0;
			infos[i].last_occurance = last_index[peek];
			unknown_repeat++;
			last_index[peek] = i;
		}

//		i++;
//		//VERBOSE("ok next! unknown is %d", unknown_repeat);
//	} while (unknown_repeat);
	}

	show_blocks(infos, ANALYSIS_LENGTH);

	for (int k = 1; k <= SIMULTANEOUS_BLOCKS; k++) {
		VERBOSE("---- eliminating repeats of %d ------", k);
		while (remove_first_with_amount(infos, ANALYSIS_LENGTH, k, 0) != -1) {
			//show_blocks(infos, ANALYSIS_LENGTH);
		}
		show_blocks(infos, ANALYSIS_LENGTH);
	}

	random_free(&r);
	exit(0);
}
#endif

void show_blockinfos(blockinfo_t *blockinfos) {
	for (int i = 0; i < NO_BLOCKS; i++) {
		blockinfo_t *b = blockinfos + i;
		VERBOSE("block %d: seqno=%ld, state=%s", i, b->seqno, (b->state == EMPTY)?"EMPTY":((b->state == DATA)?"DATA":"FILLER"));
	}
}

void show_blockdests(blockdest_t *blockdests, int no) {
	int count_filler_locked = 0, count_data_locked = 0, count_filler = 0, count_data_unsure = 0;
	for (int i = 0; i < no; i++) {
		blockdest_t *b = blockdests + i;
		if (b->state == DATA) {
			if (b->locked == 1) count_data_locked++;
			else count_data_unsure++;
		} else if (b->state == FILLER) {
			if (b->locked == 1) count_filler_locked++;
			else count_filler++;
		} else assert(0);

		VERBOSE("blockinfo %d: block=%d state=%s%s", i, b->block, (b->state == EMPTY)?"EMPTY":((b->state == DATA)?"DATA":"FILLER"), b->locked?" LOCKED":"");
	}
	VERBOSE("counts: data_locked=%d, filler_locked=%d, data_unsure=%d, %f", count_data_locked, count_filler_locked, count_data_unsure, count_data_locked/((double)count_data_locked + count_filler_locked));
}

int main(int argc, char *argv[]) {
	random_t r;
	uint64_t seqno = 0, fillerno = 0, discardno = 0;
	blockinfo_t blockinfos[NO_BLOCKS] = { };
	blockdest_t blockdests[ANALYSIS_LENGTH] = { };

	verbose_init(argv[0]);

	random_init(&r, NO_BLOCKS);

	VERBOSE("trail with NO_BLOCKS=%d, ANALISYS_LENGTH=%d, SIMULTANEOUS_BLOCKS=%d",
			NO_BLOCKS, ANALYSIS_LENGTH, SIMULTANEOUS_BLOCKS);

	int i;
	for (i = 0; i < ANALYSIS_LENGTH; i++) {
		//int peek = random_peek(&r, i);
		int peek = random_pop(&r);
		//VERBOSE("step %d, selected block is %d", i, peek);
		if (blockinfos[peek].state != DATA) {
			blockdests[i].block = peek;
			blockdests[i].state = DATA;
			blockinfos[peek].state = DATA;
			blockinfos[peek].seqno = ++seqno;
		} else {
			//FATAL("BLOCK %d already contains data", peek);
			fillerno++;
			blockdests[blockinfos[peek].seqno-1].state = FILLER;
			blockdests[i].block = peek;
			blockdests[i].state = DATA;
			blockinfos[peek].state = DATA;
			blockinfos[peek].seqno = ++seqno;
		}

		if (seqno - fillerno - 1 == SIMULTANEOUS_BLOCKS) {
			while (blockdests[discardno].state != DATA) {
				blockdests[discardno].locked = 1;
				discardno++;
			}
			blockdests[discardno].locked = 1;
			blockinfos[blockdests[discardno].block].state = EMPTY;

			fillerno++;
			discardno++;
			//FATAL("complete!");
		}

		//show_blockinfos(blockinfos);
		//show_blockdests(blockdests, i + 1);
	}

	//show_blockinfos(blockinfos);
	show_blockdests(blockdests, i);

	exit(0);
}
