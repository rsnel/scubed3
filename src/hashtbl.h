/* hashtbl.h - hash table
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
#ifndef INCLUDE_SCUBED3_HASHTBL_H
#define INCLUDE_SCUBED3_HASHTBL_H 1

#include <stdio.h>
#include <stdarg.h>

typedef struct hashtbl_elt_s {
	struct hashtbl_elt_s *next;
	pthread_mutex_t ptr_mutex;
	pthread_mutex_t data_mutex;
	char *key;
} hashtbl_elt_t;

typedef struct hashtbl_bucket_s {
	struct hashtbl_elt_s *first;
	pthread_mutex_t ptr_mutex;
	pthread_mutex_t uniq_mutex;
} hashtbl_bucket_t;

typedef struct hashtbl_s {
	hashtbl_bucket_t *buckets;
	signed key_size :8; /* if positive, it represents the size of 
			       the key in bytes if zero it signifies 
			       there is no key, if negative it means 
			       the key is an ASCIIZ string */
	unsigned key_bits :5; /* number of key bits that are
				 used for bucket selection */
	unsigned unique :1;
	unsigned thread_safe :1;

	void (*freer)(void*); /* function to call on each element
					 if the table is destroyed */

	int count;

	pthread_mutex_t count_mutex;
} hashtbl_t;

void hashtbl_free(hashtbl_t*);

void hashtbl_empty(hashtbl_t*);

void hashtbl_init_default(hashtbl_t*, int, int, int, int, void (*)(void*));

void *hashtbl_add_element(hashtbl_t*, void*);

void *hashtbl_allocate_and_add_element(hashtbl_t*, void*, int);

void *hashtbl_find_element_bykey(hashtbl_t*, const void*);

void hashtbl_delete_element_bykey(hashtbl_t*, void*);

void *hashtbl_first_element(hashtbl_t*);

void *hashtbl_next_element_byptr(hashtbl_t*, void*);

void hashtbl_unlock_element_byptr(void*);

void hashtbl_delete_element_byptr(hashtbl_t*, void*);

int hashtbl_get_count(hashtbl_t*);

void hashtbl_verbose(hashtbl_t*);

int hashtbl_ts_traverse(hashtbl_t*, int (*)(void*, hashtbl_elt_t*), void*);

void hashtbl_elementp_free(hashtbl_elt_t**);

void hashtbl_elementp_init_add(void*, hashtbl_t*, void*, int);

void hashtbl_elementp_init_find(void*, hashtbl_t*, void*);

void hashtbl_elementp_init_random(void*, hashtbl_t*);

#endif /* INCLUDE_SCUBED3_HASHTBL_H */
