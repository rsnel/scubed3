#ifndef INCLUDE_SCUBED3_FUSE_IO_H
#define INCLUDE_SCUBED3_FUSE_IO_H 1

#include <fcntl.h>
#include "lsbd.h"

int fuse_io_start(int, char**, lsbd_t*);

#endif /* INCLUDE_SCUBED3_FUSE_IO_H */
