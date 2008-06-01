#ifndef INCLUDE_SCUBED3_BITMAP_H
#define INCLUDE_SCUBED3_BITMAP_H 1

#include <stdint.h>

typedef struct {
	uint32_t no_bits;
	uint32_t no_set; /* is redundant but useful */
	uint32_t *bits;
} bitmap_t;

void bitmap_init(bitmap_t*, uint32_t);

void bitmap_setbits(bitmap_t*, uint32_t);

int bitmap_getbit(bitmap_t*, uint32_t);

uint32_t bitmap_getbits(bitmap_t*, uint32_t, uint8_t);

uint32_t bitmap_size(uint32_t);

void bitmap_setbit(bitmap_t*, uint32_t);

void bitmap_clearbit(bitmap_t*, uint32_t);

void bitmap_clearbit_safe(bitmap_t*, uint32_t);

void bitmap_free(bitmap_t*);

uint32_t bitmap_count(bitmap_t*);

void bitmap_read(bitmap_t*, const uint32_t*);

void bitmap_write(uint32_t*, bitmap_t*);

#endif /* INCLUDE_SCUBED3_BIT_H */
