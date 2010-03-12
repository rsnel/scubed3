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

typedef struct blockinfo_s {
	dllist_elt_t elt;
	uint64_t seqno;
	uint64_t prev_seqno;
	int used;
	int obsolete;
	int needed_blocks[NO_BLOCKS];
	int no_needed_blocks;
} blockinfo_t;

void print_status(blockinfo_t *b) {
	int i;

	for (i = 0; i < NO_BLOCKS; i++)
		printf("%7lld", b[i].seqno);
}

void print_extended_status(blockinfo_t *bs) {
	int i, j;

	printf("---------------------------------------------\n");
	printf("extended status\n");
	for (i = 0; i < NO_BLOCKS; i++) {
		blockinfo_t *b = &bs[i];
		printf("block %2d, seqno=%llu:", i, b->seqno);
		for (j = 0; j < b->no_needed_blocks; j++) {
			printf(" %2d", b->needed_blocks[j]);
		}
		printf("\n");
	}
}

int check_valid(blockinfo_t *b, int head) {
	int i;
	blockinfo_t *hd = &b[head];

	for (i = 0; i < hd->no_needed_blocks; i++) {
		blockinfo_t *oth = &b[hd->needed_blocks[i]];
		if (oth->seqno > hd->seqno || oth->used == 0) return 0;
	}

	return 1;
}

typedef struct checker_priv_s {
	blockinfo_t *b;
	uint64_t currseq;
	int no_valid;
} checker_priv_t;

static int checker(dllist_elt_t *elt, void *p) {
	checker_priv_t *priv = p;
	int no = ((blockinfo_t*)elt) - priv->b;
	if (priv->currseq != ((blockinfo_t*)elt)->seqno) return 0;
	if (check_valid(priv->b, no)) {
		VERBOSE("rev %llu valid", priv->currseq);
		priv->currseq--;
		priv->no_valid++;
		return 1;
	} else {
		VERBOSE("rev %llu invalid", priv->currseq);
		return 0;
	}
}

int main(int argc, char *argv[]) {
	dllist_t in_use;
	random_t r;
	blockinfo_t blocks[NO_BLOCKS] = { };
	int i, next, post_next;
	uint64_t seq = 0, prev_seq = 0;
	checker_priv_t checker_priv = {
		.b = blocks
	};

	verbose_init(argv[0]);

	random_init(&r, NO_BLOCKS);

	dllist_init(&in_use);

	for (;;) {
		seq++;
		next = random_pop(&r);
		while (next == (post_next = random_peek(&r, 0))) random_pop(&r);
		VERBOSE("next=%d, post_next=%d", next, post_next);
		if (blocks[next].used) {
			FATAL("impossible");
			//dllist_remove(&blocks[next].elt);
			//blocks[next].seqno = 0;
			//blocks[next].used = 0;
		}
		blocks[next].seqno = seq;
		blocks[next].prev_seqno = prev_seq;
		prev_seq = seq;
		blocks[next].used = 1;
		blocks[next].obsolete = 0;
		blocks[next].no_needed_blocks = 0;
		dllist_append(&in_use, &blocks[next].elt);
		if (blocks[post_next].used) {
			// free block
			//dllist_remove(&blocks[post_next].elt);
			//blocks[post_next].seqno = 0;
			//blocks[post_next].used = 0;
			blocks[post_next].obsolete = 1;
		}
		for (i = 0; i < NO_BLOCKS; i++) {
			if (blocks[i].used && !blocks[i].obsolete) blocks[next].needed_blocks[blocks[next].no_needed_blocks++] = i;
		}
		checker_priv.currseq = seq;
		checker_priv.no_valid = 0;
		dllist_iterate_backwards(&in_use, checker, &checker_priv);
		//print_status(blocks);
		//printf(": %d revisions valid\n", checker_priv.no_valid);
		print_extended_status(blocks);
		printf("->%d revisions valid\n", checker_priv.no_valid);
		if (blocks[post_next].seqno) {
			// free block
			dllist_remove(&blocks[post_next].elt);
			blocks[post_next].seqno = 0;
			blocks[post_next].used = 0;
		}

		sleep(1);
	}
	
	dllist_free(&in_use);
	random_free(&r);
	exit(0);
}

#if 0
		for (i = 0; i < NO_BLOCKS; i++) {
			if (blocks[i].used == 1) {
				printf("rev %lld is %s\n", blocks[i].seqno, check_valid(blocks, i)?"valid":"invalid");
			}
		}
#endif

#if 0

	random_verbose(&r);

	printf("peek_3 %u\n", random_peek(&r, 3));
	random_verbose(&r);

	printf("peek_10 %u\n", random_peek(&r, 10));
	random_verbose(&r);

	for (i = 0; i < 4; i++) {
		printf("%d, %u\n", i, random_peek(&r, i));
	}

	for (i = 0; i < 4; i++) {
		printf("%d, %u\n", i, random_pop(&r));
	}
	random_verbose(&r);
#endif
