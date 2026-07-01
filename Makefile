CC      := gcc
CFLAGS  := -Wall -Wextra -std=c11 -Iinclude
DBGFLAGS:= -g
OPTFLAGS:= -O2
ASAN    := -fsanitize=address,undefined

SRC := src/my-malloc.c
TEST_DIR := test

# benchmark.c is handled separately (it wants -O2, and it doesn't make
# sense to include it in the plain pass/fail "test" target). Every OTHER
# test/*.c file is auto-discovered and built into a same-named binary --
# add a new file to test/ and it's picked up automatically, no Makefile
# edits required.
BENCH_SRC := $(TEST_DIR)/benchmark.c
BENCH_BIN := $(TEST_DIR)/benchmark

TEST_SRCS := $(filter-out $(BENCH_SRC),$(wildcard $(TEST_DIR)/*.c))
TEST_BINS := $(TEST_SRCS:.c=)

# Overridable from the command line, e.g. `make asan BENCH_OPS=20000`
BENCH_OPS ?= 5000

# Which binary `make gdb` / `make valgrind` target by default (override
# with e.g. `make gdb TARGET=test/test-edge-cases`)
TARGET ?= $(firstword $(TEST_BINS))

.PHONY: all test asan bench run clean valgrind gdb help

all: $(TEST_BINS) $(BENCH_BIN)

help:
	@echo "Targets:"
	@echo "  make            - build every test binary and the benchmark"
	@echo "  make test       - build and run all test/*.c binaries (excludes benchmark)"
	@echo "  make asan       - rebuild everything with -fsanitize=address,undefined and run it"
	@echo "  make bench      - build and run the benchmark (BENCH_OPS=$(BENCH_OPS) by default)"
	@echo "  make run        - alias for 'make test'"
	@echo "  make valgrind   - run TARGET under valgrind (default: $(TARGET))"
	@echo "  make gdb        - open TARGET in gdb (default: $(TARGET))"
	@echo "  make clean      - remove all built binaries"
	@echo "Current test binaries: $(TEST_BINS)"

# Pattern rule: test/foo comes from test/foo.c + the allocator source.
# Covers test-basic, test-edge-cases, and any future test/*.c you add.
$(TEST_DIR)/%: $(TEST_DIR)/%.c $(SRC) include/my-malloc.h include/list.h
	$(CC) $(CFLAGS) $(DBGFLAGS) -o $@ $(SRC) $<

$(BENCH_BIN): $(BENCH_SRC) $(SRC) include/my-malloc.h include/list.h
	$(CC) $(CFLAGS) $(OPTFLAGS) $(DBGFLAGS) -o $@ $(SRC) $(BENCH_SRC)

test: $(TEST_BINS)
	@status=0; \
	for t in $(TEST_BINS); do \
		echo "== running $$t =="; \
		./$$t || status=1; \
	done; \
	exit $$status

run: test

bench: $(BENCH_BIN)
	./$(BENCH_BIN) --ops $(BENCH_OPS)

# Force a clean rebuild under the sanitizers -- target-specific CFLAGS
# apply to this target's prerequisites too, so `clean` first guarantees
# we're never running a stale non-instrumented binary.
asan: CFLAGS += $(ASAN)
asan: clean $(TEST_BINS) $(BENCH_BIN)
	@status=0; \
	for t in $(TEST_BINS); do \
		echo "== running $$t (asan) =="; \
		./$$t || status=1; \
	done; \
	echo "== running $(BENCH_BIN) (asan, BENCH_OPS=$(BENCH_OPS)) =="; \
	./$(BENCH_BIN) --ops $(BENCH_OPS) --no-libc || status=1; \
	exit $$status

valgrind: $(TARGET)
	valgrind --error-exitcode=1 --leak-check=full ./$(TARGET)

gdb: $(TARGET)
	gdb ./$(TARGET)

s:
	gcc -O2 -Wall -Wextra -std=c11 -Iinclude -g -o test/benchmark src/my-malloc.c test/benchmark.c
	./test/benchmark --ops 5000 --seed 42	

clean:
	rm -f $(TEST_BINS) $(BENCH_BIN)