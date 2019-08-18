strategy: strategy.c verbose.c random.c

all: test test2 test3

test3: test3.c dllist.c verbose.c random.c

test2: test2.c dllist.c verbose.c random.c

test: test.c dllist.c verbose.c random.c

CFLAGS=-Wall -Werror -g

clean:
	rm -f test test2 test3 strategy
