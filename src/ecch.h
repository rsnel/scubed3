/* ecch.h exceptions and pthread cancel cleanup handling
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
#ifndef INCLUDE_LIBUTILS_ECCH_H
#define INCLUDE_LIBUTILS_ECCH_H 1

#include "config.h"

#include <setjmp.h>
#include <pthread.h>
#include <string.h>
#include "assert.h"
#include "verbose.h"

/* every subsystem can have it's own exceptions, in principle they are global,
 * to avoid collission every subsystem that wants it's own exceptions should
 * have its prefix defined here, subsystems from applications should use
 * prefixes with 0x80?? and use their own way of avoiding collisions */
#define ECCH_DEFAULT_PFX		0x0000
#define ECCH_GIO_PFX			0x0001
#define ECCH_HASHTBL_PFX		0x0002
#define ECCH_GIO_NET_PFX		0x0003
#define ECCH_GIO_FILTER_BASEXX_PFX	0x0004
#define ECCH_GCRY_PFX			0x0005
#define ECCH_DSA_PFX			0x0006
#define ECCH(a,b)			(ECCH_##a##_PFX<<16)|b

#define ECCH_DEFAULT			ECCH(DEFAULT, 1)

#define ecch_set_context(a) pthread_setspecific(ecch_context_key, a)
extern pthread_key_t ecch_context_key;

#define ECCH_MSG_SIZE 256

typedef struct {
	char *file;
	int line_no;
	char msg[ECCH_MSG_SIZE];
} ecch_t;

typedef struct ecch_cleanup_s {
	struct ecch_cleanup_s *prev;
	void (*routine)(void*);
	void *arg;
	char *type;
	char *subtype;
	int level;
} ecch_cleanup_t;

typedef struct ecch_context_s {
	struct ecch_context_s *prev;
	ecch_cleanup_t *last;
	jmp_buf env;
	int level;
	ecch_t ecch;
} ecch_context_t;

#define ecch_initp_internal(b,c,d,e,...) \
	ecch_cleanup_t _##e##_ecch; \
	memset(d, 0, sizeof(*(d))); \
	ecch_context_add(&_##e##_ecch, ecch_contextp, \
			(void (*))(void*)&b##_free, d, #b, #c, \
			__FILE__, __LINE__); \
	b##_init_##c(d, ## __VA_ARGS__);

#define ecch_initp(b,c,d,...) ecch_initp_internal(b, c, d, d, ## __VA_ARGS__)

#define ecch_init(b,c,d,...) ecch_initp_internal(b, c, &d, d, ## __VA_ARGS__)

#define ecch_initc(a,b,c,d,...) \
	a##_##b##_t _##d##_base; \
	d = (a##_t*)&_##d##_base; \
	ecch_initp_internal(a##_##b, c, &_##d##_base, d, ## __VA_ARGS__)

#define ecch_throw_default(a,...) ecch_throw(ECCH_DEFAULT, a, ## __VA_ARGS__)

#define ecch_thread_start pthread_cleanup_push(ecch_cleanup_thread, NULL)
#define ecch_thread_end pthread_cleanup_pop(1)

#define ecch_start { \
	ecch_context_t *ecch_contextp = ecch_context_cur; \
	assert(ecch_contextp); \
	ecch_contextp->level++;

#define ecch_end \
	ecch_cleanup_levels(ecch_contextp, "end of scope", __FILE__, \
			__LINE__, ecch_contextp->level); \
	ecch_contextp->level--; \
}

#define ecch_try { \
	ecch_context_t ecch_context,  *ecch_contextp = &ecch_context; \
	uint32_t type; \
	ecch_try_common(ecch_contextp); \
	type = setjmp(ecch_context.env); \
	if (type) { \
		ecch_set_context(ecch_context_cur->prev); \
	} \
	if (!type) {

#define ecch_catch(a) } else if (type == (a)) {

#define ecch_catch_type(a) } else if (type>>16 == (a)) {

#define ecch_catch_all } else if (type) {

#define ecch_endtry } else { \
		ecch_throw(type, "%s", ecchmsg); \
	} \
	if (!type) { \
		ecch_cleanup_levels(ecch_contextp, "end of exception", \
				__FILE__, __LINE__ - 1, 0); \
		ecch_set_context(ecch_context.prev); \
	} \
}

void ecch_global_init(void);

void ecch_try_common(ecch_context_t*);

void ecch_cleanup_thread(void*);

void ecch_cleanup_levels(ecch_context_t*, const char*, char*, int, int);

void ecch_debug(ecch_cleanup_t*, char*, int, char*, const char*);

void ecch_context_add(ecch_cleanup_t*, ecch_context_t*, void (*)(void*),
		void*, char*, char*, char*, int);

#define ecch_context_cur \
	((ecch_context_t*)pthread_getspecific(ecch_context_key))

#define ecchmsg ecch_contextp->ecch.msg

#define ecch_throw(a,b,...) do { \
	ecch_context_t *c = ecch_context_cur; \
	if (!c) BUG("unhandled exception: " b, ## __VA_ARGS__); \
	/*DEBUG("throwing exception: " b, ## __VA_ARGS__); */\
	snprintf(c->ecch.msg, ECCH_MSG_SIZE, b, ## __VA_ARGS__); \
	ecch_cleanup_levels(c, "exception", __FILE__, __LINE__, 0); \
	c->ecch.file = __FILE__; \
	c->ecch.line_no = __LINE__; \
	longjmp(c->env, a); \
} while (0)

#endif /* INCLUDE_LIBUTILS_ECCH_H */
