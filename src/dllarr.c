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
		assert(a->array);
		assert(a->offset >= 0);
		assert(a->size > 0);
		assert(a->size >= a->tail.index);
		if (!a->tail.index) assert(a->head.next == &a->tail &&
				a->tail.prev == &a->head);
		else assert(a->head.next && a->tail.prev);
	}
}

static void *access_ptr(dllarr_t *a, dllarr_elt_t *elt) {
	assert(a && elt);
	if (elt == &a->head || elt == &a->tail) return NULL;
	return (void*)elt - a->offset;
}

static dllarr_elt_t *access_elt(dllarr_t *a, void *at) {
	dllarr_elt_t *at_elt;
	assert(a && at);
	at_elt = at + a->offset;
	assert(at_elt->magic == DLLARR_MAGIC);
	assert(at_elt != &a->head || at_elt != &a->tail);
	return at + a->offset;
}

void dllarr_init(dllarr_t *a, int offset) {
	assert(a && offset >= 0);

	assert(!a->head.prev && !a->tail.next);

	a->head.next = &a->tail;
	a->head.magic = DLLARR_MAGIC;
	a->head.index = -1;

	a->tail.prev = &a->head;
	a->tail.magic = DLLARR_MAGIC;
	a->tail.index = 0;

	a->size = DLLARR_START_SIZE;
	a->array = calloc(DLLARR_START_SIZE, sizeof(*a->array));
	if (!a->array) FATAL("calloc(%d, %d): %s", DLLARR_START_SIZE,
			sizeof(*a->array), strerror(errno));

	a->offset = offset;

	dllarr_assert(a);
}

void dllarr_free(dllarr_t *a) {
	dllarr_elt_t *elt, *del;
	dllarr_assert(a);

	// remove backwards to avoid array copying 
	elt = a->tail.prev;

	while ((del = elt) != &a->head) {
		elt = del->prev;
		dllarr_remove(a, access_ptr(a, del));
	}

	free(a->array);
}

void *dllarr_first(dllarr_t *a) {
	dllarr_assert(a);
	return access_ptr(a, a->head.next);
}

void *dllarr_next(dllarr_t *a, void *at) {
	dllarr_assert(a);
	assert(at);
	dllarr_elt_t *at_elt = access_elt(a, at);
	return access_ptr(a, at_elt->next);
}

void *dllarr_last(dllarr_t *a) {
	dllarr_assert(a);
	return access_ptr(a, a->tail.prev);
}

void *dllarr_prev(dllarr_t *a, void *at) {
	dllarr_assert(a);
	assert(at);
	dllarr_elt_t *at_elt = access_elt(a, at);
	return access_ptr(a, at_elt->prev);
}

void *dllarr_iterate(dllarr_t *a, dllarr_iterator_t func, void *priv) {
	dllarr_assert(a);
	assert(func);
	dllarr_elt_t *at_elt = a->head.next;
	void *ret;

	while (at_elt != &a->tail) {
		if ((ret = func(access_ptr(a, at_elt), priv))) return ret;
		at_elt = at_elt->next;
	}

	return NULL;
}


void *dllarr_insert(dllarr_t *a, void *new, void *at) {
	dllarr_assert(a);
	assert(new);
	dllarr_elt_t *new_elt = (void*)(new + a->offset);
	dllarr_elt_t *at_elt = at?access_elt(a, at):NULL;
	assert(!new_elt->prev && !new_elt->next && !new_elt->index);

	if (a->tail.index == a->size) {
		a->size *= 2;
		if (a->size*sizeof(*a->array) < 0) FATAL("dllarr too full");
		if (!(a->array = realloc(a->array, sizeof(*a->array)*a->size)))
			FATAL("realloc to %d bytes: %s",
					sizeof(*a->array)*a->size,
					strerror(errno));
	}

	/* insert at NULL means append (insert at tail) */
	if (!at) at_elt = &a->tail;
	else {
		memmove(&a->array[at_elt->index + 1],
			&a->array[at_elt->index],
			(a->tail.index - at_elt->index)*sizeof(*a->array));
		assert(at_elt->next);
		assert(at_elt->next->prev == at_elt);
	}

	assert(at_elt->prev);
	assert(at_elt->prev->next == at_elt);

	new_elt->index = at_elt->index;
	a->array[new_elt->index] = new_elt;

	at_elt->prev->next = new_elt;
	new_elt->prev = at_elt->prev;
	new_elt->next = at_elt;
	at_elt->prev = new_elt;

	// all elements (including tail), must have their index updated
	while (at_elt) {
		at_elt->index++;
		at_elt = at_elt->next;
	}

	new_elt->magic = DLLARR_MAGIC;

	return new;
}

void *dllarr_remove(dllarr_t *a, void *at) {
	dllarr_assert(a);
	assert(at);
	dllarr_elt_t *at_elt = access_elt(a, at);
	assert(at_elt->prev);
	assert(at_elt->prev->next == at_elt);
	assert(at_elt->next->prev == at_elt);

	at_elt->next->prev = at_elt->prev;
	at_elt->prev->next = at_elt->next;
	at_elt->next = at_elt->prev = NULL;

	if (at_elt->next != &a->tail)
		memmove(&a->array[at_elt->index],
			&a->array[at_elt->index + 1],
			(a->tail.index - at_elt->index - 1)*sizeof(*a->array));
		
	at_elt->index = 0;

	while ((at_elt = at_elt->next)) at_elt->index--;

	return at;
}

void *dllarr_nth(dllarr_t *a, int n) {
	dllarr_assert(a);
	assert(n >= 0 && n < a->tail.index);

	return access_ptr(a, a->array[n]);
}

int dllarr_count(dllarr_t *a) {
	dllarr_assert(a);
	return a->tail.index;
}
