CC      ?= cc
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror
CPPFLAGS = -Iinclude -D_DEFAULT_SOURCE -D_DARWIN_C_SOURCE
# AddressSanitizer crashes on startup with some macOS toolchains, so macOS defaults
# to UBSan only. CI (Linux) runs both. Override with: make test SANITIZERS=address,undefined
ifeq ($(shell uname -s),Darwin)
SANITIZERS ?= undefined
else
SANITIZERS ?= address,undefined
endif
SAN      = -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=$(SANITIZERS) -fno-sanitize-recover=all -fno-omit-frame-pointer

LIB_SRC = src/book.c src/pool.c src/idmap.c src/journal.c
HDRS    = $(wildcard include/*.h)

.PHONY: all test fuzz bench analyze clean

all: build/matchbook build/bench

build:
	mkdir -p build

build/matchbook: src/main.c $(LIB_SRC) $(HDRS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ src/main.c $(LIB_SRC)

build/bench: src/bench.c $(LIB_SRC) $(HDRS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ src/bench.c $(LIB_SRC)

# Unit tests and the fuzz binary are built with sanitizers (see SANITIZERS above).
build/test: tests/test_main.c $(LIB_SRC) $(HDRS) | build
	$(CC) $(CPPFLAGS) $(SAN) -o $@ tests/test_main.c $(LIB_SRC)

build/matchbook-san: src/main.c $(LIB_SRC) $(HDRS) | build
	$(CC) $(CPPFLAGS) $(SAN) -o $@ src/main.c $(LIB_SRC)

test: build/test
	./build/test

# Differential test: random order flow through the C engine vs. the Python reference model.
fuzz: build/matchbook-san
	python3 tools/fuzz.py --binary build/matchbook-san --runs 300

bench: build/bench
	./build/bench -n 5000000

analyze: build/bench
	./build/bench -n 1000000 --csv build/latency.csv
	python3 analysis/latency.py build/latency.csv --plot build/latency.png

clean:
	rm -rf build
