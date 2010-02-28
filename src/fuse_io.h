/* fuse_io.h - interaction with fuse
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
#ifndef INCLUDE_SCUBED3_FUSE_IO_H
#define INCLUDE_SCUBED3_FUSE_IO_H 1

#include <fcntl.h>
#include "scubed3.h"
#include "hashtbl.h"
#include "cipher.h"
#include "blockio.h"

typedef struct fuse_io_entry_s {
        hashtbl_elt_t head;

        uint64_t size;
        int inuse;
	int to_be_deleted;
	cipher_t c;
	blockio_dev_t d;
        scubed3_t l;
	hashtbl_t *ids;
	struct unique_id {
		hashtbl_elt_t head;
		char id[32];
		char *name;
	} unique_id;
} fuse_io_entry_t;

typedef struct fuse_io_id_s {
        hashtbl_elt_t head;
	char *name;
} fuse_io_id_t;

int fuse_io_start(int, char**, blockio_t*);

#endif /* INCLUDE_SCUBED3_FUSE_IO_H */
