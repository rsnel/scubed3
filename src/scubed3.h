/* scubed3.h - deniable encryption resistant to surface analysis
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
#ifndef INCLUDE_SCUBED3_H
#define INCLUDE_SCUBED3_H 1

#include <stdint.h>
#include "dllist.h"

typedef enum scubed3_io_e {
	SCUBED3_READ,
	SCUBED3_WRITE
} scubed3_io_t;

typedef struct scubed3_s {
	/* must be set from init */
	struct blockio_dev_s *dev;

	/* some constants for accessor functions */
	uint16_t mesobits;
	uint32_t mesomask;

	/* the real device is split into an array of mesoblocks, where
	 * can we find mesoblock 16? -> block_indices[16] is an uint32_t,
	 * the high bits encode the macroblock number and the low
	 * bits encode the index of the mesoblock in the specified
	 * macroblock, see the definitions of ID and NO below */
	uint32_t no_block_indices;
	uint32_t *block_indices;
} scubed3_t;

int do_req(scubed3_t*, scubed3_io_t, uint64_t, size_t, char*);

struct blockio_dev_s;

void scubed3_init(scubed3_t*, struct blockio_dev_s*);

void scubed3_reinit(scubed3_t*);

void scubed3_free(scubed3_t*);

#define id(a)   ((a) - l->dev->b->blockio_infos)

#endif /* INCLUDE_SCUBED3_H */
