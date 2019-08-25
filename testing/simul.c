#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <errno.h>
#include <string.h>
#include "random.h"
#include "verbose.h"

/* calculate expected value of number of datablocks after an amount of iterations */
/* if the expected value converges, we will get no datablocks, if it keeps growing without bound, 
 * then we will have stable datablocks */
#define NO_BLOCKS 210
#define SIMULTANEOUS_BLOCKS 100
#define STEPS 100000000

int out_len = 0;
int removed = 0;
unsigned char out[STEPS];

void show() {
	for (int i = 0; i < out_len; i++) printf("%x", out[i]);
	printf("\n");
}

int remove_entry(unsigned char *array, int idx, int len) {
	assert(idx < len);
	assert(idx >= 0);
	memmove(array + idx, array + idx + 1, len - idx - 1);
	return len - 1;
}

typedef struct tovisit_s {
	struct tovisit_s *next;
	int shift;
} tovisit_t;

typedef struct visits_s {
	tovisit_t *head;
	tovisit_t **tail_next;
	int info; // length minus stuff somewhat
} visits_t;

void visits_push(visits_t *v, tovisit_t *t) {
	assert(!t->next);
	*v->tail_next = t;
	v->tail_next = &t->next;
}

void visits_push_shift(visits_t *v, int pos) {
	tovisit_t *t;

	//VERBOSE("visits_push_shift pos=%d, v->info=%d", pos, v->info);
	if (pos + 1 == v->info) {
		//VERBOSE("calloc and push");
		t = calloc(1, sizeof(*t));
		//VERBOSE("v->head=%p, t=%p, &v->head=%p, v->tail_next=%p", v->head, t, &v->head, v->tail_next);
		visits_push(v, t);
		v->info--;
	}/* else if (pos > v->info) {
		FATAL("impossible?!?!");
	}*/ //else VERBOSE("we have already, only increase shift");

	//t = v->head;

#if 0
	while (t) {
		t->shift++;
		t = t->next;
	}
#endif
}

void visits_increase_shifts(visits_t *v) {
	tovisit_t *t = v->head;
	while (t) {
		t->shift++;
		t = t->next;
	}
}

void visits_shift(visits_t *v) {
	v->info--;
}

void visits_free(visits_t *v) {
	assert(!v->head && v->tail_next == &v->head);
}

void visits_init(visits_t *v, int info) {
	v->head = NULL;
	v->tail_next = &v->head;
	v->info = info;

	visits_push_shift(v, info-1);
	// push_shift sets distance 1 by default,
	// but the added value must have the maximum distance
	v->head->shift += SIMULTANEOUS_BLOCKS;
}

tovisit_t *visit_pop(visits_t *v) {
	//VERBOSE("pop v->head=%p v->head->next=%p !", v->head, v->head->next);
	tovisit_t *t;
	if (!v->head) return NULL;

	t = v->head;
	v->head = t->next;

	if (t->next) {
		t->next = NULL;
	} else {
	//	VERBOSE("pop: update tail_next");
		v->tail_next = &v->head;
	}

	return t;
}

int visit_pop_shift(visits_t *v) {
	int shift;
	tovisit_t *t = visit_pop(v);
	if (!t) return 0;

	shift = t->shift;

	free(t);

	return shift;
}

void showvisits(visits_t *v) {
	tovisit_t *t = v->head;

	VERBOSE("visit: info=%d", v->info);
	while (t) {
		VERBOSE("visit: shift=%d", t->shift);
		t = t->next;
	}
}

void show_array(unsigned char *array, int length) {
	printf("state: ");
	for (int i = 0; i < length; i++) {
		printf("%x", array[i]);
	}
	printf("\n");
}

int maxdepth = 0;

int findbad(unsigned char *array, int check, visits_t *v, int length) {
	if (out_len - check - 1 > maxdepth) maxdepth = out_len - check - 1;

	int shift = visit_pop_shift(v);

	for (int distance = SIMULTANEOUS_BLOCKS - shift +1; check - distance >= 0 && distance <= SIMULTANEOUS_BLOCKS; distance++) {
		//VERBOSE("checking array[%d]=%u and array[%d]=%u at distance %d", check - distance, array[check-distance], check, array[check], distance);
		if (array[check-distance] == array[check]) {
			length = remove_entry(array, check - distance, length);
			check--;
			visits_shift(v);
			for (int i = check - 1; i > check - distance; i--) {
				visits_push_shift(v, i);
			}
			visits_increase_shifts(v);
			break;
		}


	}

	return v->head?findbad(array, check - 1, v, length):length;
}

int main(int argc, char *argv[]) {
	random_t r;
	verbose_init(argv[0]);
	random_init(&r, NO_BLOCKS);
#if 0
	//int len = 13;
	//unsigned char array[] = { 0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3, 1 };
	int len = 2;
	unsigned char array[] = { 0, 0 };
	visits_t v;
	visits_init(&v, len);
	len = findbad(array, len - 1, &v, len);
	VERBOSE("new len=%d", len);
	visits_free(&v);
#endif

#if 1

	for (int i = 0; i < STEPS; i++) {
		if (i%100000 == 0) {
			VERBOSE("frac=%f, maxdepth=%d, generated=%u, depth fraction=%f", ((double)out_len)/((double)i), maxdepth, i, ((double)maxdepth/(double)i));
			//VERBOSE("sep2frac=%f", ((double)succsep2)/((double)testsep2));
		}
		out[out_len++] = random_pop(&r);
		//VERBOSE("before");
		//show_array(out, out_len);
#if 1
		visits_t v;
		visits_init(&v, out_len);
		out_len = findbad(out, out_len - 1, &v, out_len);
		visits_free(&v);
#else
		for (int j = out_len - 1; j >= 0 /*&& j >= last_removal - 2*SIMULTANEOUS_BLOCKS*/; j--) {
			for (int k = 1; k <= SIMULTANEOUS_BLOCKS && j + k < out_len; k++) {
				//VERBOSE("compare j+k=%d with j=%d", j + k, j);
				//if (out_len - k >= SIMULTANEOUS_BLOCKS) found = 0;
				if (out[j+k] == out[j]) {
					//last_removal = j;
					out_len = remove_entry(out, j, out_len);
					break;
				}
			}
		}
#endif
		//VERBOSE("after");
		//show_array(out, out_len);
	///	show();
	}
	//show(out, out_len);
#if 0
	int equal= 0;
	int total = 0;

	for (int i = 0; i < STEPS - 3; i++) {
		if (out[i] == out[i+3]) equal++;
		total++;
	}
	VERBOSE("equal/total=%f", ((double)equal)/((double)total));
#endif
#endif
}

