#include "my-malloc.h"

static malloc_state gm; // allocator state
static bool initialized = false;

long g_sbrk_calls = 0;
long g_scan_steps = 0;

static_assert(sizeof(mblockptr) % ALIGN == 0, "Must be mutiple of 16");

pthread_mutex_t global_lock = PTHREAD_MUTEX_INITIALIZER;

void heap_init()
{
    pthread_mutex_lock(&global_lock);
    if (initialized)
    {

        pthread_mutex_unlock(&global_lock);
        return;
    }
    // initialize the heap == 128 KB
    void *start = sbrk(INITIAL_HEAP_SIZE);

    if (start == (void *)-1)
    {
        pthread_mutex_unlock(&global_lock);
        return;
    }

    gm.heap_start = start;
    gm.heap_end = start + INITIAL_HEAP_SIZE;

    for (int i = 0; i < NUM_BINS; i++)
    {
        list_init(&gm.bins[i]);
    }

    gm.top_chunk = (mblockptr *)start;

    size_t raw_payload = INITIAL_HEAP_SIZE - HEADER_SIZE - FOOTER_SIZE;

    gm.top_chunk->payload = raw_payload & ~(ALIGN - 1);
    gm.topsize = gm.top_chunk->payload; // update the top size;
    gm.top_chunk->flags = 0;
    SET_FREE(gm.top_chunk);

    list_init(&gm.top_chunk->list);

    initialized = true; // turn the flag on
    pthread_mutex_unlock(&global_lock);
}

int get_bin_bucket(size_t payload)
{
    if (payload < SMALL_BIN_MAX)
    {
        return payload >> 4; // exact size small bin
    }

    int msb = 63 - __builtin_clzl((unsigned)payload);

    if (msb < LARGE_BIN_MIN_EXP)
        msb = LARGE_BIN_MIN_EXP;
    if (msb > LARGE_BIN_MAX_EXP)
        msb = LARGE_BIN_MAX_EXP;

    return NUM_SMALL_BINS + (msb - LARGE_BIN_MIN_EXP);
}

mblockptr *find_suitable_block(size_t request_size)
{
    int idx = get_bin_bucket(request_size);

    if (list_is_empty(&gm.bins[idx]))
    {
        // if the bins are empty, we carve from the top chunk
        return NULL;
    }

    list *curr = &gm.bins[idx]; // curr is head of that bins
    curr = curr->next;          // move to next block
    mblockptr *block = list_entry(curr, mblockptr, list);

    if (block->payload == request_size)
    {
        // always hit, setting other variables in malloc()
        list_unlink(&block->list);
        return block;
    }
    else
    {
        // THIS IS STILL O(N) - can be optimize if use tree O(log n)
        // TODO: PERFORM SEACHIN IN UNSORTED LARGE BINS
        mblockptr *curr_block;
        mblockptr *best = NULL;
        list_for_each_entry(curr_block, &gm.bins[idx], list)
        {

            if (curr_block->payload < request_size)
                continue;

            if (best == NULL || curr_block->payload < best->payload)
            {
                best = curr_block;
            }
        }

        if (best != NULL)
            list_unlink(&best->list);

        return best;

        // TODO: CHANGE AFTER WE CHANGE TO SORTED BINS
    }
    // if the there is no block, the caller must request from top chunk
    return NULL;
}

// extend the program break by asking OS to give big chunk of memory and assume the top chunk already exists
mblockptr *grow_top(size_t size)
{
    if (gm.top_chunk == NULL)
    {
        return NULL;
    }
    size_t block_chunk = size + HEADER_SIZE;
    size_t allocate_size = (size < CHUNK_SIZE) ? CHUNK_SIZE : block_chunk + MINBLOCKSIZE;

    void *request = sbrk(allocate_size);
    g_sbrk_calls++;
    if (request == (void *)-1)
    {
        return NULL;
    }

    gm.top_chunk->payload += allocate_size;
    gm.heap_end = (char *)request + allocate_size;
    return gm.top_chunk;
}

mblockptr *split(mblockptr *block, size_t request_size)
{
    mblockptr *remainder = BLOCK_NEXT_HEADER(block, request_size);
    remainder->payload = block->payload - REQUEST_CHUNK(request_size);
    remainder->flags = 0;
    SET_FREE(remainder); // set it as free;
    set_footer(remainder);
    list_init(&remainder->list);

    list_add_after(&gm.bins[get_bin_bucket(remainder->payload)], &remainder->list);

    // block get trimmed and given to the caller
    block->payload = request_size;
    SET_ALLOCATED(block); // set it allocated
    set_footer(block);
    list_unlink(&block->list);

    return block;
}

mblockptr *coalesce(mblockptr *curr)
{
    size_t *footer = (size_t *)((char *)curr - FOOTER_SIZE);

    int prev_free = ((char *)footer >= gm.heap_start);

    mblockptr *prev = prev_free ? BLOCK_PREV_HEADER(curr, *footer) : NULL;

    prev_free = (prev_free && (char *)prev >= gm.heap_start && IS_FREE(prev));

    if(prev_free) {

        list_unlink(&prev->list);
        prev->payload += REQUEST_CHUNK(curr->payload);
        set_footer(prev);

        curr = prev; // set new curr at prev block
    }

    mblockptr *next = BLOCK_NEXT_HEADER(curr, curr->payload);


    // the next block is top chunk, absorb to top chunk
    if(next == gm.top_chunk) {
        
        curr->payload += ABSORB(next->payload);

        gm.topsize = curr->payload;

        gm.top_chunk = curr;

        
        return curr;
    } 

    if ((char *)next < gm.heap_end && IS_FREE(next))
    {
    
        curr->payload += REQUEST_CHUNK(next->payload);
        set_footer(curr);
        list_unlink(&next->list);
    }

    return curr;
}

void *my_malloc(size_t size)
{
    if (gm.top_chunk == NULL)
    {
        heap_init();
    }

    mblockptr *curr_block;
    int s;

    if (size == 0 || size >= SIZE_MAX - (ALIGN - 1))
    {
        return NULL;
    }

    size_t request_size = ALIGN_UP(size);

    if (request_size > SIZE_MAX - ALIGN_HEADER_FOOTER)
        return NULL;

    if (request_size >= MMAP_THRESHOLD)
    {

        size_t total_need = ALIGN_HEADER_FOOTER + request_size;
        size_t total_page_up = ((total_need + LINUX_PAGE - 1) & ~(LINUX_PAGE - 1));

        void *ptr = mmap(NULL, total_page_up,
                         PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

        if (ptr == MAP_FAILED)
        {

            return NULL;
        }

        curr_block = (mblockptr *)ptr;
        curr_block->flags = 0;
        SET_ALLOCATED(curr_block);
        SET_MMAP(curr_block);
        curr_block->payload = total_page_up - HEADER_SIZE - FOOTER_SIZE;
        set_footer(curr_block);
    }
    else
    {

        s = pthread_mutex_lock(&global_lock);

        if (s != 0)
        {
            fprintf(stderr, "pthread_mutex_lock failed\n");
        }

        curr_block = find_suitable_block(request_size);

        // if return null, the bucket is empty
        if (curr_block == NULL)
        {

            // carve from the top chunk
            if (request_size >= gm.topsize)
            {   
                // grow if the top chunk is too small
                if (grow_top(request_size) == NULL)
                {
                    pthread_mutex_unlock(&global_lock);
                    return NULL;
                }
            }

            
            mblockptr *p = gm.top_chunk; // start at old top
            size_t needed = request_size + ALIGN_HEADER_FOOTER;
            p->payload = needed;   // 
            SET_ALLOCATED(p);
            set_footer(p);

            gm.topsize -= needed;
            gm.top_chunk = BLOCK_NEXT_HEADER(p, request_size); // bump request byte

            curr_block = p;
        }
        else
        {
            // for large bins only, small bins are fixed size allocated
            if (curr_block->payload >= request_size + MINBLOCKSIZE)
            {
                curr_block = split(curr_block, request_size);
            }

            SET_ALLOCATED(curr_block); // mark as allocated (clear free bit)
            SET_SBRK(curr_block);      // mark as sbrk'd (clear mmap bit)
        }

        s = pthread_mutex_unlock(&global_lock);

        if (s != 0)
        {
            fprintf(stderr, "pthread_mutex_unlock failed\n");
        }
    }

    return curr_block + 1;
}

void *my_calloc(size_t num, size_t size)
{

    if (num != 0 && size > __SIZE_MAX__ / num)
    {
        return NULL;
    }

    void *ptr = my_malloc(num * size);
    if (ptr == NULL)
    {
        return NULL;
    }

    memset(ptr, 0, num * size);

    return ptr;
}

mblockptr *try_expand(mblockptr *curr, size_t new_payload)
{   

    // TODO : gm.topchunk case
    mblockptr *next = BLOCK_NEXT_HEADER(curr, curr->payload);

    if(next == gm.top_chunk) {

        if(new_payload >= gm.topsize) {
            if(grow_top(new_payload) == NULL) {
                return NULL;
            }
        }
        size_t needed = new_payload -curr->payload;
        curr->payload = new_payload;
        gm.topsize -= needed; 
        
        set_footer(curr); // re-calculate the footer 

        mblockptr * np = BLOCK_NEXT_HEADER(curr, curr->payload); 

        gm.top_chunk = np;

        return curr;
    }

    int next_free = ((char *)next < gm.heap_end && IS_FREE(next));

    size_t next_gains = next_free ? (next->payload + HEADER_SIZE + FOOTER_SIZE) : 0;

    if (next_free)
    {   
        list_unlink(&next->list);
        curr->payload += REQUEST_CHUNK(next->payload);
        set_footer(curr);
        if (curr->payload >= new_payload)
            return curr;
    }

    size_t *footer = (size_t *)((char *)curr - FOOTER_SIZE);

    int prev_free = ((char *)footer >= gm.heap_start);

    mblockptr * prev = prev_free ? BLOCK_PREV_HEADER(curr, *footer) : NULL;

    prev_free = (prev_free && (char *)prev >= gm.heap_start && IS_FREE(prev));

    size_t prev_gains = prev_free ? (prev->payload + HEADER_SIZE + FOOTER_SIZE) : 0;

    if (prev_free && (curr->payload + prev->payload + HEADER_SIZE + FOOTER_SIZE >= new_payload))
    {

        list_unlink(&prev->list);
        prev->payload += REQUEST_CHUNK(curr->payload) ;
        prev->flags = 0;
        set_footer(prev);

        if (curr->payload > 0)
            memmove(prev + 1, curr + 1, curr->payload);

        return prev;
    }

    return NULL;
}

void *my_realloc(void *ptr, size_t size)
{
    void *new_ptr;

    if (ptr == NULL)
    {
        return my_malloc(size);
    }
    if (size == 0)
    {
        my_free(ptr);
        return NULL;
    }

    size_t request_size = ALIGN_UP(size);
    mblockptr *current_block = (mblockptr *)ptr - 1;

    if (!IS_MMAP(current_block))
    {
        int s = pthread_mutex_lock(&global_lock);
        if (s != 0)
            fprintf(stderr, "pthread_mutex_lock failed\n");

        // resize to smaller size, cut off and split the block
        if (request_size <= current_block->payload)
        {
            if (current_block->payload >= request_size + MINBLOCKSIZE)
                split(current_block, request_size);

            pthread_mutex_unlock(&global_lock);
            return ptr;
        }

        // if current block is not fit but the request size is smaller than MMAP_THRESHOLD
        if (request_size < MMAP_THRESHOLD)
        {
            mblockptr *surv = try_expand(current_block, request_size);

            if (surv != NULL)
            {
                if (surv->payload >= request_size + MINBLOCKSIZE)
                    split(surv, request_size); // split survivor block

                pthread_mutex_unlock(&global_lock);
                return surv + 1;
                // try_expand may move the data to previous address, to ensure we return correct address of the data, use block + 1
            }
        }

        mblockptr *next_block = BLOCK_NEXT_HEADER(current_block, current_block->payload);

        if ((char *)next_block == gm.heap_end)
        {

            size_t buffered = current_block->payload + current_block->payload;
            size_t new_payload = (buffered > request_size) ? buffered : request_size;
            size_t allocated_size = new_payload - current_block->payload;

            if (request_size < MMAP_THRESHOLD)
            {
                // automatically extend the program break and update its heap and payload
                void *request = sbrk(allocated_size);
                if (request != (void *)-1)
                {
                    current_block->payload += allocated_size;
                    set_footer(current_block);
                    gm.heap_end += allocated_size;

                    if (current_block->payload >= request_size + MINBLOCKSIZE)
                        split(current_block, request_size);

                    pthread_mutex_unlock(&global_lock);
                    return ptr;
                }
            }
        }
        pthread_mutex_unlock(&global_lock);
    }
    else
    {

        if (request_size <= current_block->payload)
        {
            return ptr;
        }

        void *new_loc;
        new_loc = mremap(current_block, current_block->payload + ALIGN_HEADER_FOOTER, request_size + ALIGN_HEADER_FOOTER, MREMAP_MAYMOVE);
        if (new_loc == MAP_FAILED)
        {
            perror("mremap");
            return NULL;
        }

        mblockptr *nb = (mblockptr *)new_loc;
        nb->payload = request_size;
        set_footer(nb);

        return nb + 1;
    }

    new_ptr = my_malloc(size);
    if (new_ptr == NULL)
        return NULL;

    size_t copySize =
        (current_block->payload < request_size)
            ? current_block->payload
            : request_size;

    memcpy(new_ptr, ptr, copySize);
    my_free(ptr);

    return new_ptr;
}

void insert_small_chunk(mblockptr * chunk,size_t size) {
    int idx = get_bin_bucket(size);
    list* head = &gm.bins[idx];   // now at the sentinel head       
    list_push_front(head, &chunk->list);
}

void insert_large_chunk() {

}

void my_free(void *ptr)
{
    if (ptr == NULL)
        return;


    // get the block header 
    mblockptr *block = (mblockptr *)ptr - 1; 
    
    size_t size = block->payload;
    // TODO: check if smaller than SMALL_BIN_MAX then decide which functions to call
    // Coalesce trước biết size rồi mới insert vào bins

    int s;
    if (IS_FREE(block))
    {
        fprintf(stderr, "double free detected at %p\n", ptr);
        abort();
    }

    if (IS_MMAP(block))
    {
       
        munmap(block, ALIGN_HEADER_FOOTER + block->payload);
    }
    else
    {
        s = pthread_mutex_lock(&global_lock);
        if (s != 0)
            fprintf(stderr, "pthread_mutex_lock failed\n");
        SET_FREE(block);
        set_footer(block);
        mblockptr *survivor = coalesce(block);
        if (survivor == block)
            list_add_after(&gm.bins[get_bin_bucket(survivor->payload)], &survivor->list);


        
        char *block_end = (char *)survivor + HEADER_SIZE + survivor->payload + FOOTER_SIZE;
        /*  If the last block is bigger than a shrink threshold, we shrink and return memory for OS, but we must to make sure that we
            don't shrink too much to even below the inital heap size
        */

        if (block_end == gm.heap_end && survivor->payload >= SHRINK_THRESHOLD)
        {
            uintptr_t floor = (uintptr_t)gm.heap_start + MMAP_THRESHOLD;
            uintptr_t new_break = (uintptr_t)gm.heap_end - survivor->payload + SHRINK_KEEP;

            // Clamp to the floor
            if (new_break < floor)
            {
                new_break = floor;
            }

            // Calculate actual bytes to give back
            size_t actual_shrink_amt = (uintptr_t)gm.heap_end - new_break;

            if (actual_shrink_amt > 0)
            {

                list_unlink(&survivor->list);

                // Calculate the new payload size based on the actual new break
                survivor->payload = (size_t)(new_break - (uintptr_t)survivor - HEADER_SIZE - FOOTER_SIZE);
                set_footer(survivor);
                list_add_after(&gm.bins[get_bin_bucket(survivor->payload)], &survivor->list);

                gm.heap_end = (char *)new_break;
                sbrk(-(intptr_t)actual_shrink_amt);
            }
        }
        s = pthread_mutex_unlock(&global_lock);
        if (s != 0)
            fprintf(stderr, "pthread_mutex_unlock failed\n");
    }
}