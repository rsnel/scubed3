#include<stdio.h>
#include<stdint.h>
#include<string.h>
#include<assert.h>
#include<errno.h>
#include"verbose.h"
#include"random.h"


void random_init(random_t *r) {
	if (!(r->fp = fopen("/dev/urandom", "r")))
		FATAL("opening /dev/urandom: %s", strerror(errno));
}

uint32_t random_uint32(random_t *r) {
	uint32_t ret;

	if (fread(&ret, sizeof(ret), 1, r->fp) != 1)
                FATAL("reading from /dev/urandom: %s", strerror(errno));

        return ret;
}

// the idea of this function is as follows,
// suppose we have a random number generator that randomly
// generates 0, 1, 2, 3, ..., 31 (32 possibilities)
// and suppose we want to generate a number from 0 to 12 with it
// we then say:
// - there are 13 possibilties
// - these thirteen possibilities fit 2 times in 32
// - if we generate a number >= 2*13, retry
// - take result%13 as the answer
// 
// values of variables for this example
// max = 12
// count = 13
// limit = 13*((32 - 12)/13) + 12 = 25
// 32 plays the role of UINT32_MAX
// (note that integer division is used here)
uint32_t random_custom(random_t *r, uint32_t max) {
	/* edge case */
	if (max == UINT32_MAX) return random_uint32(r);

	uint32_t tmp, count = max + 1; // since max < UINT32_MAX
				       // count is well defined
	
	// limit is the largest output of the rng we can use
	// use the trick with - max and + max to avoid 64bit arithmetic
	uint32_t limit = count*((UINT32_MAX - max)/count) + max;

	do tmp = random_uint32(r);
	while (tmp > limit); 

	return tmp%count;
}

void random_free(random_t *r) {
	if (fclose(r->fp) == EOF) WARNING("closing /dev/urandom: %s", strerror(errno));
}
