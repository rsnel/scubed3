#ifndef INCLUDE_LSBD_H
#define INCLUDE_LSBD_H 1

#include <stdint.h>
#include "bit.h"
#include "dllist.h"

typedef enum lsbd_io_e {
	LSBD_READ,
	LSBD_WRITE
} lsbd_io_t;

typedef struct lsbd_s {
	/* must be set from init */
	struct blockio_dev_s *dev;

	/* some constants for accessor functions */
	uint16_t mesobits;
	uint32_t mesomask;

	/* current block in memory */
	struct blockio_info_s *cur;
	char *data;
	int updated;

	/* temporary vars for checking the integrity of the device */
	uint64_t next_seqno;

	/* the real device is split into an array of mesoblocks, where
	 * can we find mesoblock 16? -> block_indices[16] is an uint32_t,
	 * the high bits encode the macroblock number and the low
	 * bits encode the index of the mesoblock in the specified
	 * macroblock, see the definitions of ID and NO below */
	uint32_t *block_indices;
} lsbd_t;

int do_req(lsbd_t*, lsbd_io_t, uint64_t, size_t, char*);

#define id(a)   ((a) - l->dev->b->blockio_infos)

#endif /* INCLUDE_LSBD_H */
