/*
 * test-edge-cases.c - edge case / robustness suite for my-malloc
 *
 * Build:
 *   gcc -Wall -Wextra -std=c11 -Iinclude -g \
 *       -o test/test-edge-cases src/my-malloc.c test/test-edge-cases.c
 *
 * Build with sanitizers (now safe to use, since the allocator no longer
 * shadows libc's malloc/calloc/realloc/free symbol names):
 *   gcc -Wall -Wextra -std=c11 -Iinclude -fsanitize=address,undefined -g \
 *       -o test/test-edge-cases src/my-malloc.c test/test-edge-cases.c
 *
 * Run:
 *   ./test/test-edge-cases            (normal output)
 *   ./test/test-edge-cases -v         (verbose: prints internal block state)
 *
 * DESIGN NOTES (production techniques used here):
 *
 *  - Crash isolation: tests that are *expected* to abort or crash the
 *    process (double free, etc.) are run in a forked child via
 *    run_isolated(). The parent inspects the exit status/signal instead
 *    of dying with the child. This mirrors how real test frameworks
 *    implement "death tests" (e.g. GoogleTest's ASSERT_DEATH) so one
 *    intentionally-fatal case can't take the whole suite down.
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
#include <signal.h>
#include <time.h>

/* ------------------------------------------------------------------ */
/* Tiny test framework                                                 */
/* ------------------------------------------------------------------ */

static int g_checks_run = 0;
static int g_checks_failed = 0;
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
        g_checks_run++;                                                     \
        if (cond) {                                                         \
            printf("  %s[PASS]%s %s\n", COL_GREEN, COL_RESET, msg);         \
        } else {                                                            \
            printf("  %s[FAIL]%s %s  (test-edge-cases.c:%d)\n",             \
                   COL_RED, COL_RESET, msg, __LINE__);                      \
            g_checks_failed++;                                              \
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

static void test_fuzz_mixed_ops(unsigned seed)
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

    for (int i = 0; i < SLOTS; i++) {
        if (slots[i].live) my_free(slots[i].ptr);
    }
}

/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
            g_verbose = 1;
        }
    }
    enable_color_if_tty();

    heap_init();

    test_null_handling();
    test_double_free();
    test_alignment();
    test_zero_size_variants();
    test_split_threshold();
    test_forward_coalesce();
    test_backward_coalesce();
    test_three_way_coalesce();
    test_large_allocation_extends_heap();
    test_many_extensions_stay_consistent();
    test_realloc_forced_relocation();
    test_canary_survival();
    test_fuzz_mixed_ops((unsigned)time(NULL));

    printf("\n=====================================\n");
    if (g_checks_failed == 0) {
        printf("%s%d/%d checks passed%s\n", COL_GREEN, g_checks_run, g_checks_run, COL_RESET);
    } else {
        printf("%s%d/%d checks passed (%d FAILED)%s\n", COL_RED,
               g_checks_run - g_checks_failed, g_checks_run, g_checks_failed, COL_RESET);
    }
    printf("=====================================\n");

    return g_checks_failed == 0 ? 0 : 1;
}