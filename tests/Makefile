# Unit tests for layered SoundFont instruments.
# The fixtures are small SF2 files synthesized in memory by the test itself;
# copies are also written to ./generated for inspection (git-ignored).

CC ?= cc
# -Wno-null-pointer-subtraction silences a clang 16+ warning inside tsf.h's
# offsetof-style macros (unknown -Wno- options are ignored by GCC).
CFLAGS ?= -std=c99 -Wall -Wextra -Wpedantic -Wno-null-pointer-subtraction -g
LDLIBS = -lm

test_layers: test_layers.c ../tsf.h
	$(CC) $(CFLAGS) -I.. test_layers.c $(LDLIBS) -o test_layers

test: test_layers
	./test_layers

clean:
	rm -f test_layers
	rm -rf test_layers.dSYM generated

.PHONY: test clean
