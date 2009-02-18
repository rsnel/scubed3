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
#include "config.h"

#include <pthread.h>
#include <assert.h>
#include "verbose.h"
//#include "globalch.h"
#include "ecch.h"

pthread_key_t ecch_context_key;

//static void global_free() {
//	pthread_key_delete(ecch_context_key);
//}

void ecch_global_init() {
	if (pthread_key_create(&ecch_context_key, NULL))
		FATAL("unable to allocate pthread key for ecch context");
	//GLOBALCH_INFO_PUSH;
}

void ecch_try_common(ecch_context_t *c) {
	c->prev = ecch_context_cur;
	c->last = NULL;
	c->level = 0;
	ecch_set_context(c);
}

static ecch_cleanup_t *ecch_cleanup(ecch_cleanup_t *p, 
		const char *reason, char *file,
		int line, int level) {
	while (p && p->level >= level) {
		assert(p->routine && p->type);
		ecch_debug(p, file, line, "free", reason);
		(p->routine)(p->arg);
		p = p->prev;
	}
	return p;
}

void ecch_cleanup_levels(ecch_context_t *c, const char *reason, char *file,
		int line, int level) {
	ecch_cleanup_t *p;
	assert(c);
	p = c->last;
	c->last = ecch_cleanup(p, reason, file, line, level);
}

void ecch_cleanup_thread(void *should_be_null) {
	assert(!should_be_null);
	while (ecch_context_cur) {
		ecch_cleanup_levels(ecch_context_cur, "thread cancellation", 
				__FILE__, __LINE__, 0);

		ecch_set_context(ecch_context_cur->prev);
	}
	/* tell TSD cleanup handler that ecch_thread_cleanup was called */
	//ecch_set_context(NULL); /* is this really needed? */
}

void ecch_debug(ecch_cleanup_t *c, char *file, int line, char *mode, 
		const char *reason) {
#if 0
	  VERBOSE("<%d>:%s:%d:%s_%s%s%s(%p)%s%s%s", c->level, file,
	  //VERBOSE("<%d>:%s:%d:%s_%s%s%s%s%s%s", c->level, file,
		line, c->type, mode, c->subtype?"_":"", 
		c->subtype?c->subtype:"", c->arg, reason?" (":"", 
		reason?reason:"", reason?")":"");
#endif
}

void ecch_context_add(ecch_cleanup_t *c, ecch_context_t *o,
		void (*func)(void*), void *arg, char *type, char *subtype,
		char *file, int line) {
	c->prev = o->last;
	o->last = c;
	c->level = o->level;
	c->routine = func;
	c->arg = arg;
	c->type = type;
	c->subtype = subtype;
	ecch_debug(c, file, line, "init", NULL);
}

