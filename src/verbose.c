/* verbose.c - verbosity stuff
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
#include <stdio.h>
#include <string.h>

#include "verbose.h"

char *exec_name = NULL;
int debug = 1;
int verbose = 1;

void verbose_init(char *argv0) {
	/* stolen from wget */
	exec_name = strrchr(argv0, '/');
	if (!exec_name++) exec_name = argv0;
}

void verbose_buffer(const char *id, const void *buf, size_t size) {
	char tmp[strlen(id) + 2 + 2*size + 1], *ptr = tmp;
	int ret;

	ret = snprintf(ptr, strlen(id) + 2 + 1, "%s: ", id);
	if (ret < 0 || ret >= strlen(id) + 2 + 1) FATAL("this can't happen");

	ptr += ret;

	while (size) {
		ret = snprintf(ptr, 3, "%02x", *(unsigned char*)buf++);
		if (ret < 0 || ret >= 2 + 1) FATAL("this can't happen");
		ptr += ret;
		size--;
	}

	VERBOSE("%s", tmp);
}

void verbose_md5(const char *md5sum_res) {
	DEBUG("md5: %02x %02x %02x %02x %02x %02x %02x %02x  "
		"%02x %02x %02x %02x %02x %02x %02x %02x",
		(unsigned char)md5sum_res[0], (unsigned char)md5sum_res[1],
		(unsigned char)md5sum_res[2], (unsigned char)md5sum_res[3],
		(unsigned char)md5sum_res[4], (unsigned char)md5sum_res[5],
		(unsigned char)md5sum_res[6], (unsigned char)md5sum_res[7],
		(unsigned char)md5sum_res[8], (unsigned char)md5sum_res[9],
		(unsigned char)md5sum_res[10], (unsigned char)md5sum_res[11],
		(unsigned char)md5sum_res[12], (unsigned char)md5sum_res[13],
		(unsigned char)md5sum_res[14], (unsigned char)md5sum_res[15]);
}

