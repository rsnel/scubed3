/* ext2.c - ext2 mount/umount detection
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
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <assert.h>

#include "scubed3.h"
#include "verbose.h"
#include "ext2.h"
#include "util.h"

#define EXT2_MAGIC 0xef53

#define S_MAGIC			(super+56)
#define S_STATE			(super+58)

#define EXT2_FS_DIRTY		0
#define EXT2_FS_CLEAN		1

ext2_t *ext2_init(scubed3_t *s) {
	ext2_t *e = ecalloc(sizeof(ext2_t), 1);
	char buf[16384], *super = buf + 1024;

	do_req(s, SCUBED3_READ, 0, 16384, buf);

	e->s = s;

	if (binio_read_uint16_le(S_MAGIC) != EXT2_MAGIC) {
		VERBOSE("ext2 fs not found, wrong magic 0x%04x",
				binio_read_uint16_le(S_MAGIC));
		free(e);
		return NULL;
	}

	VERBOSE("ext2fs found");

	if (binio_read_uint16_le(S_STATE) != EXT2_FS_CLEAN) {
		VERBOSE("filesystem is in use or not cleanly umounted");
		free(e);
		return NULL;
	}

	e->mounted = 0;

	return e;
}

#if 0
void ext2_handler(ext2_t *e, const void *data, uint32_t mesoblk,
		uint32_t muoff, uint32_t size) {
	const char *idx = data + 1024;
	uint32_t toff = muoff%(1<<e->log);
	uint32_t tstart = muoff/(1<<e->log);
	assert(e->l->b->mesoblk_log > 10);
	if (mesoblk == 0 && muoff <= 1024 && muoff + size >= 2048) {
		if (binio_read_uint16_le(S_STATE) == EXT2_FS_CLEAN &&
				e->mounted) {
			VERBOSE("ext2: filesystem umounted");
			if (e->l->updated) do {
				commit_current_macroblock(e->l);
				select_new_macroblock(e->l);
			} while (e->l->cur->no_indices == e->l->b->mmpm);
			e->mounted = 0;
		} else if (binio_read_uint16_le(S_STATE) == EXT2_FS_DIRTY &&
				!e->mounted) {
			VERBOSE("ext2: filesystem mounted");
			e->mounted = 1;
		}
	}

}
#endif
