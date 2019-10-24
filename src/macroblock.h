/* macroblock.h - macroblock stuff
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
#ifndef INCLUDE_SCUBED3_MACROBLOCK_H
#define INCLUDE_SCUBED3_MACROBLOCK_H 1

#include <stdint.h>

/* MACROBLOCK_STATE_EMPTY_E
 *
 *      a block belongs to an open device, but the block
 *      on disk does not indicate that it does so, because
 *      it is actually been never used by the device or it
 *      is marked as discarded by our device after the device
 *      is shrunk the way we know it belongs to us is that
 *      the newest block of our device (highest seqno)
 *      contains a list of all blocks allocated to it
 *
 * MACROBLOCK_STATE_FILLER_E
 *
 *      a block that is randomly selected again in the next round,
 *      such a block can't be used to store data
 *
 * MACROBLCK_STATE_OBSOLETED_E
 *
 *      a block that will be rewritten in the next round (but is not
 *      selected this round, otherwise it would be STATE_FILLER),
 *      the data in this block must be moved to another block
 *      in this round (fortunately this is always possible)
 *
 * MACROBLOCK_STATE_OK_E
 *
 *      a block that is not empty and will not be overwritten in the
 *      next round
 */
typedef enum macroblock_state_e {
	MACROBLOCK_STATE_EMPTY_E,
	MACROBLOCK_STATE_FILLER_E,
	MACROBLOCK_STATE_OBSOLETED_E,
	MACROBLOCK_STATE_OK_E
} macroblock_state_t;

typedef struct macroblock_s {
	struct macroblock_s *next;

	macroblock_state_t state;
	uint64_t next_seqno;

	uint64_t lifespan2;	// FIXME remove
	uint64_t lifespan;	// FIXME remove
	int id;			// FIXME remove
} macroblock_t;

#endif /* INCLUDE_SCUBED3_MACROBLOCK_H */
