#ifndef INCLUDE_LSBD_FUSE_IO_H
#define INCLUDE_LSBD_FUSE_IO_H 1

#include <fcntl.h>
#include "lsbd.h"

int fuse_io_start(int, char**, lsbd_t*);

#endif /* INCLUDE_LSBD_FUSE_IO_H */
