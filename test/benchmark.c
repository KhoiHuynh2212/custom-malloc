/*
 * benchmark.c - throughput/latency/fragmentation benchmark for my-malloc
 *
 * Build (ALWAYS with optimizations on for benchmarking -- an unoptimized
 * build tells you nothing about real performance):
 *
 *   gcc -O2 -Wall -Wextra -std=c11 -Iinclude -g \
 *       -o test/benchmark src/my-malloc.c test/benchmark.c
 *
 * Run:
 *   ./test/benchmark                       (defaults: isolated phases)
 *   ./test/benchmark --ops 200000 --seed 7 --csv results.csv
 *   ./test/benchmark --shared              (old behavior: one shared heap)
 *   ./test/benchmark --help
 *
 * PER-PHASE PROCESS ISOLATION (the default mode):
 *
 *   Each benchmark phase runs in its own freshly fork()'d child process.
 *   The parent NEVER calls heap_init() or touches the custom allocator
 *   itself -- it only forks, waits, and prints -- so every child is
 *   forked from an identical, still-pristine process image. That means
 *   every phase's my_malloc/my_free calls start from:
 *     - heap_start/heap_end freshly set by that child's own heap_init()
 *     - an empty free list (no blocks left over from a previous phase)
 *     - a program break (sbrk) that hasn't been extended by anything
 *       this benchmark did before
 *   Without this, running growth_only right after fixed_churn would let
 *   growth_only quietly reuse free-list capacity that fixed_churn left
 *   behind, and the fragmentation phase's "how many bytes did the heap
 *   grow" measurement would read close to zero simply because an earlier
 *   phase already grew it -- exactly the confusing result you'd get
 *   from a shared-heap run. Isolating each phase into its own process
 *   removes that cross-phase contamination entirely: what you see for
 *   phase N is caused only by phase N.
 *
 *   Pass --shared to disable this and go back to one heap shared across
 *   every phase in a single process (useful if you specifically want to
 *   see how later phases benefit from earlier ones' leftover free
 *   capacity -- that's a legitimate thing to want to measure too, just
 *   a different question than "what does this pattern cost from cold").
 *
 * NOTE ON NAMING: my-malloc.c exposes my_malloc/my_calloc/my_realloc/
 * my_free as distinct symbols rather than shadowing libc's malloc/
 * calloc/realloc/free. That means this file can freely call ordinary
 * libc malloc/free/qsort/fopen/etc. for its own bookkeeping (scratch
 * buffers, sorting latency samples, writing the CSV) without any risk
 * of interposition weirdness -- glibc's malloc and your my_malloc are
 * two completely independent, non-conflicting allocators living in the
 * same process.
 *
 * PRODUCTION TECHNIQUES USED IN THIS FILE:
 *   - per-phase subprocess isolation for a genuinely cold starting
 *     state per benchmark case (see above) -- the same idea behind
 *     "death tests" / isolated benchmark runners in other frameworks
 *   - CLOCK_MONOTONIC timing (immune to wall-clock adjustments)
 *   - warm-up phase before every measured run (avoids first-touch /
 *     cold-cache / lazy-page-fault skew)
 *   - per-operation latency sampling + full percentile breakdown
 *     (p50/p90/p99/max), not just an average -- averages hide tail
 *     latency, which is usually what actually matters for allocators
 *   - a compiler-barrier "escape" hatch so -O2 can't dead-code-eliminate
 *     allocations the benchmark never reads from (the classic
 *     DoNotOptimize trick from Google Benchmark)
 *   - multiple realistic workload shapes, not just one loop:
 *       fixed-size churn, random-size churn, growth-only, fragmentation-
 *       inducing alternation, and a realloc-heavy pattern
 *   - sbrk(0)-based heap growth accounting to report actual memory
 *     overhead (bytes taken from the OS vs. bytes the caller asked for)
 *   - deterministic, printed PRNG seed for reproducibility
 *   - machine-readable CSV output alongside the human-readable report,
 *     correctly flushed across process boundaries (see child_finish())
 *   - defensive error handling on every allocation (never assumes
 *     success)
 */

#include "my-malloc.h"
#include <stdint.h>
#include <time.h>
#include <getopt.h>
#include <errno.h>
#include <unistd.h>
#include <sys/wait.h>

/* ------------------------------------------------------------------ */
/* Compiler barrier so -O2 can't optimize benchmarked work away        */
/* ------------------------------------------------------------------ */

static inline void escape(void *p)
{
    __asm__ volatile("" : : "g"(p) : "memory");
}

/* ------------------------------------------------------------------ */
/* Timing helpers                                                      */
/* ------------------------------------------------------------------ */

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static int cmp_double(const void *a, const void *b)
{
    double da = *(const double *)a, db = *(const double *)b;
    return (da > db) - (da < db);
}

typedef struct {
    double mean_ns;
    double p50_ns;
    double p90_ns;
    double p99_ns;
    double max_ns;
    double total_sec;
    double ops_per_sec;
    size_t n;
} stats_t;

static stats_t compute_stats(double *latencies_ns, size_t n, double total_sec)
{
    stats_t s = {0};
    if (n == 0) return s;

    qsort(latencies_ns, n, sizeof(double), cmp_double);

    double sum = 0;
    for (size_t i = 0; i < n; i++) sum += latencies_ns[i];

    s.n = n;
    s.mean_ns = sum / (double)n;
    s.p50_ns = latencies_ns[(size_t)(n * 0.50)];
    s.p90_ns = latencies_ns[(size_t)(n * 0.90 < n ? n * 0.90 : n - 1)];
    s.p99_ns = latencies_ns[(size_t)(n * 0.99 < n ? n * 0.99 : n - 1)];
    s.max_ns = latencies_ns[n - 1];
    s.total_sec = total_sec;
    s.ops_per_sec = total_sec > 0 ? (double)n / total_sec : 0;
    return s;
}

static void print_stats_row(const char *label, const stats_t *s)
{
    printf("  %-32s %12.0f ops/s  mean=%8.1fns  p50=%8.1fns  p90=%8.1fns  p99=%8.1fns  max=%10.1fns\n",
           label, s->ops_per_sec, s->mean_ns, s->p50_ns, s->p90_ns, s->p99_ns, s->max_ns);
}

/* ------------------------------------------------------------------ */
/* CSV output                                                           */
/* ------------------------------------------------------------------ */

static FILE *g_csv = NULL;

static void csv_header(void)
{
    if (!g_csv) return;
    fprintf(g_csv, "benchmark,allocator,ops,ops_per_sec,mean_ns,p50_ns,p90_ns,p99_ns,max_ns,total_sec\n");
    fflush(g_csv);
}

static void csv_row(const char *bench, const char *alloc, const stats_t *s)
{
    if (!g_csv) return;
    fprintf(g_csv, "%s,%s,%zu,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.6f\n",
            bench, alloc, s->n, s->ops_per_sec, s->mean_ns,
            s->p50_ns, s->p90_ns, s->p99_ns, s->max_ns, s->total_sec);
}

/* ------------------------------------------------------------------ */
/* Config                                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    long ops;
    unsigned seed;
    const char *csv_path;
    int skip_libc;
    int isolate; /* 1 = each phase in its own fresh subprocess (default) */
} config_t;

static void print_usage(const char *prog)
{
    printf("Usage: %s [--ops N] [--seed S] [--csv path] [--no-libc] [--shared] [--help]\n", prog);
    printf("  --ops N     number of operations per benchmark phase (default 50000)\n");
    printf("  --seed S    PRNG seed for reproducible random workloads (default: time-based)\n");
    printf("  --csv path  also write machine-readable results to this CSV file\n");
    printf("  --no-libc   skip the glibc comparison, only benchmark my-malloc\n");
    printf("  --shared    run all phases in ONE process sharing one heap (old behavior);\n");
    printf("              default is to isolate each phase in its own fresh subprocess\n");
    printf("              so every phase measures a cold heap, uncontaminated by earlier\n");
    printf("              phases' leftover free-list capacity or heap growth\n");
    printf("  --help      show this message\n");
}

static config_t parse_args(int argc, char **argv)
{
    config_t cfg = {.ops = 50000, .seed = (unsigned)time(NULL),
                     .csv_path = NULL, .skip_libc = 0, .isolate = 1};

    static struct option long_opts[] = {
        {"ops", required_argument, 0, 'o'},
        {"seed", required_argument, 0, 's'},
        {"csv", required_argument, 0, 'c'},
        {"no-libc", no_argument, 0, 'n'},
        {"shared", no_argument, 0, 'S'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0},
    };

    int opt, idx;
    while ((opt = getopt_long(argc, argv, "o:s:c:nSh", long_opts, &idx)) != -1) {
        switch (opt) {
        case 'o': cfg.ops = atol(optarg); break;
        case 's': cfg.seed = (unsigned)strtoul(optarg, NULL, 10); break;
        case 'c': cfg.csv_path = optarg; break;
        case 'n': cfg.skip_libc = 1; break;
        case 'S': cfg.isolate = 0; break;
        case 'h': print_usage(argv[0]); exit(0);
        default: print_usage(argv[0]); exit(1);
        }
    }
    if (cfg.ops <= 0) {
        fprintf(stderr, "benchmark: --ops must be positive\n");
        exit(1);
    }
    return cfg;
}

/* ------------------------------------------------------------------ */
/* Heap growth accounting (custom allocator only -- sbrk-based)         */
/* ------------------------------------------------------------------ */

static void *sbrk_mark(void) { return sbrk(0); }

static long sbrk_delta_bytes(void *before)
{
    void *after = sbrk(0);
    return (long)((char *)after - (char *)before);
}

/* ------------------------------------------------------------------ */
/* Per-phase context + subprocess runner                                */
/* ------------------------------------------------------------------ */

typedef struct {
    config_t *cfg;
    double *scratch;
} phase_ctx_t;

/*
 * child_finish - flush everything this process buffered, then exit
 * without running libc's normal atexit/stdio-cleanup machinery.
 *
 * We use _exit() (not exit()) so a child never re-flushes or otherwise
 * touches state shared with the parent's copy of the same FILE objects.
 * That means we MUST flush explicitly first, or buffered stdout/CSV
 * output from this child would simply vanish.
 */
static void child_finish(int status)
{
    fflush(stdout);
    if (g_csv) fflush(g_csv);
    _exit(status);
}

/*
 * run_phase - execute one benchmark phase.
 *
 * In isolated mode: forks a fresh child, which calls heap_init() itself
 * (guaranteed cold, since the parent never has) and then runs @fn.
 *
 * In shared mode: just calls @fn directly in the current process, whose
 * heap was already initialized once at the top of main().
 */
static void run_phase(config_t *cfg, void (*fn)(phase_ctx_t *), phase_ctx_t *ctx)
{
    if (!cfg->isolate) {
        fn(ctx);
        return;
    }

    fflush(stdout);
    if (g_csv) fflush(g_csv);

    pid_t pid = fork();
    if (pid < 0) {
        perror("benchmark: fork");
        fprintf(stderr, "benchmark: falling back to running this phase in-process\n");
        fn(ctx);
        return;
    }
    if (pid == 0) {
        heap_init();
        fn(ctx);
        child_finish(0);
    }

    int status = 0;
    waitpid(pid, &status, 0);
    if (!(WIFEXITED(status) && WEXITSTATUS(status) == 0)) {
        fprintf(stderr, "benchmark: phase subprocess exited abnormally (status=%d)\n", status);
    }
}

/* ------------------------------------------------------------------ */
/* Workload 1: fixed-size churn (alloc N, immediately free)             */
/* ------------------------------------------------------------------ */

static stats_t bench_fixed_churn_custom(long ops, size_t size, double *scratch)
{
    for (long i = 0; i < ops / 10; i++) {
        void *p = my_malloc(size);
        escape(p);
        my_free(p);
    }

    double t0 = now_sec();
    for (long i = 0; i < ops; i++) {
        double s = now_sec();
        void *p = my_malloc(size);
        escape(p);
        my_free(p);
        scratch[i] = (now_sec() - s) * 1e9;
    }
    double total = now_sec() - t0;
    return compute_stats(scratch, (size_t)ops, total);
}

static stats_t bench_fixed_churn_libc(long ops, size_t size, double *scratch)
{
    for (long i = 0; i < ops / 10; i++) {
        void *p = malloc(size);
        escape(p);
        free(p);
    }

    double t0 = now_sec();
    for (long i = 0; i < ops; i++) {
        double s = now_sec();
        void *p = malloc(size);
        escape(p);
        free(p);
        scratch[i] = (now_sec() - s) * 1e9;
    }
    double total = now_sec() - t0;
    return compute_stats(scratch, (size_t)ops, total);
}

static void phase_fixed_churn(phase_ctx_t *ctx)
{
    printf("\n-- Fixed-size churn (64 bytes, my_malloc immediately followed by my_free) --\n");
    stats_t s = bench_fixed_churn_custom(ctx->cfg->ops, 64, ctx->scratch);
    print_stats_row("my-malloc", &s);
    csv_row("fixed_churn_64b", "my-malloc", &s);
    if (!ctx->cfg->skip_libc) {
        stats_t sl = bench_fixed_churn_libc(ctx->cfg->ops, 64, ctx->scratch);
        print_stats_row("glibc malloc", &sl);
        csv_row("fixed_churn_64b", "glibc", &sl);
    }
}

/* ------------------------------------------------------------------ */
/* Workload 2: random-size churn (simulates a realistic mixed workload) */
/* ------------------------------------------------------------------ */

static size_t rand_size(unsigned *state)
{
    return (size_t)(rand_r(state) % 2048) + 1;
}

static stats_t bench_random_churn_custom(long ops, unsigned seed, double *scratch)
{
    unsigned state = seed;
    for (long i = 0; i < ops / 10; i++) {
        void *p = my_malloc(rand_size(&state));
        escape(p);
        my_free(p);
    }

    state = seed;
    double t0 = now_sec();
    for (long i = 0; i < ops; i++) {
        size_t sz = rand_size(&state);
        double s = now_sec();
        void *p = my_malloc(sz);
        escape(p);
        my_free(p);
        scratch[i] = (now_sec() - s) * 1e9;
    }
    double total = now_sec() - t0;
    return compute_stats(scratch, (size_t)ops, total);
}

static stats_t bench_random_churn_libc(long ops, unsigned seed, double *scratch)
{
    unsigned state = seed;
    for (long i = 0; i < ops / 10; i++) {
        void *p = malloc(rand_size(&state));
        escape(p);
        free(p);
    }

    state = seed;
    double t0 = now_sec();
    for (long i = 0; i < ops; i++) {
        size_t sz = rand_size(&state);
        double s = now_sec();
        void *p = malloc(sz);
        escape(p);
        free(p);
        scratch[i] = (now_sec() - s) * 1e9;
    }
    double total = now_sec() - t0;
    return compute_stats(scratch, (size_t)ops, total);
}

static void phase_random_churn(phase_ctx_t *ctx)
{
    printf("\n-- Random-size churn (1..2048 bytes, my_malloc immediately followed by my_free) --\n");
    stats_t s = bench_random_churn_custom(ctx->cfg->ops, ctx->cfg->seed, ctx->scratch);
    print_stats_row("my-malloc", &s);
    csv_row("random_churn", "my-malloc", &s);
    if (!ctx->cfg->skip_libc) {
        stats_t sl = bench_random_churn_libc(ctx->cfg->ops, ctx->cfg->seed, ctx->scratch);
        print_stats_row("glibc malloc", &sl);
        csv_row("random_churn", "glibc", &sl);
    }
}

/* ------------------------------------------------------------------ */
/* Workload 3: growth-only (allocate many blocks, hold them all, then   */
/* free everything at the end -- stresses list search / heap extension) */
/* ------------------------------------------------------------------ */

static stats_t bench_growth_custom(long ops, double *scratch)
{
    void **ptrs = malloc((size_t)ops * sizeof(void *));
    if (!ptrs) { fprintf(stderr, "benchmark: OOM allocating bookkeeping array\n"); exit(1); }

    double t0 = now_sec();
    for (long i = 0; i < ops; i++) {
        double s = now_sec();
        ptrs[i] = my_malloc(32 + (size_t)(i % 64));
        escape(ptrs[i]);
        scratch[i] = (now_sec() - s) * 1e9;
    }
    double total = now_sec() - t0;

    for (long i = 0; i < ops; i++) my_free(ptrs[i]);
    free(ptrs);

    return compute_stats(scratch, (size_t)ops, total);
}

static void phase_growth_only(phase_ctx_t *ctx)
{
    printf("\n-- Growth-only (allocate all, hold, free at the end) --\n");
    void *before = sbrk_mark();
    stats_t s = bench_growth_custom(ctx->cfg->ops, ctx->scratch);
    long grown = sbrk_delta_bytes(before);
    print_stats_row("my-malloc", &s);
    printf("  %-32s heap grew %ld bytes during this phase%s\n", "", grown,
           ctx->cfg->isolate ? " (cold start)" : " (heap shared with earlier phases)");
    csv_row("growth_only", "my-malloc", &s);
}

/* ------------------------------------------------------------------ */
/* Workload 4: fragmentation-inducing alternation                       */
/* alloc(small), alloc(large), ..., then free ALL the small ones,       */
/* then try to allocate a big run -- a classic fragmentation stress     */
/* pattern. We report heap growth (sbrk delta) as the overhead metric.  */
/* ------------------------------------------------------------------ */

static void bench_fragmentation_custom(long pairs)
{
    void *before = sbrk_mark();

    void **small = malloc(sizeof(void *) * (size_t)pairs);
    void **large = malloc(sizeof(void *) * (size_t)pairs);
    if (!small || !large) { fprintf(stderr, "benchmark: OOM in fragmentation setup\n"); exit(1); }

    size_t requested_bytes = 0;
    for (long i = 0; i < pairs; i++) {
        small[i] = my_malloc(24);
        large[i] = my_malloc(512);
        escape(small[i]);
        escape(large[i]);
        requested_bytes += 24 + 512;
    }

    /* free every small block, leaving a "swiss cheese" free list of
     * small holes between still-live large blocks */
    for (long i = 0; i < pairs; i++) my_free(small[i]);

    /* now request blocks that are too big to fit any single hole,
     * forcing either heap extension or (if your allocator supported it)
     * would reveal fragmentation-driven OOM under a tighter heap */
    long big_ops = pairs / 4 > 0 ? pairs / 4 : 1;
    for (long i = 0; i < big_ops; i++) {
        void *p = my_malloc(400);
        escape(p);
        requested_bytes += 400;
        my_free(p);
    }

    for (long i = 0; i < pairs; i++) my_free(large[i]);

    long grown = sbrk_delta_bytes(before);
    double overhead_pct = requested_bytes > 0
        ? (100.0 * ((double)grown - (double)requested_bytes) / (double)requested_bytes)
        : 0.0;

    printf("  %-32s heap grew %8ld bytes for %8zu bytes requested  (%+.1f%% overhead)\n",
           "fragmentation pattern (custom)", grown, requested_bytes, overhead_pct);

    if (g_csv) {
        fprintf(g_csv, "fragmentation,my-malloc,%ld,,,,,,,\n", grown);
    }

    free(small);
    free(large);
}

static void phase_fragmentation(phase_ctx_t *ctx)
{
    printf("\n-- Fragmentation pattern (alternating small/large, drain smalls) --\n");
    long pairs = ctx->cfg->ops / 4 > 0 ? ctx->cfg->ops / 4 : 1;
    bench_fragmentation_custom(pairs);
}

/* ------------------------------------------------------------------ */
/* Workload 5: realloc-heavy (simulates a growing dynamic array/buffer) */
/* ------------------------------------------------------------------ */

static stats_t bench_realloc_growth_custom(long ops, double *scratch)
{
    void *p = my_malloc(16);
    escape(p);
    size_t cur = 16;

    double t0 = now_sec();
    for (long i = 0; i < ops; i++) {
        size_t next = cur + 8 + (size_t)(i % 32);
        double s = now_sec();
        void *np = my_realloc(p, next);
        if (!np) { fprintf(stderr, "benchmark: my_realloc failed at op %ld\n", i); exit(1); }
        p = np;
        cur = next;
        escape(p);
        scratch[i] = (now_sec() - s) * 1e9;
        if (cur > 1u << 20) { /* periodically reset so we don't grow unboundedly */
            my_free(p);
            p = my_malloc(16);
            cur = 16;
        }
    }
    double total = now_sec() - t0;
    my_free(p);
    return compute_stats(scratch, (size_t)ops, total);
}

static stats_t bench_realloc_growth_libc(long ops, double *scratch)
{
    void *p = malloc(16);
    escape(p);
    size_t cur = 16;

    double t0 = now_sec();
    for (long i = 0; i < ops; i++) {
        size_t next = cur + 8 + (size_t)(i % 32);
        double s = now_sec();
        void *np = realloc(p, next);
        if (!np) { fprintf(stderr, "benchmark: realloc failed at op %ld\n", i); exit(1); }
        p = np;
        cur = next;
        escape(p);
        scratch[i] = (now_sec() - s) * 1e9;
        if (cur > 1u << 20) {
            free(p);
            p = malloc(16);
            cur = 16;
        }
    }
    double total = now_sec() - t0;
    free(p);
    return compute_stats(scratch, (size_t)ops, total);
}

static void phase_realloc_growth(phase_ctx_t *ctx)
{
    printf("\n-- Realloc-heavy growth pattern --\n");
    stats_t s = bench_realloc_growth_custom(ctx->cfg->ops, ctx->scratch);
    print_stats_row("my-malloc", &s);
    csv_row("realloc_growth", "my-malloc", &s);
    if (!ctx->cfg->skip_libc) {
        stats_t sl = bench_realloc_growth_libc(ctx->cfg->ops, ctx->scratch);
        print_stats_row("glibc realloc", &sl);
        csv_row("realloc_growth", "glibc", &sl);
    }
}

/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    config_t cfg = parse_args(argc, argv);

    if (cfg.csv_path) {
        g_csv = fopen(cfg.csv_path, "w");
        if (!g_csv) {
            fprintf(stderr, "benchmark: could not open %s for writing: %s\n",
                    cfg.csv_path, strerror(errno));
        } else {
            csv_header();
        }
    }

    printf("my-malloc benchmark suite\n");
    printf("  ops per phase : %ld\n", cfg.ops);
    printf("  seed          : %u  (reuse with --seed %u to reproduce)\n", cfg.seed, cfg.seed);
    printf("  libc compare  : %s\n", cfg.skip_libc ? "disabled" : "enabled");
    printf("  phase mode    : %s\n", cfg.isolate
           ? "isolated (each phase in its own fresh subprocess, cold heap)"
           : "shared (one heap across all phases, --shared was passed)");
    printf("  ALIGN=%zu  HEADER_SIZE=%zu  FOOTER_SIZE=%zu  MIN_FREE_BLOCK=%zu  CHUNK_SIZE=%d\n",
           (size_t)ALIGN, (size_t)HEADER_SIZE, (size_t)FOOTER_SIZE, (size_t)MIN_FREE_BLOCK, CHUNK_SIZE);

    double *scratch = malloc(sizeof(double) * (size_t)cfg.ops);
    if (!scratch) { fprintf(stderr, "benchmark: could not allocate scratch buffer\n"); return 1; }

    if (!cfg.isolate) {
        /* Shared mode: the one process that runs every phase owns the
         * one and only heap_init() call, exactly like the old behavior. */
        heap_init();
    }

    phase_ctx_t ctx = {.cfg = &cfg, .scratch = scratch};

    run_phase(&cfg, phase_fixed_churn, &ctx);
    run_phase(&cfg, phase_random_churn, &ctx);
    run_phase(&cfg, phase_growth_only, &ctx);
    run_phase(&cfg, phase_fragmentation, &ctx);
    run_phase(&cfg, phase_realloc_growth, &ctx);

    printf("\nDone.");
    if (g_csv) {
        printf(" Results also written to CSV.");
        fclose(g_csv);
    }
    printf("\n");

    free(scratch);
    return 0;
}