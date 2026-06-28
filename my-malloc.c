#define _GNU_SOURCE
#include <unistd.h>
#include <stdio.h>
#include "list.h"

#define ALIGN _Alignof(max_align_t)
#define ALIGN_UP(n) (((n) + ALIGN - 1) & ~(ALIGN - 1))
#define MIN_FREE_BLOCK (HEADER_SIZE + FOOTER_SIZE + ALIGN)
#define FREE 1
#define ALLOCATED 0

typedef struct Block
{
    size_t payload;
    int free;
    list list;
} Block;

#define HEADER_SIZE (sizeof(Block))
#define FOOTER_SIZE ALIGN_UP(sizeof(size_t))
#define HEAP_SIZE 128 * 1024 + HEADER_SIZE

#define BLOCK_NEXT_HEADER(curr, payload) \
    ((Block *)((char *)((curr) + 1) + (payload) + FOOTER_SIZE))

#define BLOCK_PREV_HEADER(curr, prev_size) \
    ((Block *)((char *)curr - FOOTER_SIZE - prev_size - HEADER_SIZE))

// set function prototypes
void set_footer(Block *block);
void free(void *ptr);
bool try_expand(Block *block, size_t newPayload);

static Block head = {
    .payload = 0,
    .free = ALLOCATED,
    .list = LIST_INIT(head.list)}; // sentinel header block

static void *heap_start;
static void *heap_end;

void heap_init()
{

    void *start = sbrk(HEAP_SIZE);

    heap_start = start;
    heap_end = start + HEAP_SIZE;
    if (start == (void *)-1)
    {
        return;
    }
    Block *first = (Block *)start;

    printf("The heap is start at %p\n", heap_start);
    printf("The heap is end at %p\n", heap_end);
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
Block *requestBlock(size_t size)
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

void coalesce(Block *curr)
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
            list_unlink(&curr->list);
        }
    }
}
void set_footer(Block *block)
{
    size_t *footer =
        (size_t *)((char *)(block + 1) + block->payload);

    *footer = block->payload;
}
void *malloc(size_t size)
{

    if (size == 0)
        return NULL;

    size_t requestPayload = ALIGN_UP(size); // find align block
    // size_t total = BLOCK_SIZE + payload + sizeof(size_t);
    Block *block = find_suitable_block(requestPayload);

    if (block == NULL)
    {
        block = requestBlock(requestPayload);

        if (block == NULL)
        {
            return NULL;
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
void *calloc(size_t num, size_t size)
{
    if (num != 0 && size > __SIZE_MAX__ / num)
    {
        return NULL;
    }

    void *ptr = malloc(num * size);
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

void *realloc(void *ptr, size_t size)
{
    if (ptr == NULL)
    {
        return malloc(size);
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

    void* new_ptr = malloc(newPayload);
    if(new_ptr == NULL) return NULL;

    size_t copySize = block->payload;
    if(copySize > newPayload) 
        copySize = newPayload;

    memcpy(new_ptr, ptr, copySize);
    my_free(ptr);
    return new_ptr;
}

void free(void *ptr)
{
    if (ptr == NULL)
        return;
    Block *header = (Block *)ptr - 1;
    header->free = FREE;
    list_add_after(&head.list, &header->list);
    coalesce(header);
}
int main()
{

    heap_init();
    list *next = head.list.next;
    Block *blk = list_entry(next, Block, list);

    printf("Size of HEAP %ld\n", HEAP_SIZE);
    // printf("-----------------------------------\n");
    //  printf("Size of Block header %zu\n", BLOCK_SIZE);
    printf("-----------------------------------\n");
    printf("First free block size is %zu\n", blk->payload);
    printf("-----------------------------------\n");

    int *y = malloc(sizeof(int));
    // printf("Allocated Y: %zu blocks\n", list_length(&head.list));
    // printf("-----------------------------------\n");
    printf("Address of y is at %p\n", y);
    int *x = malloc(sizeof(int));
    // printf("Allocated X: %zu blocks\n", list_length(&head.list));
    // printf("-----------------------------------\n");
    printf("Address of x is at %p\n", x);
    double *z = malloc(sizeof(double));
    // printf("Allocated Z: %zu blocks\n", list_length(&head.list));
    // printf("-----------------------------------\n");
    printf("Address of z is at %p\n", z);

    // char* name = my_malloc(100000000);
    //  printf("Address of name is at %p\n", name);

    my_free(y);
    my_free(x);
    my_free(z);

    list *curr = head.list.next;

    while (curr != &head.list)
    {
        Block *blk = list_entry(curr, Block, list);
        printf("free block — size: %zu\n", blk->payload);
        curr = curr->next;
    }
    printf("Total length is %zu\n", list_length(&head.list));
    return 0;
}