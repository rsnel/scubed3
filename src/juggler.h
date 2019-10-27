/* juggler.h - block provider
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
#ifndef INCLUDE_SCUBED3_JUGGLER_H
#define INCLUDE_SCUBED3_JUGGLER_H 1

#include <stdint.h>
#include "blockio.h"
#include "random.h"
#include "dllarr.h"

/* the juggler keeps only information
 * that can be inferred by looking at the
 * contents of the disk, so that the juggler
 * can be easily be restarted without loss of
 * state (for example: between runs of scubed3 */
typedef struct juggler_s {
	blockio_info_t *scheduled, *unscheduled;
	uint32_t no_scheduled, no_unscheduled;
	uint64_t seqno;
	random_t *r;
} juggler_t;

uint32_t juggler_count(juggler_t*j);

void juggler_init(juggler_t*, random_t *r);

void juggler_add_macroblock(juggler_t*, blockio_info_t*);

void juggler_notify_seqno(juggler_t*, uint64_t seqno);

blockio_info_t *juggler_get_obsoleted(juggler_t*);

blockio_info_t *juggler_get_devblock(juggler_t*, int);

void juggler_verbose(juggler_t*, uint32_t (*getnum)(blockio_info_t*, void*), void*);

char *juggler_hash_scheduled_seqnos(juggler_t*, char*);

void juggler_free_and_empty_into(juggler_t*, void *(*append)(dllarr_t*, void*), dllarr_t*);

void juggler_free(juggler_t*);

#endif /* INCLUDE_SCUBED3_JUGGLER_H */
