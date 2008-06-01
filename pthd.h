/* pthd.h some exception generating versions of libpthread functions
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
#ifndef INCLUDE_LSBD_PTHD_H
#define INCLUDE_LSBD_PTHD_H 1

#include <pthread.h>

void pthd_mutex_destroy(pthread_mutex_t*);

void pthd_mutex_lock(pthread_mutex_t*);

void pthd_mutex_unlock(pthread_mutex_t*);

void pthd_mutex_init(pthread_mutex_t*);

void pthd_cond_init(pthread_cond_t*);

void pthd_cond_signal(pthread_cond_t*);

void pthd_cond_wait(pthread_cond_t*, pthread_mutex_t*);

void pthd_cond_destroy(pthread_cond_t*);

#endif /* INCLUDE_LSBD_PTHD_H */