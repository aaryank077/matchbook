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

# C++ feed handler (cpp/). GoogleTest comes from Homebrew on macOS
# (brew install googletest) and from libgtest-dev's default paths on Linux.
CXX      ?= c++
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra -Wpedantic -Werror
CXXSAN    = -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=$(SANITIZERS) -fno-sanitize-recover=all -fno-omit-frame-pointer
CXXINC    = -Icpp -Iinclude
CPP_HDRS  = $(wildcard cpp/itch/*.hpp)
GTEST_PREFIX ?= $(shell brew --prefix googletest 2>/dev/null)
GTEST     = $(if $(GTEST_PREFIX),-I$(GTEST_PREFIX)/include -L$(GTEST_PREFIX)/lib) -lgtest_main -lgtest -pthread

.PHONY: all test fuzz bench analyze itch-test clean

all: build/matchbook build/bench build/itch_stats

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

build/itch_stats: cpp/tools/itch_stats.cpp $(CPP_HDRS) | build
	$(CXX) $(CXXINC) $(CXXFLAGS) -o $@ cpp/tools/itch_stats.cpp

# ITCH decoder tests, under the same sanitizers as the C tests.
build/itch_test: cpp/tests/itch_test.cpp $(CPP_HDRS) | build
	$(CXX) $(CXXINC) $(CXXSAN) -o $@ cpp/tests/itch_test.cpp $(GTEST)

itch-test: build/itch_test
	./build/itch_test

clean:
	rm -rf build
