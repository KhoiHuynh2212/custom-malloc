#include "my-malloc.h"

static Block head = {
    .payload = 0,
    .free = 0,
    .list = LIST_INIT(head.list)}; // sentinel header block

static void *heap_start;
static void *heap_end;
static bool initialized = false;

static list *rover = &head.list;

void heap_init()
{

    if (initialized)
        return;

    void *start = sbrk(MMAP_THRESHOLD);

    if (start == (void *)-1)
    {
        return;
    }

    initialized = true;

    heap_start = start;
    heap_end = start + MMAP_THRESHOLD;
    Block *first = (Block *)start;

    size_t raw_payload = MMAP_THRESHOLD - HEADER_SIZE - FOOTER_SIZE;

    first->payload = raw_payload & ~(ALIGN - 1);
    first->free = 0;
    set_footer(first);
    SET_FREE(first);
    list_init(&first->list);
    list_add_after(&head.list, &first->list);
}

Block *find_suitable_block(size_t requestSize)
{

    if (list_is_empty(&head))
    {
        return NULL;
    }

    list *curr = rover->next;

    if (curr == &head.list)
        curr = curr->next;

    list *start = curr;

    do
    {
        Block *block = list_entry(curr, Block, list);
        if (block->payload >= requestSize)
        {
            rover = curr;
            return block;
        }
        curr = curr->next;
        if (curr == &head.list)
            curr = curr->next;

    } while (curr != start);
    return NULL;
}

// request OS to give big chunk of memory
Block *request_block(size_t size)
{
    size_t block_chunk = size + HEADER_SIZE + FOOTER_SIZE;
    size_t allocate_size = (size < CHUNK_SIZE) ? CHUNK_SIZE : block_chunk;

    Block *newBlock = NULL;
    void *request = sbrk(allocate_size);
    if (request == (void *)-1)
    {
        return NULL;
    }

    newBlock = (Block *)request;
    newBlock->payload = allocate_size - HEADER_SIZE - FOOTER_SIZE;
    newBlock->free = 0;
    set_footer(newBlock);
    SET_FREE(newBlock);
    list_init(&newBlock->list);

    heap_end = (char *)request + allocate_size;
    return newBlock;
}

Block *split(Block *block, size_t requestPayload)
{
    Block *remainder = BLOCK_NEXT_HEADER(block, requestPayload);
    remainder->payload = block->payload - requestPayload - HEADER_SIZE - FOOTER_SIZE;
    remainder->free = 0;
    SET_FREE(remainder); // set it as free;
    set_footer(remainder);
    list_init(&remainder->list);
    list_add_after(&head.list, &remainder->list);

    if (rover == &block->list)
    {
        rover = &head.list;
    }
    // block get trimmed and given to the caller
    block->payload = requestPayload;
    SET_ALLOCATED(block); // set it allocated
    set_footer(block);
    list_unlink(&block->list);

    return block;
}

Block *coalesce(Block *curr)
{

    Block *next_block = BLOCK_NEXT_HEADER(curr, curr->payload);

    if ((char *)next_block < (char *)heap_end && IS_FREE(next_block))
    {
        if (rover == &next_block->list)
        {
            rover = &head.list;
        }
        curr->payload += HEADER_SIZE + next_block->payload + FOOTER_SIZE;
        set_footer(curr);
        list_unlink(&next_block->list);
    }

    size_t *footer = (size_t *)((char *)curr - FOOTER_SIZE);

    if ((char *)footer >= (char *)heap_start)
    {
        Block *prev_block = BLOCK_PREV_HEADER(curr, (*footer));

        if ((char *)prev_block >= (char *)heap_start && IS_FREE(prev_block))
        {
            if (rover == &prev_block->list)
            {
                rover = &head.list;
            }
            prev_block->payload += HEADER_SIZE + FOOTER_SIZE + curr->payload;
            set_footer(prev_block);
            return prev_block;
        }
    }
    return curr;
}
void set_footer(Block *block)
{
    size_t *footer =
        (size_t *)((char *)(block + 1) + block->payload);

    *footer = block->payload;
    assert(*footer == block->payload);
}

void *my_malloc(size_t size)
{
    Block *block;

    if (size == 0 || size >= __SIZE_MAX__ - (ALIGN - 1))
    {
        return NULL;
    }

    size_t request_size = ALIGN_UP(size);

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

        block = (Block *)ptr;
        block->free = 0;
        SET_ALLOCATED(block);
        SET_MMAP(block);
        block->payload = total_page_up - HEADER_SIZE - FOOTER_SIZE;
        set_footer(block);
    }
    else
    {

        block = find_suitable_block(request_size);

        if (block == NULL)
        {

            block = request_block(request_size);

            if (block == NULL)
            {
                return NULL;
            }

            if (block->payload >= request_size + MIN_FREE_BLOCK)
            {
                block = split(block, request_size);
                return block + 1;
            }
            else
            {

                SET_ALLOCATED(block);
                SET_SBRK(block);
                return block + 1;
            }
        }

        if (block->payload >= request_size + MIN_FREE_BLOCK)
        {
            block = split(block, request_size);
            return block + 1;
        }

        list_unlink(&block->list); // only unlink if it came from free list
        SET_ALLOCATED(block);      // mark as allocated (clear free bit)
        SET_SBRK(block);           // mark as sbrk'd (clear mmap bit)
    }
    return block + 1;
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

bool try_expand(Block *curr, size_t newPayload)
{
    Block *next = BLOCK_NEXT_HEADER(curr, curr->payload);

    if ((char *)next >= (char *)heap_end)
        return false;

    if (!IS_FREE(next))
        return false;

    size_t merge_size = next->payload + curr->payload + HEADER_SIZE + FOOTER_SIZE;
    if (merge_size < newPayload)
    {
        return false;
    }
    else
    {
        curr->payload = merge_size;
        set_footer(curr);
        list_unlink(&next->list);
        return true;
    }
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

    size_t new_payload = ALIGN_UP(size);
    Block *block = (Block *)ptr - 1;

    if (rover == &block->list)
    {
        rover = &head.list;
    }

    if (!IS_MMAP(block))
    {
        if (new_payload <= block->payload)
        {
            if (block->payload >= new_payload + MIN_FREE_BLOCK)
                split(block, new_payload);

            return ptr;
        }

        if (new_payload < MMAP_THRESHOLD && try_expand(block, new_payload))
        {
            if (block->payload >= new_payload + MIN_FREE_BLOCK)
                split(block, new_payload);

            return ptr;
        }
    }
    else
    {

        if (new_payload <= block->payload)
        {
            return ptr;
        }
        void *new_loc;
        new_loc = mremap(block, block->payload + ALIGN_HEADER_FOOTER, new_payload + ALIGN_HEADER_FOOTER, MREMAP_MAYMOVE);
        if (new_loc == MAP_FAILED)
        {
            perror("mremap");
            return NULL;
        }

        Block *nb = (Block *)new_loc;
        nb->payload = new_payload;
        set_footer(nb);

        return nb + 1;
    }

    new_ptr = my_malloc(size);
    if (new_ptr == NULL)
        return NULL;

    size_t copySize =
        (block->payload < new_payload)
            ? block->payload
            : new_payload;

    memcpy(new_ptr, ptr, copySize);
    my_free(ptr);

    return new_ptr;
}

void my_free(void *ptr)
{
    if (ptr == NULL)
        return;
    Block *block = (Block *)ptr - 1;

    if (IS_FREE(block))
    {
        fprintf(stderr, "double free detected at %p\n", ptr);
        abort();
    }

    if (IS_MMAP(block))
    {
        // Crash on double free
        munmap(block, ALIGN_HEADER_FOOTER + block->payload);
    }
    else
    {
        SET_FREE(block);
        set_footer(block);
        Block *survivor = coalesce(block);
        if (survivor == block)
            list_add_after(&head.list, &survivor->list);

        char *block_end = (char *)survivor + HEADER_SIZE + survivor->payload + FOOTER_SIZE;
        if (block_end == (char *)heap_end && survivor->payload >= SHRINK_THRESHOLD)
        {
            uintptr_t floor = (uintptr_t)heap_start + MMAP_THRESHOLD;
            uintptr_t new_break = (uintptr_t)heap_end - survivor->payload + SHRINK_KEEP;

            // Clamp to the floor
            if (new_break < floor)
            {
                new_break = floor;
            }

            // Calculate actual bytes to give back
            size_t actual_shrink_amt = (uintptr_t)heap_end - new_break;

            if (actual_shrink_amt > 0)
            {
                list_unlink(&survivor->list);

                // Calculate the new payload size based on the actual new break
                survivor->payload = (size_t)(new_break - (uintptr_t)survivor - HEADER_SIZE - FOOTER_SIZE);
                set_footer(survivor);
                list_add_after(&head.list, &survivor->list);

                heap_end = (char *)new_break;
                sbrk(-(intptr_t)actual_shrink_amt);
            }
        }
    }
}