/* license GPLv3 or any later */
#ifndef INCLUDE_LSBD_DLLIST_H
#define INCLUDE_LSBD_DLLIST_H 1

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

int dllist_is_empty(dllist_t*);

#endif /* INCLUDE_LSBD_DLLIST_H */
