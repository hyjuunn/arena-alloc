CC := gcc
CFLAGS := -Wall -Wextra -O2 -g -std=c11
INCLUDES := -Iinclude

SRC := src/jmalloc.c
OBJ := $(SRC:.c=.o)

all: app bench

app: src/main.o $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^

bench: tests/bench.o $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^

src/%.o: src/%.c include/jmalloc.h
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

tests/%.o: tests/%.c include/jmalloc.h
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

clean:
	rm -f src/*.o tests/*.o app bench

.PHONY: all clean
