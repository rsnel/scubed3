/* plmgr.h - paranoia level manager
 *
 * Copyright (C) 2019  Rik Snel <rik@snel.it>
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
#ifndef INCLUDE_SCUBED3_PLMGR_H
#define INCLUDE_SCUBED3_PLMGR_H 1

#include "blockio.h"

typedef struct plmgr_thread_priv_s {
	pthread_mutex_t complain_mutex;
	pthread_cond_t complain_cond;
	blockio_t *b;
	
	/* write request from scubed3 partitions
	 * to make space in the cache, pleasewrite_name
	 * and pleasewrite_ptr are protected by
	 * pleasewrite mutex */
	pthread_mutex_t pleasewrite_mutex;
	pthread_cond_t pleasewrite_cond;
	char *pleaswrite_name; /* this must be COPY of the name */
	void *pleasewrite_ptr;
} plmgr_thread_priv_t;

void *plmgr_thread(void *arg);

void plmgr_thread_cancel_join_cleanup(pthread_t, plmgr_thread_priv_t*);

#endif /* INCLUDE_SCUBED3_PLMGR_H */
