/* dllist.h - doubly linked lists
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
#ifndef INCLUDE_SCUBED3_DLLIST_H
#define INCLUDE_SCUBED3_DLLIST_H 1

typedef struct dllist_elt_s {
	struct dllist_elt_s *prev;
	struct dllist_elt_s *next;
} dllist_elt_t;

typedef struct dllist_s {
	dllist_elt_t head; /* oldest */
	dllist_elt_t tail; /* newest */
} dllist_t;

void dllist_init(dllist_t*);

void dllist_free(dllist_t*);

void dllist_insert_before(dllist_elt_t*, dllist_elt_t*);

void dllist_remove(dllist_elt_t*);

void dllist_append(dllist_t*, dllist_elt_t*);

void dllist_prepend(dllist_t*, dllist_elt_t*);

void *dllist_get_tail(dllist_t*);

void *dllist_get_head(dllist_t*);

void *dllist_iterate(dllist_t*, int (*)(dllist_elt_t*, void*), void*);

void *dllist_iterate_backwards(dllist_t*, int (*)(dllist_elt_t*, void*), void*);

int dllist_is_empty(dllist_t*);

#endif /* INCLUDE_SCUBED3_DLLIST_H */
