#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <math.h>
#include "verbose.h"
#include "random.h"

//#define TEST2

#define NO_BLOCKS 160
#define SIMULTANEOUS_BLOCKS 40
#define ANALYSIS_LENGTH 1000000
//#define NO_BLOCKS 3
//#define SIMULTANEOUS_BLOCKS 1
//#define ANALYSIS_LENGTH 300000

// demonstration of algorithm
//
//  test 1
//        20323
//
//  0     0
//  1     01
//  2     012
//  3     0123   (overwrite 2, difference=3 so no problem)
//  4     01234  (overwrite 3, difference=2, relabel from 2 to 4)
//  4.2   01-
//  4.3   01-2   (overwrite 2, difference=2, relabel from 0 to 3)
//  4.3.0 -
//  4.3.1 -0
//  4.3.2 -0-
//  4.3.3 -0-1
//  4.4   -0-12

#ifdef TEST1
#define NO_BLOCKS 4
#define SIMULTANEOUS_BLOCKS 2
#define ANALYSIS_LENGTH 5
#endif

//  test 2
//        20312015
//  0     0
//  1     01
//  2     012
//  3     0123
//  4     01234     (overwrite 2, difference>3, so no problem)
//  5     012345    (overwrite 0, differecen>3, so no problem)
//  6     0123456   (overwrite 1. difference=2, relabel from 3 to 6)
//  6.3   012-
//  6.4   012-3     (overwrite 2, difference=3, relabel from 0 to 4)
//  6.4.0 -
//  6.4.1 -0
//  6.4.2 -01
//  6.4.3 -01-
//  6.4.4 -01-2
//  6.5   -01-23    (overwrite 0, difference=3, relabel from 1 to 5)
//  6.5.1 --
//  6.5.2 --0
//  6.5.3 --0-
//  6.5.4 --0-1
//  6.5.5 --0-12
//  6.6   --0-123
//  7     --0-1234

#ifdef TEST2
#define NO_BLOCKS 6
#define SIMULTANEOUS_BLOCKS 3
#define ANALYSIS_LENGTH 8
#endif

//          20320323
//  0       0
//  1       01
//  2       012
//  3       0123      (overwrite 2, difference>2, so no problem)
//  4       01234     (overwrite 0, difference>2, so no problem)
//  5       012345    (overwrite 3, difference>2, so no problem)
//  6       0123456   (overwrite 2, difference>2, so no problem)
//  7       01234567  (overwrite 3, difference=2, relabel from 5 to 7)
//  7.5     01234-
//  7.6     01234-5   (overwrite 2, difference=2, relabel from 3 to 6)
//  7.6.3   012-
//  7.6.4   012-3     (overwrite 0, difference=2, relabel from 1 to 4)
//  7.6.4.1 0-
//  7.6.4.2 0-1
//  7.6.4.3 0-1-
//  7.6.4.4 0-1-2
//  7.6.5   0-1-23    (overwrite 3, difference=2, relabel from 2 to 5)
//  7.6.5.2 0--
//  7.6.5.3 0---
//  7.6.5.4 0---1
//  7.6.5.5 0---1-
//  7.6.6   0---1-2   (overwrite 2, difference 2, relabel from 0 to 6)
//  7.6.6.0 -
//  7.6.6.1 --
//  7.6.6.2 ---
//  7.6.6.3 ----
//  7.6.6.4 ----0
//  7.6.6.5 ----0-
//  7.6.6.6 ----0-1
//  7.7     ----0-12

#ifdef TEST3
#define NO_BLOCKS 6
#define SIMULTANEOUS_BLOCKS 3
#define ANALYSIS_LENGTH 8
#endif

typedef enum block_state_e { DATA, FILLER } block_state_t;

typedef struct seq_s {
	struct seq_s *prev;
	block_state_t state;
	int count, block_idx;
} seq_t;

typedef struct disk_s {
	seq_t *seq;
} disk_t;

typedef struct counter_s {
	int data;
	int filler;
} counter_t;

typedef struct depth_s {
	struct depth_s *next;
	long int i;
} depth_t;

depth_t *tail_depth(depth_t *d) {
	return d->next?tail_depth(d->next):d;
}

int count_depth(depth_t *d) {
	assert(d);
	return d->next?(count_depth(d->next) + 1):0;
}

typedef struct info_s {
	counter_t counter;
	//disk_t disk[NO_BLOCKS];
	disk_t *disk;
	//seq_t seq[ANALYSIS_LENGTH];
	seq_t *seq;
	int maxdepth;
	long int maxbacktrack;
} info_t;

long int rewind_to(seq_t *target, info_t *i, long int j) {
	seq_t *cur;

	do {
		cur = &i->seq[--j];
		if (cur->state == DATA) i->counter.data--;
		else if (cur->state == FILLER) i->counter.filler--;
		else assert(0);

		/* check that this block is actually on disk
		 * at this time at block_idx */
		assert(i->disk[cur->block_idx].seq == cur);

		/* remove the block */
		i->disk[cur->block_idx].seq = cur->prev;
	} while (cur != target);

	return j;
}

void do_block(seq_t *s, info_t *i, depth_t *d) {
	depth_t depth = { .next = d };
	seq_t *prev; // pointer to the block that will be overwritten (if any)
	seq_t **diskblock;
	int tmp = count_depth(d);
	long int tmp2 = tail_depth(d)->i - d->i;

	if (tmp > i->maxdepth) i->maxdepth = tmp;
	if (tmp2 > i->maxbacktrack) i->maxbacktrack = tmp2;

	//VERBOSE("attempt to place block %ld on disk at %d, depth=%d, dc=%d, fc=%d ",
	//		d->i, s->block_idx, count_depth(d), i->counter.data, i->counter.filler);

	diskblock = &i->disk[s->block_idx].seq;
	prev = *diskblock;

	if (prev) {
		/* if we overwrite a datablock, we must make
		 * sure that it is old enough */
		if (prev->state == DATA) {
			int age = i->counter.data - prev->count;
			//VERBOSE("writing on top of DATA-block count=%d, age is %d", prev->count, age);
			if (age <= SIMULTANEOUS_BLOCKS) {
				//VERBOSE("conflict while placing block %ld mark previous block as filler", d->i);

				/* rewind to the overwritten block */
				depth.i = rewind_to(prev, i, d->i);

				/* check that the returned index is accurate */
				assert(depth.i == prev - i->seq);

				/* mark last popped block as filler */
				prev->state = FILLER;

				/* replay, starting value is already in depth.i */
				for (; depth.i < d->i; depth.i++) {
					do_block(&i->seq[depth.i], i, &depth);
				}
			}
		}
	}

	*diskblock = s;
	s->prev = prev;
	if (s->state == DATA) {
		//VERBOSE("placed block %ld at %d as DATA %d at depth %d", d->i, s->block_idx, i->counter.data, count_depth(d));
		s->count = i->counter.data++;
	} else if (s->state == FILLER) {
		//VERBOSE("placed block %ld at %d as FILLER at depth %d", d->i, s->block_idx, count_depth(d));
		s->count = i->counter.filler++;
	} else assert(0);

}

int main(int argc, char *argv[]) {
	random_t r;
	depth_t depth = { };
	info_t info = { }; /* initialization to 0 sets all blocks to state DATA */

	verbose_init(argv[0]);

	info.disk = calloc(NO_BLOCKS, sizeof(disk_t));
	if (!info.disk) FATAL("out of memory allocating %d times %ld (%ld) bytes", NO_BLOCKS, sizeof(disk_t), NO_BLOCKS*sizeof(disk_t));

	info.seq = calloc(ANALYSIS_LENGTH, sizeof(seq_t));
	if (!info.seq) FATAL("out of memory allocating %d times %ld (%ld) bytes", ANALYSIS_LENGTH, sizeof(seq_t), ANALYSIS_LENGTH*sizeof(seq_t));

	random_init(&r, NO_BLOCKS);

#ifdef TEST1 // test 1
	assert(NO_BLOCKS == 4);
	assert(ANALYSIS_LENGTH == 5);
	assert(SIMULTANEOUS_BLOCKS == 2);
	random_push(&r, 3);
	random_push(&r, 2);
	random_push(&r, 3);
	random_push(&r, 0);
	random_push(&r, 2);
#endif
#ifdef TEST2 // test 2
	assert(NO_BLOCKS == 6);
	assert(ANALYSIS_LENGTH == 8);
	assert(SIMULTANEOUS_BLOCKS == 3);
	random_push(&r, 5);
	random_push(&r, 1);
	random_push(&r, 0);
	random_push(&r, 2);
	random_push(&r, 1);
	random_push(&r, 3);
	random_push(&r, 0);
	random_push(&r, 2);
#endif
#ifdef TEST3 // test 3
	assert(NO_BLOCKS == 6);
	assert(ANALYSIS_LENGTH == 8);
	assert(SIMULTANEOUS_BLOCKS == 3);
	random_push(&r, 3);
	random_push(&r, 2);
	random_push(&r, 3);
	random_push(&r, 0);
	random_push(&r, 2);
	random_push(&r, 3);
	random_push(&r, 0);
	random_push(&r, 2);
#endif

	for (depth.i = 0; depth.i < ANALYSIS_LENGTH; depth.i++) {
		seq_t *s = &info.seq[depth.i];
		s->block_idx = random_pop(&r);
		do_block(s, &info, &depth);
	}

	int fillstreak = 0;
	int max_fillstreak = 0;
	int density = 0;
	for (int i = 0; i < ANALYSIS_LENGTH; i++) {
		fprintf(stderr, info.seq[i].state == DATA?((i < depth.i - info.maxbacktrack)?"D":"d"):"-");
		if (info.seq[i].state == FILLER) fillstreak++;
		else {
			if (fillstreak > max_fillstreak) max_fillstreak = fillstreak;
			fillstreak = 0;
			density++;
		}
//		if (i%180 == 179) {
//			fprintf(stderr, " density: %f\n", density/(double)180);
//			density=0;
//		}

	}
	fprintf(stderr, "\n");


	VERBOSE("trial with NO_BLOCKS=%d, ANALYSIS_LENGTH=%d, SIMULTANEOUS_BLOCKS=%d",
			NO_BLOCKS, ANALYSIS_LENGTH, SIMULTANEOUS_BLOCKS);
	VERBOSE("fraction %f", info.counter.data/(double)depth.i);
	VERBOSE("maxdepth=%d, maxbacktrack=%ld, max_fillstreak=%d, log_back=%f, log_depth=%f", info.maxdepth, info.maxbacktrack, max_fillstreak, log((double)info.maxbacktrack), log((double)info.maxdepth));
#if 0
	VERBOSE("rewinding to first block");
	rewind_to(info.seq, &info, ANALYSIS_LENGTH);
	VERBOSE("replay");
	assert(ANALYSIS_LENGTH > 5*NO_BLOCKS);
	for (depth.i = 0; depth.i < ANALYSIS_LENGTH - 5*NO_BLOCKS; depth.i++) {
		do_block(&info.seq[depth.i], &info, &depth);
		fprintf(stderr, "%2ld:", depth.i);
		for (int i = 0; i < NO_BLOCKS; i++) {
			seq_t *s = info.disk[i].seq;
			fprintf(stderr, " %x/", i);
			if (s) {
				if (s->state == FILLER) fprintf(stderr, "%2ld-", s - info.seq);
				else {
					int age = info.counter.data - s->count;
					fprintf(stderr, "%2ld%s", s - info.seq, age<=SIMULTANEOUS_BLOCKS?"D":"~");
				}
			}
			else fprintf(stderr, "---");
		}
		fprintf(stderr, " next");
		for (int i = 1; i < 16; i++) {
			fprintf(stderr, " %x", info.seq[i+depth.i].block_idx);
		}
		fprintf(stderr, "\n");
	}
#endif
	exit(0);
}
