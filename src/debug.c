#include <assert.h>
#include "my-malloc.h"
#include "debug.h"

#ifdef DEBUG
static const malloc_state *state = NULL;

static void ensure_state(void)
{
    if (state == NULL)
        state = debug_get_state();   // chỉ gọi hàm accessor 1 LẦN, lần đầu tiên cần dùng
}


void check_top_chunk(void)
{
    ensure_state();
    assert((char *)(state->topchunkptr + 1) + state->topsize == state->heap_end);
}

void check_bins(void)
{
    ensure_state();
    mblockptr *curr;

    for (int i = 0; i < NUM_BINS; i++)
    {
        list_for_each_entry(curr, &state->bins[i], list)
        {
            assert(curr->payload != 0);
            size_t *footer = (size_t *)((char *)(curr + 1) + curr->payload);
            assert(IS_FREE(curr));
            assert(get_bin_bucket(curr->payload) == i);
            assert(curr->payload == *footer);
            assert(curr != state->topchunkptr);
        }
    }
}


void check_heap() {
    ensure_state();
    mblockptr * curr = (mblockptr *) state->heap_start;

    while(curr != (mblockptr*) state->topchunkptr && (char*) curr < state->heap_end)
    {
        assert(curr->payload != 0);
        mblockptr* next = BLOCK_NEXT_HEADER(curr, curr->payload);
        assert((char *)next <= state->heap_end);
        size_t *footer = (size_t *)((char *)(curr + 1) + curr->payload);
        assert(curr->payload == *footer);

        if(next != state->topchunkptr) {
            assert(!(IS_FREE(curr) && IS_FREE(next)));
        } else {
            assert(!IS_FREE(curr));
        }
        curr = next;
    }
}


void check_heap_bin_consistency(void) {
    ensure_state();

    size_t free_chunk = 0;
    mblockptr * curr = (mblockptr*) state->heap_start;

    while(curr != (mblockptr*) state->topchunkptr && (char*) curr < state->heap_end) {
        if(IS_FREE(curr)) {
            free_chunk++;
        }

        curr = BLOCK_NEXT_HEADER(curr, curr->payload);
    }

    size_t binned_cnt = 0;

    for(int i = 0; i <  NUM_BINS; i++) {
        binned_cnt += list_length(&state->bins[i]);
    }

    assert(free_chunk == binned_cnt);
}


void check_malloced_chunk (void* ptr, size_t requested_size) {
    // TODO: implement this function
}

#endif