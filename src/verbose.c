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

void verbose_md5(char *md5sum_res) {
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
