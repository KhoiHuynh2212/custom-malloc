#include "my-malloc.h"

static int tests_run = 0;
static int tests_failed = 0;

#define CHECK(cond, msg)                                             \
    do {                                                              \
        tests_run++;                                                  \
        if (cond) {                                                   \
            printf("  [PASS] %s\n", msg);                             \
        } else {                                                      \
            printf("  [FAIL] %s  (line %d)\n", msg, __LINE__);        \
            tests_failed++;                                           \
        }                                                              \
    } while (0)

#define SECTION(name) printf("\n== %s ==\n", name)

static void test_basic_alloc(void)
{
    SECTION("basic malloc/free");

    void *p = my_malloc(64);
    CHECK(p != NULL, "malloc(64) returns non-NULL");

    // memory must be writable across the whole requested size
    memset(p, 0xAB, 64);
    unsigned char *bytes = (unsigned char *)p;
    int ok = 1;
    for (int i = 0; i < 64; i++) {
        if (bytes[i] != 0xAB) { ok = 0; break; }
    }
    CHECK(ok, "allocated memory is fully writable");

    my_free(p);
    CHECK(1, "my_free() does not crash");
}

static void test_malloc_zero(void)
{
    SECTION("malloc(0)");
    void *p = my_malloc(0);
    CHECK(p == NULL, "malloc(0) returns NULL");
}

static void test_distinct_blocks_no_overlap(void)
{
    SECTION("distinct non-overlapping blocks");

    void *a = my_malloc(32);
    void *b = my_malloc(32);
    void *c = my_malloc(32);

    CHECK(a && b && c, "three allocations succeed");
    CHECK(a != b && b != c && a != c, "pointers are distinct");

    memset(a, 0x11, 32);
    memset(b, 0x22, 32);
    memset(c, 0x33, 32);

    unsigned char *pa = a, *pb = b, *pc = c;
    int ok = 1;
    for (int i = 0; i < 32; i++) {
        if (pa[i] != 0x11 || pb[i] != 0x22 || pc[i] != 0x33) { ok = 0; break; }
    }
    CHECK(ok, "writes to one block don't clobber neighbors");

    my_free(a);
    my_free(b);
    my_free(c);
}

static void test_free_list_reuse(void)
{
    SECTION("freed block is reused by a later malloc of similar size");

    void *p1 = my_malloc(128);
    CHECK(p1 != NULL, "first malloc(128) succeeds");
    my_free(p1);

    void *p2 = my_malloc(128);
    CHECK(p2 == p1, "malloc(128) after free reuses the same address");
    my_free(p2);
}

static void test_calloc_zeroes(void)
{
    SECTION("calloc zero-initializes memory");

    size_t n = 50;
    unsigned char *p = my_calloc(n, sizeof(unsigned char));
    CHECK(p != NULL, "calloc succeeds");

    int ok = 1;
    for (size_t i = 0; i < n; i++) {
        if (p[i] != 0) { ok = 0; break; }
    }
    CHECK(ok, "all bytes are zero");

    // sanity: calloc overflow guard (num * size overflows -> NULL)
    void *huge = my_calloc((size_t)-1, 2);
    CHECK(huge == NULL, "calloc detects multiplication overflow");

    my_free(p);
}

static void test_realloc_grow_preserves_data(void)
{
    SECTION("realloc grow preserves contents");

    char *p = my_malloc(16);
    CHECK(p != NULL, "malloc(16) succeeds");
    memcpy(p, "hello world!!", 14);

    char *p2 = my_realloc(p, 256);
    CHECK(p2 != NULL, "realloc to larger size succeeds");
    CHECK(memcmp(p2, "hello world!!", 14) == 0, "original data preserved after grow");

    my_free(p2);
}

static void test_realloc_shrink(void)
{
    SECTION("realloc shrink");

    char *p = my_malloc(256);
    CHECK(p != NULL, "malloc(256) succeeds");
    memcpy(p, "shrink me", 10);

    char *p2 = my_realloc(p, 16);
    CHECK(p2 != NULL, "realloc to smaller size succeeds");
    CHECK(memcmp(p2, "shrink me", 10) == 0, "data preserved after shrink");

    my_free(p2);
}

static void test_realloc_null_acts_like_malloc(void)
{
    SECTION("realloc(NULL, size) behaves like malloc");
    void *p = my_realloc(NULL, 40);
    CHECK(p != NULL, "realloc(NULL, 40) returns non-NULL");
    my_free(p);
}

static void test_realloc_zero_acts_like_free(void)
{
    SECTION("realloc(ptr, 0) behaves like free");
    void *p = my_malloc(40);
    void *r = my_realloc(p, 0);
    CHECK(r == NULL, "realloc(ptr, 0) returns NULL");
}

static void test_coalescing(void)
{
    SECTION("adjacent free blocks coalesce");

    // Three back-to-back allocations
    void *a = my_malloc(64);
    void *b = my_malloc(64);
    void *c = my_malloc(64);
    CHECK(a && b && c, "three allocations for coalesce test succeed");

    size_t before = list_length(&((Block *)a - 1)[0].list); // not meaningful alone; just touch list.h

    my_free(a);
    my_free(b); // freeing the middle+first should coalesce with 'a' and with 'c' if 'c' were free too

    // Now allocate something bigger than a single 64-byte block but that
    // fits in the coalesced a+b region, to prove they merged.
    void *big = my_malloc(64 + 64 + HEADER_SIZE + FOOTER_SIZE - 8);
    CHECK(big != NULL, "allocation fitting only in the merged a+b region succeeds");
    CHECK(big == a, "merged region reused starting at freed block 'a'");

    (void)before;
    my_free(big);
    my_free(c);
}

static void test_stress(void)
{
    SECTION("stress: many random malloc/free/realloc cycles");

    enum { N = 200 };
    void *ptrs[N] = {0};
    size_t sizes[N] = {0};

    unsigned seed = 12345;
    for (int i = 0; i < N; i++) {
        seed = seed * 1103515245 + 12345;
        size_t sz = (seed % 500) + 1;
        ptrs[i] = my_malloc(sz);
        sizes[i] = sz;
        if (ptrs[i]) memset(ptrs[i], (int)(i & 0xFF), sz);
    }

    int alloc_ok = 1;
    for (int i = 0; i < N; i++) {
        if (!ptrs[i]) { alloc_ok = 0; break; }
    }
    CHECK(alloc_ok, "200 varied-size allocations all succeeded");

    // verify data integrity, then free every other block
    int data_ok = 1;
    for (int i = 0; i < N; i++) {
        unsigned char *b = ptrs[i];
        for (size_t j = 0; j < sizes[i]; j++) {
            if (b[j] != (unsigned char)(i & 0xFF)) { data_ok = 0; break; }
        }
        if (!data_ok) break;
    }
    CHECK(data_ok, "no cross-contamination between the 200 blocks");

    for (int i = 0; i < N; i += 2) {
        my_free(ptrs[i]);
        ptrs[i] = NULL;
    }

    // reallocate into the freed holes
    int realloc_ok = 1;
    for (int i = 0; i < N; i += 2) {
        seed = seed * 1103515245 + 12345;
        size_t sz = (seed % 500) + 1;
        ptrs[i] = my_malloc(sz);
        if (!ptrs[i]) { realloc_ok = 0; break; }
        memset(ptrs[i], 0x5A, sz);
        sizes[i] = sz;
    }
    CHECK(realloc_ok, "re-allocating into freed holes succeeds");

    for (int i = 0; i < N; i++) {
        my_free(ptrs[i]);
    }
    CHECK(1, "freeing all remaining blocks does not crash");
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    heap_init();

    test_basic_alloc();
    test_malloc_zero();
    test_distinct_blocks_no_overlap();
    test_free_list_reuse();
    test_calloc_zeroes();
    test_realloc_grow_preserves_data();
    test_realloc_shrink();
    test_realloc_null_acts_like_malloc();
    test_realloc_zero_acts_like_free();
    test_coalescing();
    test_stress();

    printf("\n=====================================\n");
    printf("%d/%d checks passed\n", tests_run - tests_failed, tests_run);
    printf("=====================================\n");

    return tests_failed == 0 ? 0 : 1;
}