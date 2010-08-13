#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include "verbose.h"
#include "random.h"

static uint16_t randint(FILE *fp, uint32_t no) {
        uint64_t rd;
	assert(no > 0 && no <= 65536);
        if (fread(&rd, sizeof(rd), 1, fp) != 1) FATAL("reading from /dev/urandom: %s", strerror(errno));

        return (uint16_t)(((double)no)*rd/(UINT64_MAX+1.0));
}

uint16_t random_custom(random_t *r, uint32_t no) {
	assert(r && no <= 65536 && no > 0);
	return randint(r->fp, no);
}

void random_rescale(random_t *r, uint32_t no) {
	assert(r && no <= 65536);
	r->no = no;
}

void random_init(random_t *r, uint32_t no) {
	assert(r && no <= 65536);
	if (!(r->fp = fopen("/dev/urandom", "r"))) FATAL("opening /dev/urandom: %s", strerror(errno));
	r->no = no;
	r->buffer_size = 4;
	r->buffer_len = 0;
	if (!(r->buffer = calloc(sizeof(r->buffer[0]), r->buffer_size))) FATAL("allocating random buffer: %s", strerror(errno));
}

uint16_t random_pop(random_t *r) {
	uint16_t ret;

	if (r->buffer_len) {
		ret = r->buffer[0];
		memmove(&r->buffer[0], &r->buffer[1], (r->buffer_len--)*sizeof(r->buffer[0]));
	} else return randint(r->fp, r->no);

	return ret;
}

void random_flush(random_t *r) {
	r->buffer_len = 0;
}

void random_verbose(random_t *r) {
	int i;
	assert(r);
	VERBOSE("random status:");
	VERBOSE("| buffer_size=%d", r->buffer_size);
	VERBOSE("| buffer_len=%d", r->buffer_len);
	for (i = 0; i < r->buffer_len; i++)
		VERBOSE("| buffer[%d] = %u", i, r->buffer[i]);
}

uint16_t random_peek(random_t *r, uint32_t idx) {
	int chg = 0;
	assert(idx < 32*1024*1024);

	if (idx >= r->buffer_len) {
		while (idx > r->buffer_size) {
			r->buffer_size <<= 1;
			chg = 1;
		}

		if (chg && !(r->buffer = realloc(r->buffer, r->buffer_size*sizeof(r->buffer[0]))))
			FATAL("reallocating random buffer: %s", strerror(errno));

		while (idx >= r->buffer_len)
			r->buffer[r->buffer_len++] = randint(r->fp, r->no);
	}

	return r->buffer[idx];
}

void random_push(random_t *r, uint16_t val) {
	assert(r);
	assert(val < r->no);
	if (r->buffer_size == r->buffer_len) {
		r->buffer_size <<= 1;
		if (!(r->buffer = realloc(r->buffer, r->buffer_size*sizeof(r->buffer[0])))) FATAL("reallocating random buffer: %s", strerror(errno));
	}

	memmove(&r->buffer[1], &r->buffer[0], (r->buffer_len++)*sizeof(r->buffer[0]));
	r->buffer[0] = val;
}

void random_free(random_t *r) {
	free(r->buffer);
	if (r->fp) fclose(r->fp);
}
