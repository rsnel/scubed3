/* scubed3ctl.c - scubed3 control program
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
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/user.h>
#undef NDEBUG /* we need sanity checking */
#include <assert.h>

#include "config.h"
#include "verbose.h"

#define SCUBED3_CONTROL "/mnt/scubed3/.control"

int main(int argc, char **argv) {
	FILE *fp;
	char buf[1024];
	int ret;

	verbose_init(argv[0]);

	if (!(fp = fopen(SCUBED3_CONTROL, "r+")))
		FATAL("fopen(\""SCUBED3_CONTROL"\", \"r+\"): %s",
				strerror(errno));

	if ((ret = fprintf(fp, "status\n")) < 0)
		FATAL("fprintf: %s", strerror(errno));

	if (fflush(fp)) FATAL("fflush: %s", strerror(errno));

	if ((ret = fread(buf, 1, sizeof(buf), fp)) >= 0)
		FATAL("fread: %s", strerror(errno));

	VERBOSE("read %d bytes", ret);

	exit(0);
}
