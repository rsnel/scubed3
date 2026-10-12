/* blockio.h - handles block input/output
 *
 * Copyright (C) 2019  Rik Snel <rik@snel.it>
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

#include "dllarr.h"
#include "cipher.h"
#include "bitmap.h"
#include "random.h"
#include "pthd.h"

typedef struct blockio_info_s blockio_info_t;

#define BLOCKIO_MAX_TRIMS 8

#include "juggler.h"


typedef struct blockio_s blockio_t;

struct blockio_info_s {
	uint32_t pool_index; // position in the pool of the juggler

	/* for unallocated, replay and release */
	dllarr_elt_t ur;
	uint64_t seqno;
	uint32_t next_block; /* from the index: the block written after it */
	char data_hash[32];
	//char seqnos_hash[32];

	uint32_t no_indices;
	uint32_t no_nonobsolete;
	uint32_t *indices;

	/* the scubed device associated with this block, if any */
	struct blockio_dev_s *dev;
};

typedef struct blockio_dev_s {
	char *name;
	blockio_t *b;
	cipher_t *c;
	dllarr_t replay;

	/* removed blocks, they are given to the pool of unallocated blocks
	 * after the removal is on disk (in the index of the next write) */
	dllarr_t release;
	int updated; /* do we need to write this block? */
	bitmap_t status; // record status of all macroblocks with
			 // respect to this device

	uint32_t no_macroblocks;
	uint32_t reserved_macroblocks;

	/* shrinking: no_macroblocks - pending_removals is the target size,
	 * blocks are removed one by one, in the order the juggler offers
	 * them, when they hold no data */
	uint32_t pending_removals;

	/* trims: if a block with seqno < trims[i].seqno contains a reference
	 * to mesoblock >= trims[i].size, that reference is dead, it was
	 * discarded by shrinking (this prevents resurrection of old data
	 * when the partition is enlarged again); the list is kept in the
	 * index and pruned when no block of the partition is that old */
	uint32_t no_trims;
	struct blockio_trim_s {
		uint64_t seqno;
		uint32_t size;
	} trims[BLOCKIO_MAX_TRIMS];

	/* a block with a seqno below trim_floor is never replayed (a trim
	 * that was pruned could apply to it) */
	uint64_t trim_floor;

	blockio_info_t *bi; /* current block */
	blockio_info_t *tail_macroblock; /* tbe block that must be cleaned out */

	juggler_t j;

	/* here we build the macroblock
	 * to be written out to disk */
	char *tmp_macroblock;

	/* the ciphertext of the macroblock is built here, so that
	 * tmp_macroblock keeps the plaintext; ram_block is the block
	 * whose plaintext is in tmp_macroblock (if any) */
	char *out_macroblock;
	blockio_info_t *ram_block;


	// use one random_t per dev, to avoid locking issues
	random_t r;

	/* stats */

	uint32_t writes; // no macroblocks

	void *io;
} blockio_dev_t;

struct blockio_s {
	uint32_t macroblock_size;
	uint8_t macroblock_log;
	uint32_t total_macroblocks; /* amount of raw macroblocks */
	uint32_t max_macroblocks;
	uint32_t bitmap_offset;

	/* the mutex protects the unallocated list
	 * and the associated *bi->dev pointer in each
	 * blockio_info_t, which is * NULL if and only if
	 * the block is in unallocated */
	pthread_mutex_t unallocated_mutex;
	dllarr_t unallocated;

	uint8_t mesoblk_log;
	uint16_t mmpm; /* max mesoblocks per macroblock */

	blockio_info_t *blockio_infos;

	void *(*open)(const void*);
	void *open_priv; /* filename, required for open */
	void (*read)(void*, void*, uint64_t, uint32_t);
	void (*write)(void*, const void*, uint64_t, uint32_t);
	void (*close)(void*);
	void (*sync)(void*);
};

uint32_t blockio_get_macroblock_index(blockio_info_t*);

void blockio_init_file(blockio_t*, const char*, uint8_t, uint8_t);

void blockio_dev_init(blockio_dev_t*, blockio_t*, cipher_t*,
		const char*);

void blockio_dev_free(blockio_dev_t*);

void blockio_dev_scan_header(dllarr_t*, blockio_dev_t*,
		blockio_info_t*, uint64_t*);

blockio_info_t *blockio_dev_get_new_macroblock(blockio_dev_t*);

void blockio_dev_read_mesoblk(blockio_dev_t*, void*, uint32_t, uint32_t);

void blockio_dev_read_mesoblk_part(blockio_dev_t*, void*, uint32_t,
		uint32_t, uint32_t, uint32_t);

int blockio_check_data_hash(blockio_info_t*);

void blockio_dev_write_current_macroblock(blockio_dev_t*);

void blockio_free(blockio_t*);

void blockio_dev_select_next_macroblock(blockio_dev_t*);

void blockio_dev_write_current_and_select_next_macroblock(
		blockio_dev_t*);

typedef enum blockio_dev_macroblock_status_e {
		FREE, USED
} blockio_dev_macroblock_status_t;

void blockio_dev_change_macroblock_status(blockio_info_t*,
		blockio_dev_macroblock_status_t);

blockio_dev_macroblock_status_t blockio_dev_get_macroblock_status_bynum(
		blockio_dev_t*, uint32_t);

blockio_dev_macroblock_status_t blockio_dev_get_macroblock_status(
		blockio_info_t*);

// can return error -1: not enough blocks available
int blockio_dev_allocate_macroblocks(blockio_dev_t*, uint32_t);

int blockio_dev_free_macroblocks(blockio_dev_t*, uint32_t);

void blockio_prepare_block(blockio_info_t*);

uint32_t blockio_dev_usable_macroblocks(blockio_dev_t*);

blockio_info_t *blockio_dev_find_victim(blockio_dev_t*);

void blockio_dev_remove_block(blockio_dev_t*, blockio_info_t*);

int blockio_dev_add_trim(blockio_dev_t*, uint64_t, uint32_t);

int blockio_dev_is_trimmed(blockio_dev_t*, uint64_t, uint32_t);

#endif /* INCLUDE_SCUBED3_BLOCKIO_H */
