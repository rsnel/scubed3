all: test rtest

test: test.c verbose.c juggler.c util.c random.c macroblock.h

rtest: rtest.c verbose.c random.c

LDFLAGS=-lm
CFLAGS=-Wall -Werror -g -O3 -D_GNU_SOURCE

clean:
	rm -f test
