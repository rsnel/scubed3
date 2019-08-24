all: badrandom strategy exact simul

simul: simul.c verbose.c random.c

exact: exact.c verbose.c

badrandom: badrandom.c verbose.c random.c

strategy: strategy.c verbose.c random.c

test3: test3.c dllist.c verbose.c random.c

test2: test2.c dllist.c verbose.c random.c

test: test.c dllist.c verbose.c random.c

LDFLAGS=-lm
CFLAGS=-Wall -Werror -g -O3

clean:
	rm -f test test2 test3 strategy badrandom
