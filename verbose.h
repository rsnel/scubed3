#ifndef INCLUDE_LSBD_VERBOSE_H
#define INCLUDE_LSBD_VERBOSE_H 1

#include <stdlib.h>
#include <stdio.h>

/* these macros are for textual user readable output, the first argument
 * to each macro must be a double quoted string, so: VERBOSE(msg); is wrong
 * and VERBOSE("%s", msg); is correct (and also good practise). */
#define WHINE(a,...) fprintf(stderr, "%s:" a "\n", exec_name, ## __VA_ARGS__)
extern char *exec_name;
extern int debug;
extern int verbose;
#define WARNING(a,...) WHINE("warning:" a, ## __VA_ARGS__)
#define DEBUG(a,...) if (debug) WHINE("debug:" a, ## __VA_ARGS__)
#define VERBOSE(a,...) if (verbose) WHINE(a, ## __VA_ARGS__)
#define FATAL(a,...) do { \
	WHINE("fatal:" a, ## __VA_ARGS__); exit(1); } while (0)


void verbose_init(char*);

void verbose_md5(char*);

#endif /* INCLUDE_LSBD_VERBOSE_H */
