#include "my-malloc.h"

static Block head = {
    .payload = 0,
    .free = 0,
    .list = LIST_INIT(head.list)}; // sentinel header block

static void *heap_start;
static void *heap_end;
static bool initialized = false;

void heap_init()
{

    initialized = true;

    if (!initialized)
        return;

    void *start = sbrk(MMAP_THRESHOLD);

    if (start == (void *)-1)
    {
        return;
    }

    heap_start = start;
    heap_end = start + MMAP_THRESHOLD;
    Block *first = (Block *)start;

    size_t raw_payload = MMAP_THRESHOLD - HEADER_SIZE - FOOTER_SIZE;

    first->payload = raw_payload & ~(ALIGN - 1);
    SET_FREE(first);
    set_footer(first);
    list_init(&first->list);
    list_add_after(&head.list, &first->list);
}

Block *find_suitable_block(size_t requestSize)
{
    list *curr = head.list.next;
    while (curr != &head.list)
    {
        Block *block = list_entry(curr, Block, list);
        if (block->payload >= requestSize)
        {
            return block;
        }
        curr = curr->next;
    }
    return NULL;
}

// request OS to give big chunk of memory
Block *request_block(void)
{

    Block *newBlock = NULL;
    void *request = sbrk(CHUNK_SIZE);
    if (request == (void *)-1)
    {
        return NULL;
    }

    newBlock = (Block *)request;
    SET_FREE(newBlock);
    newBlock->payload = CHUNK_SIZE;
    set_footer(newBlock);
    list_init(&newBlock->list);


    heap_end = (char *)request + CHUNK_SIZE;
    return newBlock;
}

Block *split(Block *block, size_t requestPayload)
{
    Block *remainder = BLOCK_NEXT_HEADER(block, requestPayload);
    remainder->payload = block->payload - requestPayload - HEADER_SIZE - FOOTER_SIZE;
    SET_FREE(remainder); // set it as free;
    set_footer(remainder);
    list_init(&remainder->list);
    list_add_after(&head.list, &remainder->list);

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
        void *ptr = mmap(NULL, total_need,
                         PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

        if (ptr == MAP_FAILED)
        {
            return NULL;
        }

        block = (Block *)ptr;
        SET_ALLOCATED(block);
        SET_MMAP(block);
        block->payload = request_size;
        set_footer(block);
    }
    else
    {

        block = find_suitable_block(request_size);

        if (block == NULL)
        {

            block = request_block();

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
            size_t shrink_amount = survivor->payload - ALIGN;
            list_unlink(&survivor->list);

            survivor->payload = ALIGN;
            set_footer(survivor);
            list_add_after(&head.list, &survivor->list);
            heap_end = (char *)heap_end - shrink_amount;
            sbrk(-shrink_amount);
        }
    }
}