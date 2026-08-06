#include <assert.h>
#include "my-malloc.h"
#include "debug.h"

#ifdef DEBUG

void check_top_chunk(void)
{
    assert((char *)(gm.top_chunk + 1) + gm.topsize == gm.heap_end);
}

void check_bins(void)
{
    mblockptr *curr;

    for (int i = 0; i < NUM_BINS; i++)
    {
        list_for_each_entry(curr, &gm.bins[i], list)
        {   
            assert(curr->payload != 0);
            size_t *footer = (size_t *)((char *)(curr + 1) + curr->payload);
            assert(IS_FREE(curr));
            assert(get_bin_bucket(curr->payload) == i);
            assert(curr->payload == *footer);
            assert(curr != gm.top_chunk);
        }
    }
} 


void check_heap() {
    
    mblockptr * curr = (mblockptr *) gm.heap_start;

    while(curr != (mblockptr*) gm.top_chunk && (char*) curr < gm.heap_end) 
    {
        assert(curr->payload != 0);
        mblockptr* next = BLOCK_NEXT_HEADER(curr, curr->payload);
        
        size_t *footer = (size_t *)((char *)(curr + 1) + curr->payload);
        assert(curr->payload == *footer);

        if(next != gm.top_chunk) {
            assert(!(IS_FREE(curr) && IS_FREE(next)));
        }
        curr = next;
    }  
} 


void check_malloced_chunk (void* ptr, size_t requested_size) {
    // TODO: implement this function
}

#endif