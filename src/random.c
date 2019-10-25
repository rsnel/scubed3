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
// you have a D20 and want a random number 1, 2, ..., 9.
// Throw the D20, if you get a value below 19, divide by 9 and take the remainder
// if you get 19 or 20: throw again
//
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
// count = 13
// limit = 13*((32 - 13 + 1)/13) + 13 - 1 = 25
// 32 plays the role of UINT32_MAX
// (note that integer division is used here)
//
// count is the number of possibilities, so this function 
// can't be used to generate a random uint32...
uint32_t random_custom(random_t *r, uint32_t count) {
	assert(count);
	uint32_t tmp;
	
	// limit is the largest output of the rng we can use
	// use the trick with '- count' and '+ count' to avoid 64bit arithmetic
	// in 64 bit arithmetic we could have written
	// 	count*((UINT32_MAX + 1)/count) - 1
	// which leads to problems in 32 bits because UINT32_MAX + 1 == 0
	uint32_t limit = count*((UINT32_MAX - count + 1)/count) + count - 1;

	do tmp = random_uint32(r);
	while (tmp > limit); 

	return tmp%count;
}

void random_free(random_t *r) {
	// we can't handle errors in cleanup functions
	if (fclose(r->fp) == EOF)
		WARNING("closing /dev/urandom: %s", strerror(errno));
}
