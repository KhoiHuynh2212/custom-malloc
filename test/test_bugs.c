#include "debug.h"
#include "malloc.h"

void test_grow_top_topsize_stays_synced(void)
{
    heap_init();

    size_t almost_all = 65526;
    void *a = my_malloc(almost_all);
    check_top_chunk();   

    
    void *b = my_malloc(500);
    check_top_chunk();   

    printf("test_grow_top_topsize_stays_synced: PASS\n");
}

void test_top_carve_payload_matches_request(void)
{
    heap_init();
    void *p = my_malloc(200);
    check_heap();  
    printf("test_top_carve_payload_matches_request: PASS\n");
}


void test_shrink_threshold() {
    
}

int main() {
    test_grow_top_topsize_stays_synced();
    test_top_carve_payload_matches_request();
}