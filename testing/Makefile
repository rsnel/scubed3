all: test rtest

test: test.c verbose.c juggler.c util.c random.c blockio.h binio.c gcry.c ecch.c

rtest: rtest.c verbose.c random.c

LDFLAGS=-lm -lgcrypt -lgpg-error -lpthread
CFLAGS=-Wall -Werror -g -O3 -D_GNU_SOURCE -I..

clean:
	rm -f test
