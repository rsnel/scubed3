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

#define NO_BLOCKS 4
#define SIMULTANEOUS_BLOCKS 2
#define ANALYSIS_LENGTH 8

// demonstration of algorithm
//
//        012120
//  0     0
//  1     01
//  2     012
//  3     0123   (overwrite 1, difference<3, relabel from 1 to 3)
//  3.1   0-
//  3.2   0-1
//  3.3   0-12
//  4     0-123  (overwrite 2, difference<3, relabel from 2 to 4)
//  4.2   0--
//  4.3   0--1
//  4.4   0--12
//  5     0--123 (overwrite 0, difference=3, relabel from 0 to 5)
//  5.0   -
//  5.1   --
//  5.2   ---
//  5.3   ---0
//  5.4   ---01
//  5.5   ---012
//
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
//
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
//
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
//


typedef enum block_state_e { DATA, FILLER } block_state_t;

typedef struct seq_s {
	block_state_t state;
	int count, block;
} seq_t;

const char *block_state_string(block_state_t state) {
	switch (state) {
		case DATA:
			return "DATA";
		case FILLER:
			return "FILLER";
		default:
			assert(0);
	}
}

typedef struct disk_s {
	seq_t *seq;
} disk_t;

typedef struct dfcount_s {
	int data_counter;
	int fill_counter;
} dfcount_t;

int main(int argc, char *argv[]) {
	random_t r;
	dfcount_t dfcount = { };
	disk_t disk[NO_BLOCKS] = { };
	seq_t seq[ANALYSIS_LENGTH] = { };

	verbose_init(argv[0]);

	random_init(&r, NO_BLOCKS);

	VERBOSE("trial with NO_BLOCKS=%d, ANALYSIS_LENGTH=%d, SIMULTANEOUS_BLOCKS=%d",
			NO_BLOCKS, ANALYSIS_LENGTH, SIMULTANEOUS_BLOCKS);

	for (int i = 0; i < ANALYSIS_LENGTH; i++) {
		int block = random_pop(&r);
		VERBOSE("step %d, selected block is %d, data_counter %d", i, block, dfcount.data_counter);

		seq[i].block = block;
		seq[i].state = DATA;
		seq[i].count = dfcount.data_counter++;
		if (!disk[block].seq) {
			disk[block].seq = &seq[i];
		} else {
			FATAL("BLOCK %d already contains data", block);
		}
	}


	exit(0);
}

	/* random_push(&r, 0);
	random_push(&r, 2);
	random_push(&r, 2);
	random_push(&r, 3);
	random_push(&r, 2);
	random_push(&r, 3);
	random_push(&r, 0);
	random_push(&r, 2); */
	/*random_push(&r, 5);
	random_push(&r, 1);
	random_push(&r, 0);
	random_push(&r, 2);
	random_push(&r, 1);
	random_push(&r, 3);
	random_push(&r, 0);
	random_push(&r, 2);
	random_push(&r, 4);
	random_push(&r, 3);
	random_push(&r, 4);
	random_push(&r, 0);
	random_push(&r, 0);
	random_push(&r, 3);
	random_push(&r, 3);
	random_push(&r, 1);
	random_push(&r, 2);
	random_push(&r, 5);
	random_push(&r, 3);
	random_push(&r, 1);
	random_push(&r, 4);*/
