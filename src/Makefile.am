AUTOMAKE_OPTIONS = foreign
sbin_PROGRAMS = scubed3 scubed3ctl keystorectl
scubed3_SOURCES = scubed3.c scubed3.h binio.c binio.h bitmap.c bitmap.h \
		  blockio.c blockio.h cipher.c cipher.h dllarr.c dllarr.h \
		  fuse_io.c fuse_io.h gcry.c gcry.h hashtbl.c hashtbl.h \
		  pthd.c pthd.h util.c util.h verbose.c verbose.h \
		  cipher_null.c cipher_cbc.c control.c control.h ecch.c ecch.h \
		  random.c random.h  juggler.c juggler.h
CTL_COMMON = ctl_common.c ctl_common.h verbose.c verbose.h gcry.c gcry.h \
	     ecch.h ecch.c hashtbl.c hashtbl.h pthd.c pthd.h \
	     util.c util.h keystore.c keystore.h
scubed3ctl_SOURCES = scubed3ctl.c $(CTL_COMMON)
keystorectl_SOURCES = keystorectl.c $(CTL_COMMON)
AM_CFLAGS = -D_GNU_SOURCE -O3 -g -Wall -Werror -D_FILE_OFFSET_BITS=64
scubed3_CFLAGS = $(AM_CFLAGS) $(FUSE_CFLAGS)
scubed3_LDADD = -lpthread $(FUSE_LIBS) -lgcrypt -lm -lrt -lgpg-error
scubed3ctl_LDADD = $(READLINE_LIBS) -lgcrypt -lpthread -lgpg-error
keystorectl_LDADD = $(scubed3ctl_LDADD)
scubed3_LDFLAGS = $(STATIC_LDFLAGS)
scubed3ctl_LDFLAGS = $(STATIC_LDFLAGS)
keystorectl_LDFLAGS = $(STATIC_LDFLAGS)
