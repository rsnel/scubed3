all: simul test

simul: simul.c verbose.c juggler.c util.c

test: test.c dllist.c verbose.c random.c

LDFLAGS=-lm
CFLAGS=-Wall -Werror -g -O3 -D_GNU_SOURCE

clean:
	rm -f test
