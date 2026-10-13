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

/* each mesoblock is divided in 4 parts (4KiB for 16KiB mesoblocks), the
 * low 4 bits of the hash of a mesoblock say which parts are corrupt (bit
 * i is part i), the other 60 bits are the truncated SHA256 hash */
/* special values in the index of a block: a slot that contains a list of
 * tombstones (discarded logical mesoblocks, see README), and a slot whose
 * contents are dead (its mesoblock was discarded while it was in RAM) */
#define BLOCKIO_TOMBSTONES 0xFFFFFFFE
#define BLOCKIO_DEAD 0xFFFFFFFF

#define BLOCKIO_PARTS 4
#define BLOCKIO_CORRUPT_MASK ((1<<BLOCKIO_PARTS) - 1)

#include "juggler.h"


typedef struct blockio_s blockio_t;

struct blockio_info_s {
	uint32_t pool_index; // position in the pool of the juggler
	uint64_t added, removed; // draws of the juggler (see juggler.h)

	/* for unallocated, replay and release */
	dllarr_elt_t ur;
	uint64_t seqno;
	uint32_t next_block; /* from the index: the block written after it */
	int unreadable; /* the index could not be read when scanning */

	uint32_t no_indices;
	uint32_t no_nonobsolete;
	uint32_t *indices;

	/* the number of indices in the index of this block on disk (indices
	 * still contains them, also if no_indices was reset to 0, until the
	 * block becomes the current block and is filled again) */
	uint32_t no_ondisk;

	/* this block was added to the partition (grow) while it still had
	 * an index of ours on disk, that will be replayed if the block is
	 * not written before the partition is opened again */
	int readded;

	/* truncated SHA256 of the ciphertext of each mesoblock (all mmpm of
	 * them, also the unused ones), the low bits mark the parts of the
	 * mesoblock that are known to be corrupt (see BLOCKIO_PARTS); in
	 * the current block only the low bits are meaningful, the hashes
	 * are computed when the block is written */
	uint64_t *hashes;

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

	/* the on disk indices of the current block, before it became the
	 * current block (they are replaced on disk when it is written) */
	uint32_t *old_ondisk;
	uint32_t no_old_ondisk;

	/* refcount[L] is the number of blocks on disk (that would be
	 * replayed) with logical mesoblock L in their index, for
	 * L < no_refcount (the size of the partition in mesoblocks); a
	 * discarded mesoblock needs a tombstone while its refcount > 0 */
	uint32_t *refcount;
	uint32_t no_refcount;

	/* tombstones that will be written in the current block, they take
	 * slots after the data slots (tombs_per_mesoblk per slot) */
	uint32_t *tombs;
	uint32_t no_tombs, size_tombs;
	uint32_t tombs_per_mesoblk;

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

	/* the current block is empty and it is also the block that is
	 * written after it (its own tail): it must be written out empty
	 * before data goes in it (see blockio_dev_select_next_macroblock) */
	int self_tail;

	/* paranoia level 2: the current block is the last block of the
	 * partition, it is removed by its final write, when it is its turn */
	int final_pending;

	/* paranoia level 2: this partition may write one block now */
	int turn;

	void *io;
} blockio_dev_t;

struct blockio_s {
	uint32_t macroblock_size;
	uint8_t macroblock_log;
	uint32_t total_macroblocks; /* amount of raw macroblocks */
	uint32_t max_macroblocks;
	uint32_t hashes_offset;
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
	int (*read)(void*, void*, uint64_t, uint32_t); /* -1 on error */
	void (*write)(void*, const void*, uint64_t, uint32_t);
	void (*close)(void*);
	void (*sync)(void*);

	/* paranoia level (0 to 3, see README), the open partitions (devs),
	 * and step(dev): let partition dev write one block (it gets the
	 * turn), it is called when the scheduler of level 2 draws dev; the
	 * level is changed with blockio_set_paranoia */
	int paranoia;
	blockio_dev_t **devs;
	uint32_t no_devs;
	void (*step)(blockio_dev_t*);
	random_t r;

	/* in paranoia level 2 the writes of all partitions are done one by
	 * one (one partition may write blocks of another one): this lock is
	 * taken before the lock of a partition by everything that may write */
	pthread_mutex_t io_mutex;

	/* statistics: blocks of the free space written with random data */
	uint64_t random_writes;

	/* paranoia level 3: a block is written at random moments, on average
	 * rate bytes per second (see blockio_wait_tick); the clock thread
	 * does the writes while no partition waits for its turn */
	uint64_t rate;
	struct timespec next_tick;
	pthread_mutex_t tick_mutex;
	pthread_cond_t tick_cond;
	pthread_t clock_thread;
	void *clock_io;
	int clock_running, clock_stop;
	int tick_waiting; /* the clock thread waits for io_mutex */
	uint64_t ticks; /* statistics */
};

/* the default rate of paranoia level 3: 800 KiB/s, a 4 MiB block every 5.12
 * seconds on average (see README) */
#define BLOCKIO_DEFAULT_RATE (800<<10)

void blockio_set_paranoia(blockio_t*, int, uint64_t);

void blockio_io_lock(blockio_t*);

void blockio_io_unlock(blockio_t*);

void blockio_clock_start(blockio_t*);

void blockio_clock_stop(blockio_t*);

uint32_t blockio_get_macroblock_index(blockio_info_t*);

void blockio_init_file(blockio_t*, const char*, uint8_t, uint8_t);

void blockio_dev_init(blockio_dev_t*, blockio_t*, cipher_t*,
		const char*);

void blockio_dev_free(blockio_dev_t*);

void blockio_dev_scan_header(dllarr_t*, blockio_dev_t*,
		blockio_info_t*, uint64_t*);

blockio_info_t *blockio_dev_get_new_macroblock(blockio_dev_t*);

// return the mask of corrupt parts of the mesoblock (if the hash does not
// match, all parts are corrupt), the decrypted data is in buf anyway
int blockio_dev_read_mesoblk(blockio_dev_t*, void*, uint32_t, uint32_t);

int blockio_dev_read_mesoblk_part(blockio_dev_t*, void*, uint32_t,
		uint32_t, uint32_t, uint32_t);

// return the number of mesoblocks in the block that do not match their
// hash; if live is not NULL, only the mesoblocks k with live[k] != 0 are
// checked and live[k] is set to 2 + the mask of corrupt parts if the
// mesoblock has corrupt parts
uint32_t blockio_check_data_hashes(blockio_info_t*, char *live);

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

uint32_t blockio_dev_free_slots(blockio_dev_t*);

uint32_t blockio_dev_weight(blockio_dev_t*);

int blockio_dev_tomb_fits(blockio_dev_t*, uint32_t);

void blockio_dev_add_tomb(blockio_dev_t*, uint32_t);

void blockio_dev_resize_refcount(blockio_dev_t*, uint32_t);

void blockio_dev_count_ondisk(blockio_dev_t*, blockio_info_t*, int);

uint32_t blockio_dev_read_tombs(blockio_dev_t*, blockio_info_t*, uint32_t,
		uint32_t*);

blockio_info_t *blockio_dev_find_victim(blockio_dev_t*);

void blockio_dev_remove_block(blockio_dev_t*, blockio_info_t*);

int blockio_dev_add_trim(blockio_dev_t*, uint64_t, uint32_t);

int blockio_dev_is_trimmed(blockio_dev_t*, uint64_t, uint32_t);

#endif /* INCLUDE_SCUBED3_BLOCKIO_H */
