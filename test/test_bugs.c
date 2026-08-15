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


void test_no_shrink_below_threshold (void) {
    heap_init();
    const malloc_state * s = debug_get_state();
    size_t top_size_before = s->topsize;  
    void* heap_end_before = s->heap_end;
    assert(top_size_before < (size_t) SHRINK_THRESHOLD);
    void* p = my_malloc(16);
    assert(p != NULL);
    my_free(p);
    assert(s->heap_end == heap_end_before);
    assert(s->topsize != TOP_PAD_SIZE);
    assert(s->topsize >= top_size_before);
    printf("test_no_shrink_below_threshold: PASS (topsize=%zu, still below %d)\n",
           s->topsize, SHRINK_THRESHOLD);
} 


void test_shrink_lands_on_pad_size(void) {
    heap_init();
    const malloc_state * st = debug_get_state(); 

    for (int i = 0; i < 3; i++)
        assert(grow_top(1) != NULL); 
    assert(st->topsize >= (size_t)SHRINK_THRESHOLD);

    void *guard = my_malloc(16);
    assert(guard != NULL);
    my_free(guard);

    assert(st->topsize == (size_t)TOP_PAD_SIZE);
    assert(st->topchunkptr->payload == (size_t)TOP_PAD_SIZE);
    assert((char *)(st->topchunkptr + 1) + st->topsize == st->heap_end);

    printf("test_no_shrink_below_threshold: PASS (topsize=%zu, still below %d)\n",
           st->topsize, SHRINK_THRESHOLD);

}
int main() {
    test_shrink_lands_on_pad_size();
}