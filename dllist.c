/* license GPLv3 or any later */
#include <assert.h>
#include <stdio.h>

#include "verbose.h"
#include "dllist.h"

void dllist_init(dllist_t *d) {
	assert(d);

	d->head.prev = NULL;
	d->head.next = &d->tail;

	d->tail.prev = &d->head;
	d->tail.next = NULL;
}

void dllist_free(dllist_t *d) {
	assert(d && !d->head.prev && !d->tail.next &&
			d->head.next && d->tail.prev);
}

int dllist_is_empty(dllist_t *d) {
	assert(d && !d->head.prev && !d->tail.next &&
			d->head.next && d->tail.prev);
	if (d->head.next == &d->tail && d->tail.prev == &d->head)
		return 1;
	return 0;
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
	assert(e && e->next && e->prev);
	e->next->prev = e->prev;
	e->prev->next = e->next;
	e->next = e->prev = NULL;
}

void *dllist_iterate(dllist_t *d,
		int (*func)(dllist_elt_t*, void*), void *arg) {
	dllist_elt_t *cur;
	assert(d && !d->head.prev && !d->tail.next &&
			d->head.next && d->tail.prev && func);

	cur = d->head.next;

	while (cur->next) {
		if (!func(cur, arg)) return cur;
		cur = cur->next;
	}

	return NULL;
}

