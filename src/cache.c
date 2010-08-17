/* cache.c - cache and delayed writer
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
#include <string.h>
#include <errno.h>
#include <assert.h>
#include "verbose.h"
#include "util.h"
#include "pthd.h"
#include "fuse_io.h"
#include "blockio.h"
#include "ecch.h"
#include "cache.h"

void *cache_thread(void *arg) {
	cache_thread_priv_t *priv = arg;

	pthread_exit(NULL);
}

