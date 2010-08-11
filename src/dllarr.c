/* dllarr.c - doubly linked (list and) array
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
#include <stdio.h>
#include <assert.h>
#include <errno.h>
#include <string.h>

#include "verbose.h"
#include "dllarr.h"

#define DLLARR_START_SIZE	32

static void dllarr_assert(dllarr_t *a) {
	assert(a && !a->head.prev && !a->tail.next);
	if (a->head.index) {
		assert(a->head.index == -1);
		assert(a->size > 0);
		assert(a->size >= a->tail.index);
		if (!a->tail.index) assert(a->head.next == &a->tail &&
				a->tail.prev == &a->head);
		else assert(a->head.next && a->tail.prev);
	}
}

void dllarr_init(dllarr_t *a) {
	assert(a);

	assert(!a->head.prev && !a->tail.next);
	a->head = {
		.next = &a->tail;
		.magic = DLLARR_MAGIC,
		.index = -1
	};
	a->tail = {
		.prev = &a->head;
		.magic = DLLARR_MAGIC,
		.index = 0 
	};

	a->size = DLLARR_START_SIZE;
	a->array = calloc(DLLARR_START_SIZE, sizeof(*a->array));
	if (!a->array) FATAL("calloc(%d, %d): %s", DLLARR_START_SIZE,
			sizeof(*a->array), strerror(errno));
}

void dllarr_free(dllarr_t *a) {
	dllarr_elt_t *elt;
	dllarr_assert(a);

	free(a->array);

	// remove backwards to avoid array copying 
	elt = a->tail.prev;

	while (elt != &a->head) {
		//FIXME implement
		//dllarr_remove(a, elt);
		elt = elt->prev;
	}
}

#if 0
void *dllarr_first(dllarr_t *d) {
	assert(d && !d->head.prev && !d->tail.next &&
			d->head.next && d->tail.prev);

	if (d->head.next != &d->tail) return d->head.next;

	return NULL;
}

void *dllarr_get_last(dllarr_t *d) {
	assert(d && !d->head.prev && !d->tail.next &&
			d->head.next && d->tail.prev);

	if (d->tail.prev != &d->head) return d->tail.prev;

	return NULL;
}

void dllarr_insert_at(dllarr_elt_t *here, dllarr_elt_t *e) {
	assert(e && !e->next && !e->prev);
	assert(here);
	assert(here->prev);
	assert(here->prev->next);
	here->prev->next = e;
	e->prev = here->prev;
	e->next = here;
	here->prev = e;
	e->no_elts = here->no_elts;
	*e->no_elts += 1;

}

void dllarr_remove(dllarr_elt_t *e) {
	assert(e);
	assert(e->next);
	assert(e->prev);
	e->next->prev = e->prev;
	e->prev->next = e->next;
	*e->no_elts -= 1;
	e->next = e->prev = NULL;
	e->no_elts = NULL;
}

void *dllarr_iterate(dllarr_t *d,
		int (*func)(dllarr_elt_t*, void*), void *arg) {
	dllarr_elt_t *cur, *next;
	assert(d && !d->head.prev && !d->tail.next &&
			d->head.next && d->tail.prev && func);

	cur = d->head.next;

	while ((next = cur->next)) {
		if (!func(cur, arg)) return cur;
		cur = next;
	}

	return NULL;
}

int dllarr_count(dllarr_t *d) {
	return d->no_elts;
}
#endif
