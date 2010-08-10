/* blockio.h - handles block input/output
 *
 * Copyright (C) 2009  Rik Snel <rik@snel.it>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#ifndef INCLUDE_SCUBED3_BLOCKIO_H
#define INCLUDE_SCUBED3_BLOCKIO_H 1

#include <stdint.h>

#include "scubed3.h"
#include "dllist.h"
#include "cipher.h"
#include "bitmap.h"
#include "random.h"

typedef struct blockio_s blockio_t;

typedef struct blockio_info_s {
	dllist_elt_t elt; // for used, free and selected lists
	dllist_elt_t elt2; // for ordered
	uint64_t seqno, next_seqno;
	uint16_t layout_revision;
	char data_hash[32];
	char seqnos_hash[32];
	uint32_t no_indices;
	uint32_t no_indices_gc;
	uint32_t no_nonobsolete;
	uint32_t *indices;
	struct blockio_dev_s *dev;
} blockio_info_t;

typedef struct blockio_dev_s {
	char *name;
	blockio_t *b;
	cipher_t *c;
	int updated;
	bitmap_t status; // record status of all macroblocks with
			 // respect to this device

	uint64_t next_seqno;
	uint16_t no_macroblocks; /* assigned to this device */
	uint16_t reserved_macroblocks; /* visible in scubed file */
	blockio_info_t *bi;
	int valid;

	/* state of prng */
	uint16_t layout_revision;
	uint16_t tail_macroblock_global;
	uint32_t random_len;
	random_t r;

	uint8_t keep_revisions;
	
	uint16_t mmpm; /* max mesoblocks per macroblock */

	dllist_t used_blocks, free_blocks, selected_blocks;
	dllist_t ordered;

	/* array, used with PRNG to select random blocks */
	uint16_t *macroblock_ref;

	uint8_t *tmp_macroblock;

	/* stats */

	uint32_t writes; // no macroblocks

	/* stats in mesoblocks */

	uint64_t useful;
	uint64_t wasted_keep;
	uint64_t wasted_gc;
	uint64_t wasted_empty;
} blockio_dev_t;

struct blockio_s {
	uint32_t macroblock_size;
	uint8_t macroblock_log;
	uint32_t no_macroblocks; /* amount of raw macroblocks */

	uint8_t mesoblk_log;

	blockio_info_t *blockio_infos;

	random_t r;

	void (*read)(void*, void*, uint64_t, uint32_t);
	void (*write)(void*, const void*, uint64_t, uint32_t);
	void (*close)(void*);
	void *priv;
};

void blockio_init_file(blockio_t*, const char*, uint8_t, uint8_t);

void blockio_dev_init(blockio_dev_t*, blockio_t*, cipher_t*, const char*);

void blockio_dev_free(blockio_dev_t*);

void blockio_dev_read_header(blockio_dev_t*, uint32_t, uint64_t*);

blockio_info_t *blockio_dev_get_new_macroblock(blockio_dev_t*);

blockio_info_t *blockio_dev_gc_which_macroblock(blockio_dev_t*, uint32_t);

void blockio_dev_read_mesoblk(blockio_dev_t*, void*, uint32_t, uint32_t);

void blockio_dev_read_mesoblk_part(blockio_dev_t*, void*, uint32_t,
		uint32_t, uint32_t, uint32_t);

int blockio_check_data_hash(blockio_info_t*);

void blockio_dev_write_current_macroblock(blockio_dev_t*);

void blockio_free(blockio_t*);

void blockio_dev_select_next_macroblock(blockio_dev_t*, int);

void blockio_dev_write_current_and_select_next_valid_macroblock(
		blockio_dev_t*);

typedef enum blockio_dev_macroblock_status_e {
		NOT_ALLOCATED, HAS_DATA, FREE, SELECTFROM }
	blockio_dev_macroblock_status_t;

//void blockio_dev_set_macroblock_status(blockio_dev_t*,
//		uint32_t, blockio_dev_macroblock_status_t);

void blockio_dev_change_macroblock_status(blockio_dev_t*,
		uint32_t, blockio_dev_macroblock_status_t,
		blockio_dev_macroblock_status_t);

blockio_dev_macroblock_status_t blockio_dev_get_macroblock_status(
		blockio_dev_t*, uint32_t);

// returns errors -1: not enough blocks available, -2 out of memory
int blockio_dev_allocate_macroblocks(blockio_dev_t*, uint16_t);

#endif /* INCLUDE_SCUBED3_BLOCKIO_H */
