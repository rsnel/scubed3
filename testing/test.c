#include<stdio.h>
#include<stdlib.h>
#include<stdint.h>
#include<unistd.h>

FILE *fp_rand;

#define NO_BLOCKS 10

typedef struct blockinfo_s {
	uint64_t seqno;
	int used;
	int needed_blocks[NO_BLOCKS];
	int no_needed_blocks;
} blockinfo_t;

void print_status(blockinfo_t *b) {
	int i, j;
	printf("Status:\n");

	for (i = 0; i < NO_BLOCKS; i++) {
		printf("%03d %s % 5d needed:", i, b[i].used?"USED":"FREE", b[i].seqno);
		for (j = 0; j < b[i].no_needed_blocks; j++) printf(" %d", b[i].needed_blocks[j]);
		printf("\n");
	}
}

uint16_t randint(uint32_t no) {
        uint64_t rd;
        if (fread(&rd, sizeof(rd), 1, fp_rand) != 1) exit(1);

        return (uint32_t)(((double)no)*rd/(UINT64_MAX+1.0));
}

int main(int argc, char *argv[]) {
	blockinfo_t blocks[NO_BLOCKS] = { 0, };
	int i;
	int next;
	uint64_t seq = 0;

	if (!(fp_rand = fopen("/dev/urandom", "r"))) exit(1);

	for (;;) {
		print_status(blocks);
		seq++;
		next = randint(NO_BLOCKS);
		if (blocks[next].used) {
			printf("block %d is used\n", next);
			exit(1);
		}
		blocks[next].seqno = seq;
		blocks[next].used = 1;
		blocks[next].no_needed_blocks = 0;
		for (i = 0; i < NO_BLOCKS; i++) {
			if (blocks[i].used) blocks[next].needed_blocks[blocks[next].no_needed_blocks++] = i;
		}
		sleep(1);
	}
	
	exit(0);
}

