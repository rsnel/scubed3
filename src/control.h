/* control.h - control socket server
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
#ifndef INCLUDE_SCUBED3_CONTROL_H
#define INCLUDE_SCUBED3_CONTROL_H 1

#include <fcntl.h>
#include "scubed3.h"
#include "blockio.h"
#include "hashtbl.h"

/* the socket is in a directory that only the owner of the scubed3 process
 * can access, so nobody else can connect to it (not even for a moment) */
#define CONTROL_SOCKET "/run/scubed3/control"

typedef struct control_thread_priv_s {
	hashtbl_t *h;
	hashtbl_t c;
	blockio_t *b;
	hashtbl_t *ids;
	char *mountpoint;
	int listen_fd; /* from control_listen() */
} control_thread_priv_t;

int control_listen(const char*);

void *control_thread(void *arg);

#endif /* INCLUDE_SCUBED3_CONTROL_H */
