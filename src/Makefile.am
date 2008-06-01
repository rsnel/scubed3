AUTOMAKE_OPTIONS = foreign dist-bzip2
noinst_PROGRAMS = scubed3
scubed3_SOURCES = scubed3.c abl.c binio.c bit.c bitmap.c blockio.c cipher.c \
	       dllist.c fuse_io.c gcry.c hashtbl.c pthd.c util.c verbose.c
AM_CFLAGS = -D_GNU_SOURCE -Wall -Werror -D_FILE_OFFSET_BITS=64
AM_LDFLAGS = -lpthread -lfuse -lgcrypt
