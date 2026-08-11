#include "debug.h"
#include "malloc.h"

void test_grow_top_topsize_stays_synced(void)
{
    heap_init();

    // Force topsize down close to zero without triggering grow yet
    size_t almost_all = 65526;
    void *a = my_malloc(almost_all);
    check_top_chunk();   // still fine here

    // This next call MUST trigger grow_top() internally
    void *b = my_malloc(500);
    check_top_chunk();   // <-- this is where Bug 1 would blow up (topsize underflow)

    printf("test_grow_top_topsize_stays_synced: PASS\n");
}

void test_top_carve_payload_matches_request(void)
{
    heap_init();
    void *p = my_malloc(200);
    check_heap();   // <-- this is where Bug 4 would blow up (BLOCK_NEXT_HEADER walks off)
    printf("test_top_carve_payload_matches_request: PASS\n");
}

int main() {
    test_grow_top_topsize_stays_synced();
    test_top_carve_payload_matches_request();
}