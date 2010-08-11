/* dllarr.h - doubly linked (list and) array
 *
 * Copyright (C) 2010  Rik Snel <rik@snel.it>
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
#ifndef INCLUDE_SCUBED3_DLLARR_H
#define INCLUDE_SCUBED3_DLLARR_H 1

#include <stdint.h>

#define DLLARR_MAGIC 0x37B82A6074110287ULL

struct dllarr_s;

typedef struct dllarr_elt_s {
	uint64_t magic;
	struct dllarr_elt_s *prev, *next;
	int index;
} dllarr_elt_t;

typedef struct dllarr_s {
	dllarr_elt_t head; 
	dllarr_elt_t tail; 
	dllarr_elt_t **array;
	int size, offset;
} dllarr_t;

void dllarr_init(dllarr_t*, int);

void dllarr_free(dllarr_t*);

/* double linked list access interface */

void *dllarr_first(dllarr_t*);

void *dllarr_next(dllarr_t*, void*);

void *dllarr_last(dllarr_t*);

void *dllarr_prev(dllarr_t*, void*);

/* iterator head->tail */

typedef void *(*dllarr_iterator_t)(void*, void*);

void *dllarr_iterate(dllarr_t*, dllarr_iterator_t, void*);

/* double linked list management interface */

void *dllarr_insert(dllarr_t*, void*, void*);

void *dllarr_append(dllarr_t*, void*, void*);

void *dllarr_remove(dllarr_t*, void*);

/* array interface */

void *dllarr_nth(dllarr_t*, int);

int dllarr_index(dllarr_t*, void*);

int dllarr_count(dllarr_t*);

#endif /* INCLUDE_SCUBED3_DLLARR_H */
