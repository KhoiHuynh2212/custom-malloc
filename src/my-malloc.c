#include "my-malloc.h"

static Block head = {
    .payload = 0,
    .free = ALLOCATED,
    .list = LIST_INIT(head.list)}; // sentinel header block

static void *heap_start;
static void *heap_end;


void heap_init()
{

    void *start = sbrk(HEAP_SIZE);

    if (start == (void *)-1)
    {
        return;
    }

    heap_start = start;
    heap_end = start + HEAP_SIZE;
    Block *first = (Block *)start;

    size_t raw_payload = HEAP_SIZE - HEADER_SIZE - FOOTER_SIZE;

    first->payload = raw_payload & ~(ALIGN - 1);
    first->free = FREE;
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

// request OS to give more memory if there is no free block
Block *request_block(size_t size)
{

    Block *newBlock = NULL;
    void *request = sbrk(HEADER_SIZE + size + FOOTER_SIZE);
    if (request == (void *)-1)
    {
        return NULL;
    }

    newBlock = (Block *)request;
    newBlock->free = ALLOCATED;
    newBlock->payload = size;
    set_footer(newBlock);
    list_init(&newBlock->list);

    heap_end = (char *)request + HEADER_SIZE + size + FOOTER_SIZE;
    return newBlock;
}

Block *split(Block *block, size_t requestPayload)
{
    Block *remainder = BLOCK_NEXT_HEADER(block, requestPayload);
    remainder->payload = block->payload - requestPayload - HEADER_SIZE - FOOTER_SIZE;
    remainder->free = FREE;
    set_footer(remainder);
    list_init(&remainder->list);
    list_add_after(&head.list, &remainder->list);

    // block get trimmed and given to the caller
    block->payload = requestPayload;
    block->free = ALLOCATED;
    set_footer(block);
    list_unlink(&block->list);

    return block;
}

Block* coalesce(Block *curr)
{

    Block *next_block = BLOCK_NEXT_HEADER(curr, curr->payload);

    if ((char *)next_block < (char *)heap_end && next_block->free == FREE)
    {
        curr->payload += HEADER_SIZE + next_block->payload + FOOTER_SIZE;
        set_footer(curr);
        list_unlink(&next_block->list);
    }

    size_t *footer = (size_t *)((char *)curr - FOOTER_SIZE);

    if ((char *)footer >= (char *)heap_start)
    {
        Block *prev_block = BLOCK_PREV_HEADER(curr, (*footer));

        if ((char *)prev_block >= (char *)heap_start && prev_block->free == FREE)
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
void * my_malloc(size_t size)
{

    if (size == 0)
        return NULL;

    size_t requestPayload = ALIGN_UP(size); // find align block
    
    Block *block = find_suitable_block(requestPayload);

    if (block == NULL)
    {   
        size_t extend_size = (requestPayload > CHUNK_SIZE) ? requestPayload : CHUNK_SIZE;
        block = request_block(extend_size);

        if (block == NULL)
        {
            return NULL;
        }

        if (block->payload >= requestPayload + MIN_FREE_BLOCK)
        {
            block = split(block, requestPayload);
        }

        return block + 1;
    }

    if (block->payload >= requestPayload + MIN_FREE_BLOCK)
    {
        block = split(block, requestPayload);
        return block + 1;
    }
    list_unlink(&block->list); // only unlink if it came from free list
    block->free = ALLOCATED;
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

    if (next->free == ALLOCATED)
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
    if (ptr == NULL)
    {
        return my_malloc(size);
    }
    if (size == 0)
    {
        my_free(ptr);
        return NULL;
    }

    size_t newPayload = ALIGN_UP(size);

    Block *block = (Block *)ptr - 1;

    // shrink
    if (newPayload <= block->payload)
    {
        if (block->payload >= newPayload + MIN_FREE_BLOCK)
            split(block, newPayload);
        
        return ptr;
        
    }
    // grow
    if (try_expand(block, newPayload))
    {
        if (block->payload >= newPayload + MIN_FREE_BLOCK)
            split(block, newPayload);
        
        return ptr;
    } 

    void* new_ptr = my_malloc(newPayload);
    if(new_ptr == NULL) return NULL;

    size_t copySize = block->payload;
    if(copySize > newPayload) 
        copySize = newPayload;

    memcpy(new_ptr, ptr, copySize);
    my_free(ptr);
    return new_ptr;
}

void my_free(void *ptr)
{
    if (ptr == NULL)
        return;
    Block *header = (Block *)ptr - 1;

    if (header->free == FREE)
    {
        fprintf(stderr, "double free detected at %p\n", ptr);
        abort();
    }
    header->free = FREE;
    set_footer(header);
    Block* survivor = coalesce(header);
    if (survivor == header)
        list_add_after(&head.list, &survivor->list);
}