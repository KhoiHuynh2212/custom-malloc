/*
 * test-edge-cases.c - edge case / robustness suite for my-malloc
 *
 * Build:
 *   gcc -Wall -Wextra -std=c11 -Iinclude -g \
 *       -o test/test-edge-cases src/my-malloc.c test/test-edge-cases.c
 *
 * Build with sanitizers (safe to use, allocator no longer shadows libc's
 * malloc/calloc/realloc/free symbol names):
 *   gcc -Wall -Wextra -std=c11 -Iinclude -fsanitize=address,undefined -g \
 *       -o test/test-edge-cases src/my-malloc.c test/test-edge-cases.c
 *
 * Run:
 *   ./test/test-edge-cases            (normal output)
 *   ./test/test-edge-cases -v         (verbose: prints internal block state)
 *
 * DESIGN NOTES (production techniques used here):
 *
 *  - Registry-driven, fork-per-test isolation: every test is registered
 *    once in `tests[]` and run in its own forked child process. If a test
 *    segfaults or aborts unexpectedly, only that child dies -- the parent
 *    sees it via waitpid(), reports it as a failure, and moves on to the
 *    next test instead of losing the rest of the suite's results.
 *  - Nested death-test isolation: test_double_free() additionally forks
 *    *inside itself* via run_isolated(), because that test's whole point
 *    is to confirm a SIGABRT happens on purpose. This nests cleanly inside
 *    the outer per-test fork -- the outer fork protects the suite from
 *    surprise crashes, the inner fork lets one specific test catch an
 *    *intentional* crash and turn it into a normal CHECK() pass/fail.
 *  - Shared-memory counters: because CHECK() may now run inside a forked
 *    child, the pass/fail counters live in an mmap(MAP_SHARED) region so
 *    every generation of child can update them and the parent still sees
 *    the final tally after all children exit.
 *  - White-box inspection: Block is a fully defined (non-opaque) struct
 *    in my-malloc.h, so several tests reach past the malloc/free API and
 *    directly inspect ->payload/->free/->list to confirm internal
 *    invariants (e.g. "did split() actually happen or not").
 *  - Canary/guard-byte checks: payloads are bracketed with sentinel bytes
 *    and re-verified after unrelated allocator activity, to catch heap
 *    metadata bleeding into user data.
 *  - Deterministic PRNG with a printed seed, so a failing fuzz run can be
 *    reproduced exactly by re-running with the same seed.
 *  - Structured pass/fail accounting + non-zero exit code on failure, so
 *    this is CI-friendly (`make test && echo ok`).
 *
 * NOTE ON NAMING: this file calls my_malloc/my_calloc/my_realloc/my_free
 * exclusively -- never the plain malloc/calloc/realloc/free -- since
 * those are now genuinely distinct functions (no symbol shadowing).
 * Bookkeeping arrays in this file (test slot arrays, etc.) intentionally
 * use plain libc malloc/free so the test harness itself doesn't
 * interact with the allocator under test.
 */

#include "my-malloc.h"
#include <stdint.h>
#include <stdarg.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <signal.h>
#include <time.h>

/* ------------------------------------------------------------------ */
/* Tiny test framework                                                 */
/* ------------------------------------------------------------------ */

typedef struct {
    int checks_run;
    int checks_failed;
} SharedCounters;

static SharedCounters *counters;
static int current_test_failed = 0;
static int g_verbose = 0;

static const char *COL_RED = "";
static const char *COL_GREEN = "";
static const char *COL_YELLOW = "";
static const char *COL_RESET = "";

static void enable_color_if_tty(void)
{
    if (isatty(fileno(stdout))) {
        COL_RED = "\033[31m";
        COL_GREEN = "\033[32m";
        COL_YELLOW = "\033[33m";
        COL_RESET = "\033[0m";
    }
}

#define CHECK(cond, msg)                                                    \
    do {                                                                    \
        counters->checks_run++;                                            \
        if (cond) {                                                         \
            printf("  %s[PASS]%s %s\n", COL_GREEN, COL_RESET, msg);         \
        } else {                                                            \
            printf("  %s[FAIL]%s %s  (test-edge-cases.c:%d)\n",             \
                   COL_RED, COL_RESET, msg, __LINE__);                      \
            counters->checks_failed++;                                     \
            current_test_failed++;                                        \
        }                                                                   \
    } while (0)

#define SECTION(name) printf("\n%s== %s ==%s\n", COL_YELLOW, name, COL_RESET)

static void vlog(const char *fmt, ...)
{
    if (!g_verbose) return;
    va_list ap;
    va_start(ap, fmt);
    printf("    | ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
}

/*
 * run_isolated - run @fn in a forked child so an abort()/segfault in it
 * doesn't take down the whole test binary.
 *
 * This is a *nested* fork: it's called from inside a test function that
 * is itself already running inside the outer per-test child forked by
 * main(). That's fine -- shared memory survives across nested forks, and
 * each layer is protecting against a different thing (outer: a surprise
 * crash anywhere; inner: confirming an *intentional* crash happens).
 *
 * Returns:
 *   1 if the child exited normally with status 0
 *   0 if the child exited normally with nonzero status
 *  -1 if the child was killed by a signal (out-param *signo set)
 */
static int run_isolated(void (*fn)(void), int *signo)
{
    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return 0;
    }
    if (pid == 0) {
        /* child */
        fn();
        _exit(0);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    if (WIFSIGNALED(status)) {
        if (signo) *signo = WTERMSIG(status);
        return -1;
    }
    if (signo) *signo = 0;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static Block *hdr_of(void *ptr) { return (Block *)ptr - 1; }

static int is_aligned(const void *ptr)
{
    return ((uintptr_t)ptr % ALIGN) == 0;
}

/* Fill a buffer with a repeating byte pattern derived from a seed. */
static void fill_pattern(unsigned char *buf, size_t n, unsigned char seed)
{
    for (size_t i = 0; i < n; i++) buf[i] = (unsigned char)(seed + i);
}

static int check_pattern(const unsigned char *buf, size_t n, unsigned char seed)
{
    for (size_t i = 0; i < n; i++) {
        if (buf[i] != (unsigned char)(seed + i)) return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* Edge case: NULL handling                                            */
/* ------------------------------------------------------------------ */

static void test_null_handling(void)
{
    SECTION("NULL handling");

    my_free(NULL);
    CHECK(1, "my_free(NULL) is a safe no-op");

    void *p = my_realloc(NULL, 32);
    CHECK(p != NULL, "my_realloc(NULL, 32) behaves like my_malloc(32)");
    my_free(p);

    void *z = my_realloc(NULL, 0);
    CHECK(z == NULL, "my_realloc(NULL, 0) returns NULL (no-op, nothing to free)");
}

/* ------------------------------------------------------------------ */
/* Edge case: double free must be caught, not silently corrupt memory  */
/* ------------------------------------------------------------------ */

static void child_double_free(void)
{
    void *p = my_malloc(64);
    my_free(p);
    my_free(p); /* should abort() the second time */
    /* If we get here, double-free was NOT detected -> exit nonzero
     * so the parent sees this as a failure, not a false pass. */
    _exit(1);
}

static void test_double_free(void)
{
    SECTION("double free detection (isolated child process)");

    int signo = 0;
    int result = run_isolated(child_double_free, &signo);

    CHECK(result == -1 && signo == SIGABRT,
          "double free is detected and the process aborts (SIGABRT)");
    vlog("child exit path: result=%d signo=%d", result, signo);
}

/* ------------------------------------------------------------------ */
/* Edge case: alignment guarantees                                     */
/* ------------------------------------------------------------------ */

static void test_alignment(void)
{
    SECTION("alignment guarantees");

    size_t sizes[] = {1, 2, 3, 7, 8, 9, 15, 16, 17, 63, 100, 255, 4096};
    int all_aligned = 1;

    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        void *p = my_malloc(sizes[i]);
        if (!p || !is_aligned(p)) {
            all_aligned = 0;
            vlog("size=%zu ptr=%p NOT aligned to %zu", sizes[i], p, (size_t)ALIGN);
        }
        my_free(p);
    }
    CHECK(all_aligned, "every returned pointer is aligned to ALIGN (max_align_t)");
}

/* ------------------------------------------------------------------ */
/* Edge case: zero-size interactions                                   */
/* ------------------------------------------------------------------ */

static void test_zero_size_variants(void)
{
    SECTION("zero-size variants (my_malloc/my_calloc)");

    CHECK(my_malloc(0) == NULL, "my_malloc(0) returns NULL");

    void *a = my_calloc(0, 16);
    CHECK(a == NULL, "my_calloc(0, 16) returns NULL (num*size == 0)");

    void *b = my_calloc(16, 0);
    CHECK(b == NULL, "my_calloc(16, 0) returns NULL (num*size == 0)");

    my_free(a);
    my_free(b);
}

/* ------------------------------------------------------------------ */
/* Edge case: split threshold (MIN_FREE_BLOCK boundary)                 */
/* ------------------------------------------------------------------ */

static void test_split_threshold(void)
{
    SECTION("split threshold at MIN_FREE_BLOCK boundary");

    /* Get a free block of a known, STABLE payload by allocating it with
     * an allocated "guard" block immediately after it. Without the guard,
     * freeing p1 would forward-coalesce with whatever free space happens
     * to sit after it (e.g. heap_init's leftover chunk), silently
     * inflating its payload before we ever get to measure it. */
    void *p1 = my_malloc(256);
    void *guard1 = my_malloc(ALIGN); /* blocks forward coalescing of p1 */
    CHECK(p1 != NULL && guard1 != NULL, "my_malloc(256) + guard succeed");
    size_t original_payload = hdr_of(p1)->payload;
    my_free(p1);

    /* Request just small enough that leftover < MIN_FREE_BLOCK.
     * ALIGN_UP granularity is ALIGN bytes, so shrinking by one alignment
     * step (which is less than MIN_FREE_BLOCK on any sane build) should
     * NOT trigger a split -- the allocator should hand back the whole
     * block, oversized, rather than create an unusably tiny free node. */
    size_t tiny_shrink = original_payload - ALIGN;
    void *p2 = my_malloc(tiny_shrink);
    CHECK(p2 == p1, "reused the same block for a slightly smaller request");
    CHECK(hdr_of(p2)->payload == original_payload,
          "block was NOT split when the remainder would be < MIN_FREE_BLOCK "
          "(payload left oversized on purpose)");
    vlog("original_payload=%zu tiny_shrink=%zu actual_payload=%zu MIN_FREE_BLOCK=%zu",
         original_payload, tiny_shrink, hdr_of(p2)->payload, (size_t)MIN_FREE_BLOCK);
    my_free(p2);
    my_free(guard1);

    /* Now request small enough that a split SHOULD happen -- same guard
     * trick to keep the baseline payload stable and measurable. */
    void *p3 = my_malloc(256);
    void *guard2 = my_malloc(ALIGN);
    size_t big_payload = hdr_of(p3)->payload;
    my_free(p3);

    size_t big_shrink = big_payload > (MIN_FREE_BLOCK + ALIGN)
                             ? big_payload - MIN_FREE_BLOCK - ALIGN
                             : ALIGN;
    void *p4 = my_malloc(big_shrink);
    CHECK(p4 == p3, "reused the same block for the split-eligible request");
    CHECK(hdr_of(p4)->payload < big_payload,
          "block WAS split when the remainder is >= MIN_FREE_BLOCK");
    vlog("big_payload=%zu big_shrink=%zu actual_payload=%zu",
         big_payload, big_shrink, hdr_of(p4)->payload);
    my_free(p4);
    my_free(guard2);
}

/* ------------------------------------------------------------------ */
/* Edge case: forward / backward / three-way coalescing                */
/* ------------------------------------------------------------------ */

static void test_forward_coalesce(void)
{
    SECTION("forward coalescing: free(c) then free(b), b absorbs c");

    /* Four adjacent blocks so the merge under test (b absorbing c) is
     * bounded on both sides by still-allocated blocks and can't cascade
     * into 'a' or past 'd'. Each of a/b/c/d is freed exactly once across
     * this function. */
    void *a = my_malloc(64);
    void *b = my_malloc(64);
    void *c = my_malloc(64);
    void *d = my_malloc(64);
    CHECK(a && b && c && d, "four adjacent allocations succeed");

    my_free(c); /* c isolated: neighbors b (alloc) and d (alloc) -> no merge */
    my_free(b); /* b's forward neighbor c is now free -> FORWARD merge:
                   b absorbs c. b's backward neighbor a is allocated ->
                   no backward merge, so this exercises forward-only. */

    void *big = my_malloc(64 + 64 + HEADER_SIZE + FOOTER_SIZE - 8);
    CHECK(big != NULL, "allocation spanning b+c's merged region succeeds");
    CHECK(big == b, "merged region starts at 'b' (forward merge target)");

    my_free(big); /* frees the b+c region (same address as b) */
    my_free(a);
    my_free(d);
}

static void test_backward_coalesce(void)
{
    SECTION("backward coalescing: free(a) then free(b), b merges into a");

    void *a = my_malloc(64);
    void *b = my_malloc(64);
    void *c = my_malloc(64); /* keep allocated so b's forward neighbor is busy */
    CHECK(a && b && c, "three adjacent allocations succeed");

    my_free(a); /* a alone in free list, no free neighbor yet */
    my_free(b); /* b's forward neighbor c is allocated -> no forward merge
                   b's backward neighbor a is free -> BACKWARD merge:
                   coalesce() should return 'a', absorbing b into it */

    CHECK(list_is_linked(&hdr_of(a)->list),
          "'a' remains linked in the free list after absorbing 'b' "
          "(not double-added)");

    void *big = my_malloc(64 + 64 + HEADER_SIZE + FOOTER_SIZE - 8);
    CHECK(big == a, "allocation needing the merged a+b region reuses 'a'");

    my_free(big);
    my_free(c);
}

static void test_three_way_coalesce(void)
{
    SECTION("three-way coalescing: middle free absorbs both neighbors");

    void *a = my_malloc(48);
    void *b = my_malloc(48);
    void *c = my_malloc(48);
    void *d = my_malloc(48); /* trailing allocated block bounds the region */
    CHECK(a && b && c && d, "four adjacent allocations succeed");

    my_free(a); /* isolated free block            */
    my_free(c); /* isolated free block (not adjacent to a) */
    my_free(b); /* b sits between two free blocks:
                   forward merge absorbs c into b, then backward merge
                   absorbs (b+c) into a -> single a+b+c block            */

    void *big = my_malloc(48 * 3 + HEADER_SIZE * 2 + FOOTER_SIZE * 2 - 8);
    CHECK(big == a, "single allocation spans the fully-merged a+b+c region");

    my_free(big);
    my_free(d);
}

/* ------------------------------------------------------------------ */
/* Edge case: allocation bigger than CHUNK_SIZE forces sbrk extension   */
/* ------------------------------------------------------------------ */

static void test_large_allocation_extends_heap(void)
{
    SECTION("allocation larger than CHUNK_SIZE");

    size_t big_size = CHUNK_SIZE * 3;
    void *p = my_malloc(big_size);
    CHECK(p != NULL, "my_malloc(3 * CHUNK_SIZE) succeeds via extra sbrk");

    memset(p, 0x7E, big_size);
    unsigned char *bytes = p;
    int ok = 1;
    for (size_t i = 0; i < big_size; i += 4096) { /* spot-check across pages */
        if (bytes[i] != 0x7E) { ok = 0; break; }
    }
    CHECK(ok, "large block is fully writable across its extended range");

    my_free(p);
}

static void test_many_extensions_stay_consistent(void)
{
    SECTION("repeated heap extension keeps the allocator consistent");

    enum { N = 64 };
    void *ptrs[N];
    int all_ok = 1;
    int all_distinct = 1;

    for (int i = 0; i < N; i++) {
        ptrs[i] = my_malloc(CHUNK_SIZE / 2); /* forces several sbrk growths */
        if (!ptrs[i]) { all_ok = 0; break; }
        memset(ptrs[i], (i & 0xFF), CHUNK_SIZE / 2);
    }
    CHECK(all_ok, "64 half-CHUNK_SIZE allocations all succeed across multiple sbrk growths");

    for (int i = 0; i < N && all_distinct; i++) {
        for (int j = i + 1; j < N; j++) {
            if (ptrs[i] == ptrs[j]) { all_distinct = 0; break; }
        }
    }
    CHECK(all_distinct, "all 64 allocations are at distinct addresses");

    int data_ok = 1;
    for (int i = 0; i < N; i++) {
        unsigned char *b = ptrs[i];
        for (size_t j = 0; j < CHUNK_SIZE / 2; j += 512) {
            if (b[j] != (unsigned char)(i & 0xFF)) { data_ok = 0; break; }
        }
        if (!data_ok) break;
    }
    CHECK(data_ok, "data survives intact across the extended heap region");

    for (int i = 0; i < N; i++) my_free(ptrs[i]);
}

/* ------------------------------------------------------------------ */
/* Edge case: realloc must actually relocate when it can't expand       */
/* in place (neighbor is allocated, not free)                          */
/* ------------------------------------------------------------------ */

static void test_realloc_forced_relocation(void)
{
    SECTION("my_realloc relocates when the next block is allocated");

    void *a = my_malloc(64);
    void *b = my_malloc(64); /* keeps 'a' from expanding forward */
    CHECK(a && b, "two adjacent allocations succeed");

    unsigned char *ca = a;
    fill_pattern(ca, 64, 0x42);

    void *a2 = my_realloc(a, 512);
    CHECK(a2 != NULL, "my_realloc to a larger size succeeds");
    CHECK(a2 != a, "my_realloc relocated the block (couldn't expand in place)");
    CHECK(check_pattern(a2, 64, 0x42), "original 64 bytes preserved after relocation");

    my_free(a2);
    my_free(b);
}

/* ------------------------------------------------------------------ */
/* Edge case: try_expand in-place growth (forward / backward / 3-way)  */
/*                                                                      */
/* These target try_expand() specifically -- the realloc-time sibling  */
/* of coalesce() that can relocate the block via memmove() when it     */
/* merges backward. Correctness here hinges on getting the merged      */
/* block's address, its list membership, and every byte of moved data  */
/* right -- a bug in any of those is silent corruption, not a crash.   */
/* ------------------------------------------------------------------ */

static void test_try_expand_forward_only(void)
{
    SECTION("try_expand: forward-only merge grows in place, no data move");

    void *a = my_malloc(64);
    void *b = my_malloc(64);
    void *c = my_malloc(64);
    CHECK(a && b && c, "three adjacent allocations succeed");

    fill_pattern(b, 64, 0x11);

    my_free(c); /* c becomes free, adjacent to b's forward side */

    void *b2 = my_realloc(b, 64 + 64 + HEADER_SIZE + FOOTER_SIZE - 8);
    CHECK(b2 != NULL, "realloc requesting a b+c-sized region succeeds");
    CHECK(b2 == b, "forward-only merge does not relocate the block");
    CHECK(check_pattern(b2, 64, 0x11), "original bytes preserved (no move needed)");
    CHECK(!list_is_linked(&hdr_of(b2)->list),
          "merged block is allocated -- not sitting on the free list");

    my_free(a);
    my_free(b2);
}

static void test_try_expand_backward_only(void)
{
    SECTION("try_expand: backward-only merge relocates via memmove, data intact");

    /* SZ is deliberately bigger than HEADER_SIZE + FOOTER_SIZE, so the
     * memmove's source and destination ranges genuinely overlap -- this
     * is the exact scenario that requires memmove() instead of memcpy(),
     * since memcpy() on overlapping regions is undefined behavior. */
    enum { SZ = 256 };

    void *a = my_malloc(SZ);
    void *b = my_malloc(SZ);
    void *c = my_malloc(SZ); /* blocks b's forward neighbor, so only the
                                 backward path under test can fire */
    CHECK(a && b && c, "three adjacent allocations succeed");

    unsigned char *bytes = b;
    for (size_t i = 0; i < SZ; i++) bytes[i] = (unsigned char)(i * 7 + 3);

    my_free(a); /* a becomes free, adjacent to b's backward side */

    void *b2 = my_realloc(b, SZ + SZ + HEADER_SIZE + FOOTER_SIZE - 8);
    CHECK(b2 != NULL, "realloc requesting an a+b-sized region succeeds");
    CHECK(b2 == a, "backward-only merge relocates to the absorbed block's address");

    int intact = 1;
    for (size_t i = 0; i < SZ; i++) {
        if (((unsigned char *)b2)[i] != (unsigned char)(i * 7 + 3)) { intact = 0; break; }
    }
    CHECK(intact, "every byte of the original payload survives the memmove, "
                  "including the region that overlapped with the old header");

    CHECK(!list_is_linked(&hdr_of(b2)->list),
          "absorbed-and-grown block is allocated -- not on the free list");
    CHECK(!IS_FREE(hdr_of(b2)), "merged block's free bit is cleared");

    size_t *footer = (size_t *)((char *)(hdr_of(b2) + 1) + hdr_of(b2)->payload);
    CHECK(*footer == hdr_of(b2)->payload,
          "footer matches payload after backward merge (metadata self-consistent)");

    my_free(b2);
    my_free(c);
}

static void test_try_expand_three_way(void)
{
    SECTION("try_expand: three-way merge (prev+curr+next) relocates correctly");

    void *a = my_malloc(48);
    void *b = my_malloc(48);
    void *c = my_malloc(48);
    void *d = my_malloc(48); /* trailing blocker bounds the region */
    CHECK(a && b && c && d, "four adjacent allocations succeed");

    fill_pattern(b, 48, 0x5A);

    my_free(a); /* isolated free block behind b */
    my_free(c); /* isolated free block ahead of b */

    void *big = my_realloc(b, 48 * 3 + HEADER_SIZE * 2 + FOOTER_SIZE * 2 - 8);
    CHECK(big != NULL, "realloc requesting the full a+b+c region succeeds");
    CHECK(big == a, "three-way merge relocates to the leftmost absorbed block");
    CHECK(check_pattern(big, 48, 0x5A), "original bytes preserved through the three-way merge");
    CHECK(!list_is_linked(&hdr_of(big)->list), "merged block is allocated -- not on the free list");

    my_free(big);
    my_free(d);
}

static void test_try_expand_split_after_merge(void)
{
    SECTION("try_expand: leftover after a merge gets split back into the free list");

    void *a = my_malloc(64);
    void *b = my_malloc(64);
    void *c = my_malloc(64);
    CHECK(a && b && c, "three adjacent allocations succeed");

    fill_pattern(b, 64, 0x99);
    my_free(c); /* free neighbor, far bigger than what we'll actually request */

    /* Ask for only slightly more than b's original size. The forward
     * merge finds much more space than needed, so the remainder should
     * be split back out into a reusable free block, not folded wastefully
     * into b2's payload. */
    void *b2 = my_realloc(b, 64 + ALIGN);
    CHECK(b2 == b, "small growth still expands in place via forward merge");
    CHECK(check_pattern(b2, 64, 0x99), "original bytes preserved");
    CHECK(hdr_of(b2)->payload < 64 + 64 + HEADER_SIZE + FOOTER_SIZE,
          "leftover space was split off, not left folded into b2's payload");

    void *reuse = my_malloc(ALIGN);
    CHECK(reuse != NULL, "split-off remainder is available for a fresh allocation");

    my_free(a);
    my_free(b2);
    my_free(reuse);
}

static void test_try_expand_insufficient_falls_back(void)
{
    SECTION("try_expand: merge that still wouldn't fit correctly declines");

    void *a = my_malloc(64);
    void *b = my_malloc(64);
    void *c = my_malloc(64);
    void *d = my_malloc(64); /* trailing blocker */
    CHECK(a && b && c && d, "four adjacent allocations succeed");

    fill_pattern(b, 64, 0x33);
    my_free(a);
    my_free(c); /* both neighbors free, but nowhere near enough combined --
                    try_expand must report "not enough", not hand back an
                    undersized block */

    void *big = my_realloc(b, 8192);
    CHECK(big != NULL, "realloc for a size far beyond any local merge still succeeds "
                        "(falls back to malloc+copy+free)");
    CHECK(check_pattern(big, 64, 0x33), "original bytes preserved through the fallback path");

    my_free(big);
    my_free(d);
}

static void test_try_expand_no_prev_at_heap_start(void)
{
    SECTION("try_expand: no out-of-bounds read when there is no previous block");

    /* This runs as the first allocator call in its own forked child, which
     * inherits the heap exactly as heap_init() left it -- so this malloc
     * carves off the very start of the initial free block, meaning the
     * returned block sits at heap_start with nothing behind it. The
     * backward-merge check in try_expand walks to (char*)curr - FOOTER_SIZE
     * to read a footer that, for this block, does not exist; it must be
     * guarded by the heap_start comparison rather than assumed safe.
     * Build this binary with -fsanitize=address for a hard guarantee this
     * isn't quietly reading before the mapped region. */
    void *p = my_malloc(64);
    void *guard = my_malloc(64); /* blocks the forward path too, so only the
                                     backward boundary check is exercised */
    CHECK(p && guard, "first two allocations of the heap succeed");

    fill_pattern(p, 64, 0x7A);

    void *p2 = my_realloc(p, 64 + 64 + HEADER_SIZE + FOOTER_SIZE - 8);
    CHECK(p2 != NULL, "realloc succeeds even though no merge is possible "
                       "(no block exists before heap_start, next is allocated)");
    CHECK(check_pattern(p2, 64, 0x7A), "original bytes preserved regardless of path taken");

    my_free(p2);
    my_free(guard);
}

/* ------------------------------------------------------------------ */
/* Edge case: canary / guard bytes survive unrelated allocator churn    */
/* ------------------------------------------------------------------ */

static void test_canary_survival(void)
{
    SECTION("canary bytes at payload edges survive unrelated churn");

    enum { N = 16, SZ = 40 };
    void *ptrs[N];

    for (int i = 0; i < N; i++) {
        ptrs[i] = my_malloc(SZ);
        CHECK(ptrs[i] != NULL, "canary block allocated");
        unsigned char *b = ptrs[i];
        b[0] = (unsigned char)(0xC0 + i); /* front canary */
        b[SZ - 1] = (unsigned char)(0xD0 + i); /* back canary */
    }

    /* Churn: allocate/free a bunch of unrelated blocks of varying sizes
     * in between, the kind of activity most likely to reveal an
     * allocator that lets metadata bleed into neighboring payloads. */
    for (int round = 0; round < 8; round++) {
        void *tmp[8];
        for (int i = 0; i < 8; i++) {
            tmp[i] = my_malloc((size_t)(16 + round * 8 + i * 4));
            if (tmp[i]) memset(tmp[i], 0xFF, 16 + round * 8 + i * 4);
        }
        for (int i = 0; i < 8; i++) my_free(tmp[i]);
    }

    int intact = 1;
    for (int i = 0; i < N; i++) {
        unsigned char *b = ptrs[i];
        if (b[0] != (unsigned char)(0xC0 + i) || b[SZ - 1] != (unsigned char)(0xD0 + i)) {
            intact = 0;
            vlog("canary corrupted for block %d", i);
        }
    }
    CHECK(intact, "all front/back canaries intact after unrelated allocator churn");

    for (int i = 0; i < N; i++) my_free(ptrs[i]);
}

/* ------------------------------------------------------------------ */
/* Fuzz-style stress test with checksummed data and mixed operations   */
/* ------------------------------------------------------------------ */

typedef struct {
    void *ptr;
    size_t size;
    unsigned char seed;
    int live;
} tracked_t;

static void test_fuzz_mixed_ops_seeded(unsigned seed)
{
    SECTION("fuzz: mixed my_malloc/my_realloc/my_free with checksum verification");
    printf("  (seed = %u -- rerun with this seed to reproduce exactly)\n", seed);

    srand(seed);

    enum { SLOTS = 128, OPS = 2000 };
    static tracked_t slots[SLOTS];
    memset(slots, 0, sizeof(slots));

    int corruption_found = 0;
    int op_failures = 0;

    for (int op = 0; op < OPS; op++) {
        int idx = rand() % SLOTS;
        int action = rand() % 4; /* 0=alloc,1=free,2=realloc,3=alloc */

        if (slots[idx].live) {
            /* verify before mutating/removing */
            if (!check_pattern(slots[idx].ptr, slots[idx].size, slots[idx].seed)) {
                corruption_found = 1;
            }
        }

        switch (action) {
        case 1: /* free */
            if (slots[idx].live) {
                my_free(slots[idx].ptr);
                slots[idx].live = 0;
            }
            break;

        case 2: /* realloc (or alloc if empty) */
            if (slots[idx].live) {
                size_t newsize = (size_t)(rand() % 2000) + 1;
                void *np = my_realloc(slots[idx].ptr, newsize);
                if (!np) { op_failures++; break; }
                slots[idx].ptr = np;
                slots[idx].size = newsize;
                slots[idx].seed = (unsigned char)rand();
                fill_pattern(np, newsize, slots[idx].seed);
                break;
            }
            /* fallthrough to alloc if slot was empty */
            /* fall through */
        default: { /* 0, 3, and realloc-fallthrough: fresh allocation */
            if (slots[idx].live) my_free(slots[idx].ptr);
            size_t sz = (size_t)(rand() % 2000) + 1;
            void *p = my_malloc(sz);
            if (!p) { op_failures++; slots[idx].live = 0; break; }
            unsigned char sd = (unsigned char)rand();
            fill_pattern(p, sz, sd);
            slots[idx] = (tracked_t){.ptr = p, .size = sz, .seed = sd, .live = 1};
            break;
        }
        }
    }

    /* final integrity pass over everything still live */
    for (int i = 0; i < SLOTS; i++) {
        if (slots[i].live) {
            if (!check_pattern(slots[i].ptr, slots[i].size, slots[i].seed)) {
                corruption_found = 1;
            }
        }
    }

    CHECK(!corruption_found, "no data corruption across 2000 randomized ops");
    CHECK(op_failures == 0, "no unexpected allocation failures during fuzz run");

    /* Design invariant: allocated blocks are never linked into the free
     * list. If try_expand() or split() ever forgets to unlink an absorbed
     * or trimmed block, this is what catches it -- corruption_found above
     * only catches bad *data*, this catches bad *metadata*. */
    int invariant_ok = 1;
    for (int i = 0; i < SLOTS; i++) {
        if (slots[i].live && list_is_linked(&hdr_of(slots[i].ptr)->list)) {
            invariant_ok = 0;
        }
    }
    CHECK(invariant_ok, "every live allocation is off the free list "
                        "(allocated-never-on-list invariant holds)");

    for (int i = 0; i < SLOTS; i++) {
        if (slots[i].live) my_free(slots[i].ptr);
    }
}

/* TestCase.fn is void(*)(void), but the fuzz test wants a seed. This thin
 * wrapper draws the seed itself (from the clock) so it still fits the
 * registry's uniform signature -- the seed is printed either way, so a
 * failing run is still exactly reproducible. */
static void test_fuzz_mixed_ops(void)
{
    test_fuzz_mixed_ops_seeded((unsigned)time(NULL));
}

/* ------------------------------------------------------------------ */
/* Registry -- add new tests here, nowhere else.                       */
/* ------------------------------------------------------------------ */

typedef void (*test_fn)(void);

typedef struct {
    const char *name;
    test_fn fn;
} TestCase;

static const TestCase tests[] = {
    {"NULL handling",                        test_null_handling},
    {"double free detection",                test_double_free},
    {"alignment guarantees",                 test_alignment},
    {"zero-size variants",                   test_zero_size_variants},
    {"split threshold",                      test_split_threshold},
    {"forward coalescing",                   test_forward_coalesce},
    {"backward coalescing",                  test_backward_coalesce},
    {"three-way coalescing",                 test_three_way_coalesce},
    {"large allocation extends heap",        test_large_allocation_extends_heap},
    {"many extensions stay consistent",      test_many_extensions_stay_consistent},
    {"realloc forced relocation",            test_realloc_forced_relocation},
    {"try_expand forward-only merge",        test_try_expand_forward_only},
    {"try_expand backward-only merge",       test_try_expand_backward_only},
    {"try_expand three-way merge",           test_try_expand_three_way},
    {"try_expand split after merge",         test_try_expand_split_after_merge},
    {"try_expand insufficient merge falls back", test_try_expand_insufficient_falls_back},
    {"try_expand no-prev heap-start boundary", test_try_expand_no_prev_at_heap_start},
    {"canary survival",                      test_canary_survival},
    {"fuzz: mixed ops",                      test_fuzz_mixed_ops},
};

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
            g_verbose = 1;
        }
    }
    enable_color_if_tty();

    /* Unbuffered stdout matters here: every test's child exits via
     * _exit(), which skips flushing stdio buffers. Buffered output
     * written just before a crash would otherwise vanish silently. */
    setvbuf(stdout, NULL, _IONBF, 0);

    heap_init();

    /* Shared memory so every forked child's CHECK() results (including
     * grandchildren, from the nested double-free test) are visible back
     * in the parent once each child exits. */
    counters = mmap(NULL, sizeof(SharedCounters), PROT_READ | PROT_WRITE,
                     MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    counters->checks_run = 0;
    counters->checks_failed = 0;

    size_t num_tests = sizeof(tests) / sizeof(tests[0]);
    int suites_failed = 0;
    int suites_crashed = 0;

    for (size_t i = 0; i < num_tests; i++) {
        fflush(stdout); /* flush before fork so the child doesn't inherit a stale buffer */

        pid_t pid = fork();
        if (pid == 0) {
            /* child: run exactly one test, then report pass/fail via exit code */
            current_test_failed = 0;
            tests[i].fn();
            _exit(current_test_failed ? 1 : 0);
        }

        int status;
        waitpid(pid, &status, 0);

        if (WIFSIGNALED(status)) {
            printf("  %s[CRASH]%s '%s' terminated by signal %d (%s)\n",
                   COL_RED, COL_RESET, tests[i].name,
                   WTERMSIG(status), strsignal(WTERMSIG(status)));
            suites_crashed++;
            suites_failed++;
        } else if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
            suites_failed++;
        }
    }

    printf("\n=====================================\n");
    if (counters->checks_failed == 0 && suites_failed == 0) {
        printf("%s%d/%d checks passed across %zu test suites%s\n",
               COL_GREEN, counters->checks_run, counters->checks_run, num_tests, COL_RESET);
    } else {
        printf("%s%d/%d checks passed across %zu test suites%s\n",
               COL_RED, counters->checks_run - counters->checks_failed,
               counters->checks_run, num_tests, COL_RESET);
    }
    printf("%d suite%s failed", suites_failed, suites_failed == 1 ? "" : "s");
    if (suites_crashed) {
        printf(" (%d crashed the test process)", suites_crashed);
    }
    printf("\n=====================================\n");

    return suites_failed == 0 ? 0 : 1;
}