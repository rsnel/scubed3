AUTOMAKE_OPTIONS = foreign
noinst_PROGRAMS = scubed3 scubed3ctl
scubed3_SOURCES = scubed3.c scubed3.h binio.c binio.h bitmap.c bitmap.h \
		  blockio.c blockio.h cipher.c cipher.h dllarr.c dllarr.h \
		  fuse_io.c fuse_io.h gcry.c gcry.h hashtbl.c hashtbl.h \
		  pthd.c pthd.h util.c util.h verbose.c verbose.h \
		  cipher_null.c cipher_cbc.c control.c control.h ecch.c ecch.h \
		  random.c random.h cache.c cache.h
scubed3ctl_SOURCES = scubed3ctl.c verbose.c verbose.h gcry.c gcry.h \
		     ecch.h ecch.c hashtbl.c hashtbl.h pthd.c pthd.h \
		     util.c util.h
AM_CFLAGS = -D_GNU_SOURCE -O3 -g -Wall -Werror -D_FILE_OFFSET_BITS=64
AM_LDFLAGS = -lpthread -lfuse -lgcrypt -lm -lrt -lgpg-error
scubed3ctl_LDFLAGS = -lreadline -lgcrypt -lpthread -lgpg-error
