AUTOMAKE_OPTIONS = foreign
noinst_PROGRAMS = scubed3
scubed3_SOURCES = scubed3.c scubed3.h cipher_abl.c binio.c binio.h bit.c bit.h \
		  bitmap.c bitmap.h blockio.c blockio.h cipher.c cipher.h \
		  dllist.c dllist.h fuse_io.c fuse_io.h gcry.c gcry.h \
		  hashtbl.c hashtbl.h pthd.c pthd.h util.c util.h \
		  verbose.c verbose.h cipher_null.c cipher_cbc_large.c \
		  ext2.c ext2.h
AM_CFLAGS = -D_GNU_SOURCE -O2 -g -Wall -Werror -D_FILE_OFFSET_BITS=64
AM_LDFLAGS = -lpthread -lfuse -lgcrypt
