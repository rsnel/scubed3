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

//#define NO_BLOCKS 8
//#define HISTORY 3
#define NO_BLOCKS 16
#define HISTORY 4

typedef struct blockinfo_s {
	uint64_t seqno;
	uint64_t prev_seqno;
	int used;
} blockinfo_t;

static int last_diff(random_t *r, int last, int *first) {
	int i;
	assert(last >= 0);

	for (i = 0; i < last; i++)
		if (random_peek(r, i) == random_peek(r, last)) {
			if (i == 0) *first = 0;
			return 0;
		}

	return 1;
}

int main(int argc, char *argv[]) {
	//FILE *fp;
	random_t r;
	uint64_t seqno;
	blockinfo_t blocks[NO_BLOCKS] = { };

	verbose_init(argv[0]);

	//if (!(fp = fopen("random.test", "w"))) FATAL("error opening \"random test\"");
	//fprintf(fp, "type: d\ncount: %d\nnumbit: 6\n", 1024*64);

	random_init(&r, NO_BLOCKS);

	for (seqno = 0; seqno < 1024; seqno++) {
		int cleanup, i, next, needed, no_used = 0, valid2 = 1, different = 1, tmp = 0;
		while (different <= HISTORY) {
			tmp++;
			if (last_diff(&r, tmp, &valid2)) different++;
		}
		//cleanup = random_peek(&r, tmp);
		cleanup = random_peek(&r, 1);
		needed = blocks[cleanup].used;
#if 1
		fprintf(stderr, "sequence:");
		for (i = 0; i <= tmp; i++) {
			fprintf(stderr, " %d", random_peek(&r, i));
		}
		fprintf(stderr, "\n");
#endif

		next = random_pop(&r);

		if (blocks[next].used) assert(0);

		//if (valid2) blocks[random_peek(&r, tmp - 1)].used = 0;
		if (valid2) blocks[cleanup].used = 0;
		if (valid2) blocks[next].used = 1;
		for (i = 0; i < NO_BLOCKS; i++) {
			no_used += blocks[i].used;
		}
		VERBOSE("%2d %s (diffval %2d) (clean %2d %s) no_used=%2d", next, valid2?"VALID  ":"INVALID", tmp, valid2?cleanup:-1, valid2?(needed?"NEEDED":"UNNDED"):"IMPOSS", no_used);
		//sleep(1);
	}


	random_free(&r);
	//fclose(fp);
	exit(0);
}

