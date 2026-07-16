CC      := gcc
# -pthread is required project-wide now that test/test_threads.c (built via
# the generic pattern rule below, same as every other test/*.c) links
# against pthreads -- without it, linking is only "accidentally" fine on
# glibc systems that merge libpthread into libc, and breaks elsewhere.
CFLAGS  := -Wall -Wextra -std=c11 -Iinclude -pthread
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

.PHONY: all test asan bench run clean valgrind gdb help threads

all: $(TEST_BINS) $(BENCH_BIN)

help:
	@echo "Targets:"
	@echo "  make            - build every test binary and the benchmark"
	@echo "  make test       - build and run all test/*.c binaries (excludes benchmark)"
	@echo "  make asan       - rebuild everything with -fsanitize=address,undefined and run it"
	@echo "  make bench      - build and run the benchmark (BENCH_OPS=$(BENCH_OPS) by default)"
	@echo "  make threads    - build and run just test/test_threads (the pthread stress test)"
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

# Build and run just the pthread stress test on its own, without the rest
# of the test/*.c suite. Uses the same pattern rule (and same -pthread
# CFLAGS) as `make test`, so this is exactly what `make test` runs for
# test_threads -- just isolated and easy to re-run on its own.
threads: $(TEST_DIR)/test_threads
	@echo "== running $(TEST_DIR)/test_threads =="
	./$(TEST_DIR)/test_threads

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

thread:
	gcc -pthread -std=c11 -Iinclude -g -O0 -o test/test_threads_helgrind src/my-malloc.c test/test_threads.c
	valgrind --tool=helgrind --history-level=full ./test/test_threads_helgrind

clean:
	rm -f $(TEST_BINS) $(BENCH_BIN)