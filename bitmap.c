/* bitmap.c - bitmap handler */
#include <stdint.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "verbose.h"
#include "bitmap.h"
#include "binio.h"

void bitmap_init(bitmap_t *b, uint32_t no_bits) {
	assert(b);
	if (!(b->bits = calloc(((no_bits+31)/32), sizeof(uint32_t))))
		FATAL("error allocating %d bytes: %s",
				sizeof(uint32_t)*((no_bits+31)/32),
				strerror(errno));
	b->no_bits = no_bits;
	b->no_set = 0;
}

void bitmap_setbits(bitmap_t *b, uint32_t set) {
	int i;
	assert(b && set <= b->no_bits);
	if (b->no_set) memset(b->bits, 0, 4*((b->no_bits+31)/32));
	b->no_set = set;
	for (i = 0; i < set/(8*sizeof(uint32_t)); i++)
		b->bits[i] = 0xFFFFFFFF;
	if ((i = set%(8*sizeof(uint32_t))))
		b->bits[set/(8*sizeof(uint32_t))] =
			0xFFFFFFFF>>(8*sizeof(uint32_t)-i);
}

int bitmap_getbit(bitmap_t *b, uint32_t index) {
	assert(b && index < b->no_bits);
	return b->bits[index/(8*sizeof(uint32_t))]&
		1<<(index%(8*sizeof(uint32_t)));
}

uint32_t bitmap_getbits(bitmap_t *b, uint32_t offset, uint8_t size) {
	uint32_t res = 0;
	uint32_t dword;
	uint8_t bit;
	assert(b && offset < b->no_bits);
	assert(offset + size <= b->no_bits);
	assert(size < 8*sizeof(uint32_t));
	assert(size);

	dword = offset/(8*(sizeof(uint32_t)));
	bit = offset%(8*(sizeof(uint32_t)));

	res = b->bits[dword]>>bit;
	if (bit + size < 8*sizeof(uint32_t)) {
		res &= 0xFFFFFFFF>>(8*sizeof(uint32_t) - size);
	} else if (bit + size > 8*sizeof(uint32_t)) {
		res |= b->bits[dword+1]<<(bit + size - 8*sizeof(uint32_t));
		res &= 0xFFFFFFFF>>(8*sizeof(uint32_t) - size);
	}

	//VERBOSE("dword=%u, bit=%u, size=%u, res=%08x", dword, bit, size, res);

	return res;
}

/* may only be called on a bit that is unset */
void bitmap_setbit(bitmap_t *b, uint32_t index) {
	assert(b);
	assert(index < b->no_bits);
	assert(!bitmap_getbit(b, index));
	assert(b->no_set);
	b->bits[index/(8*sizeof(uint32_t))] |=
		(1<<(index%(8*sizeof(uint32_t))));
	b->no_set++;
}

/* may only be called on a bit that is set */
void bitmap_clearbit(bitmap_t *b, uint32_t index) {
	assert(b);
	assert(index < b->no_bits);
	assert(bitmap_getbit(b, index));
	assert(b->no_set);
	b->bits[index/(8*sizeof(uint32_t))] &=
		~(1<<(index%(8*sizeof(uint32_t))));
	b->no_set--;
}

/* may be called on any bit */
void bitmap_clearbit_safe(bitmap_t *b, uint32_t index) {
	assert(b);
	assert(index < b->no_bits);
	if(bitmap_getbit(b, index)) b->no_set--;
	b->bits[index/(8*sizeof(uint32_t))] &=
		~(1<<(index%(8*sizeof(uint32_t))));
}

uint32_t bitmap_count(bitmap_t *b) {
	assert(b);
	return b->no_set;
}

void bitmap_free(bitmap_t *b) {
	assert(b);
	free(b->bits);
}

uint32_t bitmap_size(uint32_t no_bits) {
	return (2 + (no_bits+31)/32)*sizeof(uint32_t);
}

void bitmap_read(bitmap_t *b, const uint32_t *in) {
	int i;
	assert(b && in);
	b->no_set = binio_read_uint32_be(in++);
	for (i = 0; i < (b->no_bits+31)/32; i++) {
		b->bits[i] = binio_read_uint32_be(in++);
	}
}

void bitmap_write(uint32_t *out, bitmap_t *b) {
	int i;
	assert(b && out);
	binio_write_uint32_be(out++, b->no_set);
	for (i = 0; i < (b->no_bits+31)/32; i++) {
		binio_write_uint32_be(out++, b->bits[i]);
	}
}