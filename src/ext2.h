#ifndef INCLUDE_SCUBED3_EXT2_H
#define INCLUDE_SCUBED3_EXT2_H 1

#include <stdint.h>
#include "blockio.h"
#include "binio.h"

struct scubed3_s;

typedef struct ext2_s {
	struct scubed3_s *s;
	int mounted;
} ext2_t;

ext2_t *ext2_init(struct scubed3_s*);

void ext2_handler(ext2_t*, uint64_t, size_t, const void*);

#endif /* INCLUDE_SCUBED3_EXT2_H */
