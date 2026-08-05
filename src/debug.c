#include <assert.h>
#include "my-malloc.h"
#include "debug.h"

#ifdef DEBUG

#define DEBUG

void check_top_chunk(void)
{
    assert((char *)(gm.top_chunk + 1) + gm.topsize == gm.heap_end);
}

void check_bins(void)
{
    mblockptr *curr_block;

    for (int i = 0; i < NUM_BINS; i++)
    {
        list_for_each_entry(curr_block, &gm.bins[i], list)
        {   
            asssert(curr_block->payload != 0);
            size_t *footer = (size_t *)((char *)(curr_block + 1) + curr_block*->payload);
            assert(IS_FREE(curr_block));
            assert(get_bin_bucket(curr_block->payload) == i);
            assert(curr_block->payload == *footer)
            assert(curr_block != gm.top_chunk);
        }
    }
} 


void check_heap() {
    // TODO : Traverse sequentially 
    
    // check footer matches payload (payload is nonzero)

}

#endif