# Unit tests for TinySoundFont.
#
#   make          build and run all tests
#   make test     same as above
#   make clean    remove build artifacts
#
# Requires a C99 compiler (cc by default, override with `make CC=clang`).

CC      ?= cc
CFLAGS  ?= -O2
CFLAGS  += -std=c99 -Wall -Wextra -Wshadow -pedantic
LDLIBS  ?= -lm

BIN := test_playback

.PHONY: all test clean

all: test

test: $(BIN)
	./$(BIN)

$(BIN): test_playback.c sf2_builder.h ../tsf.h
	$(CC) $(CFLAGS) -o $@ test_playback.c $(LDLIBS)

clean:
	rm -f $(BIN)
