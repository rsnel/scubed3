#ifndef INCLUDE_SCUBED3_BLOCKIO_H
#define INCLUDE_SCUBED3_BLOCKIO_H 1

#include <stdint.h>

#include "lsbd.h"
#include "dllist.h"
#include "cipher.h"
#include "bitmap.h"

typedef struct blockio_s blockio_t;

typedef struct blockio_info_s {
	dllist_elt_t elt;
	uint64_t seqno;
	char data_hash[16];
	char seqnos_hash[16];
	uint32_t *indices;
	uint16_t no_indices;
	uint16_t max_indices;
	uint16_t no_nonobsolete;
	struct blockio_dev_s *dev;
} blockio_info_t;

typedef struct blockio_dev_s {
	const char *name;
	blockio_t *b;
	cipher_t *c;
	bitmap_t used;
	uint32_t next_free_macroblock;
	void *tmp_idx;
	uint32_t no_macroblocks; /* assigned to this device */
	uint32_t scanning_at;
	uint64_t highest_seqno_seen;

	uint32_t reserved;

	uint32_t no_indexblocks;
	uint8_t mesoblk_log;
	uint32_t mesoblk_size;
	uint16_t mmpm; /* max mesoblocks per macroblock */
	uint8_t strip_bits;

	dllist_t used_blocks;

	blockio_info_t **macroblocks;
} blockio_dev_t;

struct blockio_s {
	uint32_t macroblock_size;
	uint8_t macroblock_log;
	uint32_t no_macroblocks; /* amount of raw macroblocks */

	blockio_info_t *blockio_infos;

	void (*read)(void*, void*, uint64_t, uint32_t);
	void (*write)(void*, const void*, uint64_t, uint32_t);
	void (*close)(void*);
	void *priv;
};

void blockio_init(blockio_t*, const char*, uint8_t);

void blockio_dev_init(blockio_dev_t*, blockio_t*, cipher_t*, uint8_t,
		const char*);

void blockio_dev_free(blockio_dev_t*);

void blockio_dev_read_header(blockio_dev_t*, uint32_t);

blockio_info_t *blockio_dev_get_new_macroblock(blockio_dev_t*);

blockio_info_t *blockio_dev_gc_which_macroblock(blockio_dev_t*, uint32_t);

void blockio_dev_read_mesoblk(blockio_dev_t*, void*, uint32_t, uint32_t);

void blockio_dev_read_mesoblk_part(blockio_dev_t*, void*, uint32_t,
		uint32_t, uint32_t, uint32_t);

int blockio_check_data_hash(blockio_info_t*, void*);

void blockio_dev_write_macroblock(blockio_dev_t*, const void*, blockio_info_t*);

void blockio_free(blockio_t*);

#endif /* INCLUDE_SCUBED3_BLOCKIO_H */
