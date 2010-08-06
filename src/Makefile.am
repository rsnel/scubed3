AUTOMAKE_OPTIONS = foreign
noinst_PROGRAMS = scubed3 scubed3ctl
scubed3_SOURCES = scubed3.c scubed3.h cipher_abl.c binio.c binio.h \
		  bitmap.c bitmap.h blockio.c blockio.h cipher.c cipher.h \
		  dllist.c dllist.h fuse_io.c fuse_io.h gcry.c gcry.h \
		  hashtbl.c hashtbl.h pthd.c pthd.h util.c util.h \
		  verbose.c verbose.h cipher_null.c cipher_cbc_large.c \
		  ext2.c ext2.h control.c control.h ecch.c ecch.h \
		  random.c random.h
scubed3ctl_SOURCES = scubed3ctl.c verbose.c verbose.h gcry.c gcry.h \
		     ecch.h ecch.c hashtbl.c hashtbl.h
AM_CFLAGS = -D_GNU_SOURCE -O2 -g -Wall -Werror -D_FILE_OFFSET_BITS=64
AM_LDFLAGS = -lpthread -lfuse -lgcrypt
scubed3ctl_LDFLAGS = -lreadline -lgcrypt -lpthread
