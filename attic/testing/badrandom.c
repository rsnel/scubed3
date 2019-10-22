#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include "dllist.h"
#include "verbose.h"
#include "random.h"

/* demonstration of bad random numbers */

#define NO_BLOCKS 256
#define ANALYSIS_LENGTH 1024*1024
#define AGE 192

int main(int argc, char *argv[]) {
	random_t r;
	unsigned char out;
	int ages[NO_BLOCKS];
	int i;

	verbose_init(argv[0]);
	random_init(&r, NO_BLOCKS);

	for (i = 0; i < NO_BLOCKS; i++) ages[i] = -AGE - 1;

	for (i = 0; i < ANALYSIS_LENGTH; i++) {
		//VERBOSE("now at i=%d", i);
		//for (int j = 0; j < NO_BLOCKS; j++) {
		//	VERBOSE("before ages[%d]=%d", j, ages[j]);
		//}
		do {
			out = random_pop(&r);
		} while (i - ages[out] <= AGE);
		ages[out] = i;
		fwrite(&out, 1, 1, stdout);
		//VERBOSE("%d", out);
	}
}

