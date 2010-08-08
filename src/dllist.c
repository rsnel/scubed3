/* dllist.c - doubly linked lists
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
#include <stdio.h>

#include "assert.h"
#include "verbose.h"
#include "dllist.h"

void dllist_init(dllist_t *d) {
	assert(d);

	d->head.prev = NULL;
	d->head.next = &d->tail;
	d->head.no_elts = &d->no_elts;

	d->tail.prev = &d->head;
	d->tail.next = NULL;
	d->tail.no_elts = &d->no_elts;

	d->no_elts = 0;

}

void dllist_free(dllist_t *d) {
	assert(d && !d->head.prev && !d->tail.next &&
			d->head.next && d->tail.prev);
}

int dllist_is_empty(dllist_t *d) {
	assert(d && !d->head.prev && !d->tail.next &&
			d->head.next && d->tail.prev);
	if (d->head.next == &d->tail && d->tail.prev == &d->head && d->no_elts == 0)
		return 1;
	if (d->head.next != &d->tail && d->tail.prev != &d->head && d->no_elts != 0)
		return 0;

	assert(0); // impossible
	return 0; // shut up compiler
}

void *dllist_get_head(dllist_t *d) {
	assert(d && !d->head.prev && !d->tail.next &&
			d->head.next && d->tail.prev);

	if (d->head.next != &d->tail) return d->head.next;

	return NULL;
}

void *dllist_get_tail(dllist_t *d) {
	assert(d && !d->head.prev && !d->tail.next &&
			d->head.next && d->tail.prev);

	if (d->tail.prev != &d->head) return d->tail.prev;

	return NULL;
}

void dllist_insert_before(dllist_elt_t *here, dllist_elt_t *e) {
	assert(e && !e->next && !e->prev);
	here->prev->next = e;
	e->prev = here->prev;
	e->next = here;
	here->prev = e;
	e->no_elts = here->no_elts;
	*e->no_elts += 1;

}

void dllist_append(dllist_t *d, dllist_elt_t *e) {
	//assert(d && !d->head.prev && !d->tail.next &&
	//		d->head.next && d->tail.prev);
	assert(d);
	assert(!d->head.prev);
	assert(!d->tail.next);
        assert(d->head.next);
	assert(d->tail.prev);
	dllist_insert_before(&d->tail, e);
}

void dllist_prepend(dllist_t *d, dllist_elt_t *e) {
	assert(d && !d->head.prev && !d->tail.next &&
			d->head.next && d->tail.prev);
	dllist_insert_before(d->head.next, e);
}

#if 0
void dllist_append(dllist_t *d, dllist_elt_t *e) {
	assert(d && !d->head.prev && !d->tail.next &&
			d->head.next && d->tail.prev &&
			e && !e->next && !e->prev);
	e->next = &d->tail;
	e->prev = d->tail.prev;
	e->prev->next = e;
	d->tail.prev = e;
}
#endif

void dllist_remove(dllist_elt_t *e) {
	assert(e);
	assert(e->next);
	assert(e->prev);
	e->next->prev = e->prev;
	e->prev->next = e->next;
	*e->no_elts -= 1;
	e->next = e->prev = NULL;
	e->no_elts = NULL;
}

void *dllist_iterate(dllist_t *d,
		int (*func)(dllist_elt_t*, void*), void *arg) {
	dllist_elt_t *cur, *next;
	assert(d && !d->head.prev && !d->tail.next &&
			d->head.next && d->tail.prev && func);

	cur = d->head.next;

	while ((next = cur->next)) {
		if (!func(cur, arg)) return cur;
		cur = next;
	}

	return NULL;
}

uint32_t dllist_get_no_elts(dllist_t *d) {
	return d->no_elts;
}

void dllist_move_append(dllist_t *dest, dllist_t *src) {
	dllist_elt_t *oldend, *first, *last;

	if (dllist_is_empty(src)) return;

	oldend = dest->tail.prev;
	first = src->head.next;
	last = src->tail.prev;

	/* move elements to dest list */
	oldend->next = first;
	first->prev = oldend;
	last->next = &dest->tail;
	dest->tail.prev = last;
	dest->no_elts += src->no_elts;

	/* empty the source list */
	src->head.next = &src->tail;
	src->tail.prev = &src->head;
	src->no_elts = 0;


	
}
