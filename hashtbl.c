/* hashtbl.c - hashtable implementation
 *
 * Copyright (C) 2006 Rik Snel <rsnel@cube.dyndns.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */
#include <assert.h>
#include <stdint.h>
#include "verbose.h"
#include "util.h"
#include "pthd.h"
#include "gcry.h"
#include "hashtbl.h"

#define NO_BUCKETS (1<<h->key_bits)
#define TS (h->thread_safe)
#define UNIQ (h->unique)

typedef int (*compare_t)(hashtbl_t*, const void*, hashtbl_elt_t*);

#include <unistd.h>
static void hashtbl_delete_element(hashtbl_t *h,
		hashtbl_elt_t **eltp, pthread_mutex_t **ptr_mutex) {
	hashtbl_elt_t *elt;

	elt = *eltp;
	//DEBUG("attempt to lock next pointer of %p", elt);
	if (TS) pthd_mutex_lock(&elt->ptr_mutex);
	*eltp = (*eltp)->next;
	if (TS) {
		//DEBUG("unlock ptr");
		pthd_mutex_unlock(&elt->ptr_mutex);
		//DEBUG("unlock data");
		pthd_mutex_unlock(&elt->data_mutex);
		//DEBUG("destroy ptr");
		pthd_mutex_destroy(&elt->ptr_mutex);
		//DEBUG("destroy data");
		pthd_mutex_destroy(&elt->data_mutex);
		//DEBUG("mkay");
		if (ptr_mutex) pthd_mutex_unlock(*ptr_mutex);
	}

	if (h->freer) (*h->freer)(elt);
}

void hashtbl_empty(hashtbl_t *h) {
	int i;
	assert(h && h->key_bits >= 0);
	if (!h->buckets) return;

	if (TS) pthd_mutex_lock(&h->count_mutex);
	h->count = 0;
	for (i = 0; i < NO_BUCKETS; i++) {
		if (TS) pthd_mutex_lock(&h->buckets[i].ptr_mutex);
		while (h->buckets[i].first) {
			if (TS) pthd_mutex_lock(
					&h->buckets[i].first->data_mutex);
			hashtbl_delete_element(h, &h->buckets[i].first, NULL);
		}
		if (TS) pthd_mutex_unlock(&h->buckets[i].ptr_mutex);
	}
	if (TS) pthd_mutex_unlock(&h->count_mutex);
}

void hashtbl_free(hashtbl_t *h) {
	int i;
	hashtbl_empty(h);
	if (TS) {
		pthd_mutex_destroy(&h->count_mutex);
		for (i = 0; i < NO_BUCKETS; i++) {
			pthd_mutex_destroy(&h->buckets[i].ptr_mutex);
			if (UNIQ) pthd_mutex_destroy(&h->buckets[i].uniq_mutex);
		}
	}
	free(h->buckets);
}

void hashtbl_init_default(hashtbl_t *h, int key_bits, int key_size,
		int thread_safe, int unique, void (*freer)(void*)) {
	int i;
	assert(h && key_bits >= 0 && key_bits <= 16);
	assert(thread_safe == 0 || thread_safe == 1);
	assert(unique == 0 || unique == 1);
	if (key_size >= 0) assert(8*key_size >= key_bits && key_size < 128);

	h->key_bits = key_bits;
	h->buckets = ecalloc(sizeof(hashtbl_bucket_t), NO_BUCKETS);

	h->thread_safe = thread_safe;
	h->unique = unique;
	if (TS) {
		pthd_mutex_init(&h->count_mutex);
		for (i = 0; i < NO_BUCKETS; i++) {
			pthd_mutex_init(&h->buckets[i].ptr_mutex);
			if (UNIQ) pthd_mutex_init(&h->buckets[i].uniq_mutex);
		}
	}

	h->freer = freer;
	h->key_size = key_size;
	h->count = 0;
}

/* function used for hashtables with strings as key */
static uint32_t hash_sdbm(const unsigned char *str) {
	uint32_t hash = 0;
	int c;

	while ((c = *str++)) hash = c + (hash<<6) + (hash<<16) - hash;

	return hash;
}

/* comparison functions: key, pointer and 'always true' */
static int compare_key(hashtbl_t *h, const void *key, hashtbl_elt_t *elt) {
	assert(h->key_size != 0);
	if (h->key_size < 0 && !strcmp(key, *(char**)elt->data)) return 1;
	if (!memcmp(key, &elt->data, h->key_size)) return 1;
	return 0;
}

static int compare_key_ge(hashtbl_t *h, const void *key, hashtbl_elt_t *elt) {
	assert(h->key_size > 0);
	const unsigned char *key_ref = key;
	const unsigned char *key_elt = (unsigned char*)&elt->data;
	int i = 0;

	while (i < h->key_size) {
		if (key_ref[i] < key_elt[i]) return 1;
		if (key_ref[i] > key_elt[i]) return 0;
		i++;
	}
	/* equal */
	return 1;
}

static int compare_ptr(hashtbl_t *h, const void *ptr, hashtbl_elt_t *elt) {
	if (ptr == elt) return 1;
	return 0;
}

static int compare_true(hashtbl_t *h, const void *ptr, hashtbl_elt_t *elt) {
	return 1;
}

/* calculate bucket number belonging to key */
static uint32_t get_bucket(hashtbl_t *h, const void *key) {
	uint32_t bucket;
	if (h->key_size == 0) return 0;
	if (h->key_size < 0) bucket = hash_sdbm(key);
	else memcpy(&bucket, key, (h->key_size>4)?4:h->key_size);
	bucket &= 0xffffffff>>(32-h->key_bits);
	//VERBOSE("using bucket %d", bucket);

	return bucket;
}

static hashtbl_elt_t **hashtbl_find_elementp_starting_at(hashtbl_t *h,
		const void *target, pthread_mutex_t **ptr_mutex,
		hashtbl_elt_t **next, compare_t compare) {
	assert(!TS || ptr_mutex);
	assert(next);
	while (*next) {
		if ((*compare)(h, target, *next)) {
			if (TS) pthd_mutex_lock(&(*next)->data_mutex);
			return next;
		}
		if (TS) {
			pthd_mutex_lock(&(*next)->ptr_mutex);
			pthd_mutex_unlock(*ptr_mutex);
		}
		*ptr_mutex = &(*next)->ptr_mutex;
		next = &(*next)->next;
	}

	if (TS) pthd_mutex_unlock(*ptr_mutex);
	return NULL;
}


static hashtbl_elt_t **hashtbl_find_elementp_in_bucket(hashtbl_t *h,
		const void *target, pthread_mutex_t **ptr_mutex,
		uint32_t bucket, compare_t compare) {
	assert(!TS || ptr_mutex);
	assert(bucket >= 0 && bucket < NO_BUCKETS);

	if (TS) pthd_mutex_lock(&h->buckets[bucket].ptr_mutex);
	*ptr_mutex = &h->buckets[bucket].ptr_mutex;

	return hashtbl_find_elementp_starting_at(h, target, ptr_mutex,
			&h->buckets[bucket].first, compare);
}

static hashtbl_elt_t **hashtbl_find_elementp_from_bucket(
		hashtbl_t *h, void *target, pthread_mutex_t **ptr_mutex,
		uint32_t start, compare_t compare) {
	hashtbl_elt_t **eltp = NULL;
	uint32_t bucket;
	for (bucket = start; bucket < NO_BUCKETS; bucket++) {
		//VERBOSE("search in bucket %d", bucket);
		eltp = hashtbl_find_elementp_in_bucket(h,
				target, ptr_mutex, bucket, compare);
		if (eltp) return eltp;
	}

	return NULL;
}

static void *hashtbl_find_element_starting_at(hashtbl_t *h,
		void *target, hashtbl_elt_t *start, compare_t compare) {
	hashtbl_elt_t *elt = NULL, **eltp;
	pthread_mutex_t *last;

	if (TS) {
		pthd_mutex_lock(&start->ptr_mutex);
		pthd_mutex_unlock(&start->data_mutex);
	}
	last = &start->ptr_mutex;

	eltp = hashtbl_find_elementp_starting_at(h, target,
			&last, &start->next, compare);
	if (eltp) {
		elt = *eltp;
		if (TS) pthd_mutex_unlock(last);
	}

	return elt;
}

static void *hashtbl_find_element_in_bucket(hashtbl_t *h,
		const void *target, uint32_t bucket, compare_t compare) {
	hashtbl_elt_t *elt = NULL, **eltp;
	pthread_mutex_t *last;

	eltp = hashtbl_find_elementp_in_bucket(h, target,
			&last, bucket, compare);
	if (eltp) {
		elt = *eltp;
		if (TS) pthd_mutex_unlock(last);
	}
	return elt;
}

static void *hashtbl_find_element_from_bucket(hashtbl_t *h,
		void *target, uint32_t start, compare_t compare) {
	hashtbl_elt_t *elt = NULL, **eltp;
	pthread_mutex_t *last;

	eltp = hashtbl_find_elementp_from_bucket(h,
			target, &last, start, compare);
	if (eltp) {
		elt = *eltp;
		if (TS) pthd_mutex_unlock(last);
	}
	return elt;
}

static void hashtbl_delete_element_from_bucket(hashtbl_t *h,
		void *target, uint32_t bucket, compare_t compare) {
	hashtbl_elt_t **eltp;
	pthread_mutex_t *last;

	eltp = hashtbl_find_elementp_in_bucket(h, target,
			&last, bucket, compare);

	if (TS) pthd_mutex_lock(&h->count_mutex);
	h->count--;
	assert(h->count >= 0);
	if (TS) pthd_mutex_unlock(&h->count_mutex);

	hashtbl_delete_element(h, eltp, &last);
}

static int unique_key(hashtbl_t *h, hashtbl_elt_t *elt, uint32_t bucket) {
	assert(h->key_size != 0);
	hashtbl_elt_t *elt2;
	int ret = 0;

	if (h->key_size < 0)
		elt2 = hashtbl_find_element_in_bucket(h,
				*(char**)elt->data,
				bucket, compare_key);
	else if (h->key_size > 0)
		elt2 = hashtbl_find_element_in_bucket(h, elt->data,
				bucket, compare_key);
	else assert(0); /* impossible */
	if (elt2) {
		if (TS) hashtbl_unlock_element_byptr(elt2);
	} else ret = 1;

	return ret;
}

static void hashtbl_count_unlock(hashtbl_t *h) {
	if (TS) pthd_mutex_unlock(&h->count_mutex);
}

void *hashtbl_add_element(hashtbl_t *h, void *raw) {
	uint32_t bucket;
	hashtbl_elt_t *elt = raw;
	assert(elt);

	if (h->key_size < 0) bucket = get_bucket(h, *(char**)elt->data);
	else bucket = get_bucket(h, elt->data);

	if (TS) pthd_mutex_lock(&h->count_mutex);
	pthread_cleanup_push((void (*)(void*))hashtbl_count_unlock, h);
	h->count++;
	pthread_cleanup_pop(1);
	if (TS && UNIQ) pthd_mutex_lock(&h->buckets[bucket].uniq_mutex);

	if (h->key_size && UNIQ && !unique_key(h, elt, bucket)) {
		if (TS) {
			if (UNIQ) pthd_mutex_unlock(
					&h->buckets[bucket].uniq_mutex);
			pthd_mutex_lock(&h->count_mutex);
		}
		h->count--;
		if (TS) pthd_mutex_unlock(&h->count_mutex);

		//VERBOSE("Did not add duplicate?");
		return NULL;
	}

	if (TS) {
		pthd_mutex_init(&elt->ptr_mutex);
		pthd_mutex_init(&elt->data_mutex);
		pthd_mutex_lock(&h->buckets[bucket].ptr_mutex);
	}
	elt->next = h->buckets[bucket].first;
	h->buckets[bucket].first = elt;
	if (TS) {
		pthd_mutex_lock(&elt->data_mutex);
		pthd_mutex_unlock(&h->buckets[bucket].ptr_mutex);
		if (UNIQ) pthd_mutex_unlock(&h->buckets[bucket].uniq_mutex);
	}
	return elt;
}

void *hashtbl_allocate_and_add_element(hashtbl_t *h, void *key, int size) {
	hashtbl_elt_t *elt = NULL;
	//VERBOSE("h->key_size=%d, sizeof(*elt)=%d, size=%d",
	//		h->key_size, sizeof(*elt), size);
	assert((h->key_size == 0 && size >= sizeof(*elt)) || \
			(h->key_size < 0 && \
			 size >= sizeof(*elt) + sizeof(char*)) || \
			(h->key_size > 0 && \
			 size >= sizeof(*elt) + h->key_size));
	elt = ecalloc(1, size);
	if (h->key_size < 0) *(char**)elt->data = key;
	else if (h->key_size > 0) memcpy(elt->data, key, h->key_size);

	return hashtbl_add_element(h, elt);
}

void *hashtbl_find_element_bykey(hashtbl_t *h, const void *key) {
	return hashtbl_find_element_in_bucket(h, key, get_bucket(h, key),
			compare_key);
}

void hashtbl_delete_element_bykey(hashtbl_t *h, void *key) {
	hashtbl_delete_element_from_bucket(h, key, get_bucket(h, key),
			compare_key);
}

void hashtbl_delete_element_byptr(hashtbl_t *h, void *elt) {
	hashtbl_elt_t **eltp;
	pthread_mutex_t *last;

	if (h->key_size != 0) {
		hashtbl_delete_element_bykey(h, ((hashtbl_elt_t*)elt)->data);
		return;
	}

	//DEBUG("looking for %p", elt);
	eltp = hashtbl_find_elementp_from_bucket(h, elt,
			&last, 0, compare_ptr);
	//DEBUG("found %p %p", elt, *eltp);
	if (!eltp) // element not found?
		return;

	if (TS) pthd_mutex_lock(&h->count_mutex);
	h->count--;
	assert(h->count >= 0);
	if (TS) pthd_mutex_unlock(&h->count_mutex);

	//DEBUG("now deleting");
	hashtbl_delete_element(h, eltp, &last);
}

void *hashtbl_first_element(hashtbl_t *h) {
	return hashtbl_find_element_from_bucket(h, NULL, 0, compare_true);
}

void *hashtbl_first_element_ge(hashtbl_t *h, void *key) {
	return hashtbl_find_element_from_bucket(h, key, get_bucket(h, key), compare_key_ge);
}

void hashtbl_unlock_element_byptr(void *elt) {
	assert(elt);
	pthd_mutex_unlock(&((hashtbl_elt_t*)elt)->data_mutex);
}

void hashtbl_unlock_elementp(hashtbl_elt_t** elt) {
	assert(elt);
	hashtbl_unlock_element_byptr(*elt);
}

void hashtbl_elementp_free(hashtbl_elt_t **elt) {
	assert(elt);
	if (*elt) hashtbl_unlock_element_byptr(*elt);
}

void hashtbl_elementp_init_add(void *arg, hashtbl_t *ht,
		void *key, int size) {
	hashtbl_elt_t **elt = arg;
	assert(elt);
	*elt = hashtbl_allocate_and_add_element(ht, key, size);
}

void hashtbl_elementp_init_find(void *arg, hashtbl_t *ht, void *key) {
	hashtbl_elt_t **elt = arg;
	assert(elt);
	*elt = hashtbl_find_element_bykey(ht, key);
}

void *hashtbl_next_element_byptr(hashtbl_t *h, void *prev) {
	uint32_t bucket;
	hashtbl_elt_t *elt = prev;
	assert(elt);

	if (h->key_size < 0) bucket = get_bucket(h, *(char**)elt->data);
	else bucket = get_bucket(h, elt->data);
	elt = NULL;
	//VERBOSE("find elt after %p", prev);

	elt = hashtbl_find_element_starting_at(h, NULL, prev, compare_true);

	if (elt) return elt;

	return hashtbl_find_element_from_bucket(h, NULL,
			bucket + 1, compare_true);
}

int hashtbl_get_count(hashtbl_t *h) {
	int count;
	assert(h);
	if (TS) pthd_mutex_lock(&h->count_mutex);
	count = h->count;
	if (TS) pthd_mutex_unlock(&h->count_mutex);
	return count;
}

void hashtbl_verbose(hashtbl_t *h) {
	assert(h);
	//VERBOSE("%p %d %d %d", h->buckets, h->key_bits, h->key_size, h->count);
}

void hashtbl_ts_traverse(hashtbl_t *ht, void (*rep)(void*, hashtbl_elt_t*),
		void *arg) {
	hashtbl_elt_t *elt = NULL;

	pthread_cleanup_push((void (*)(void*))hashtbl_unlock_elementp, &elt);

	elt = hashtbl_first_element(ht);
	while (elt) {
		(*rep)(arg, elt);
		elt = hashtbl_next_element_byptr(ht, elt);
	}

	pthread_cleanup_pop(0);
}
