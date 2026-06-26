#define _GNU_SOURCE
#include <unistd.h>
#include <stdio.h>
#include <assert.h>
#include "list.h"

#define ALIGN _Alignof(max_align_t)
#define ALIGN_UP(n) (((n) + ALIGN - 1) & ~(ALIGN - 1))
#define MIN_FREE_BLOCK (HEADER_SIZE + sizeof(size_t) + ALIGN)
#define FREE 1
#define ALLOCATED 0

typedef struct Block
{
    size_t payload;
    int free;
    list list;
} Block;

const size_t HEADER_SIZE = sizeof(Block);
#define HEAP_SIZE 128 * 1024 + HEADER_SIZE

#define BLOCK_NEXT_HEADER(curr, payload) \
    ((Block *)((char *)((curr) + 1) + (payload) + sizeof(size_t)))

#define BLOCK_PREV_HEADER(curr, prev_size) \
    ((Block *)((char *)curr - sizeof(size_t) - prev_size - HEADER_SIZE))

// set function prototypes
void set_footer(Block *block);

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

    printf("The heap is start at %p\n", start);
    first->payload = HEAP_SIZE - HEADER_SIZE - sizeof(size_t);
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
    void *request = sbrk(HEADER_SIZE + size);
    if (request == (void *)-1)
    {
        return NULL;
    }

    newBlock = (Block *)request;
    newBlock->free = ALLOCATED;
    newBlock->payload = size;
    set_footer(newBlock);
    list_init(&newBlock->list);

    heap_end = (char*)request + HEADER_SIZE + size;
    return newBlock;
}

Block *split(Block *block, size_t requestPayload)
{
    Block *remainder = BLOCK_NEXT_HEADER(block, requestPayload);
    remainder->payload = block->payload - requestPayload - HEADER_SIZE;
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

    if ((char*) next_block != (char*) heap_end && next_block->free == FREE)
    {
        curr->payload += HEADER_SIZE + next_block->payload + sizeof(size_t);
        set_footer(curr);
        list_unlink(&next_block->list);
    }

    size_t *footer = (size_t *)((char *)curr - sizeof(size_t));

    if ((char *)footer >= (char *)heap_start)
    {
        Block *prev_block = BLOCK_PREV_HEADER(curr, (*footer));

        if ((char *)prev_block >= (char *)heap_start && prev_block->free == FREE)
        {
            prev_block->payload += HEADER_SIZE + sizeof(size_t) + curr->payload;
            set_footer(prev_block);
            list_unlink(&curr->list);
        }
    }
}

// set payload footer
void set_footer(Block *block)
{
    size_t *footer =
        (size_t *)((char *)(block + 1) + block->payload) - 1;

    *footer = block->payload;
}
void *my_malloc(size_t size)
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
        printf("Size of allocated block after requested new block %zu\n", block->payload);
        printf("-----------------------------------\n");
        return block + 1;
    }

    if (block->payload >= requestPayload + MIN_FREE_BLOCK)
    {
        block = split(block, requestPayload);
        printf("Size of allocated block after spliting %zu\n", block->payload);
        printf("-----------------------------------\n");
        return block + 1;
    }
    list_unlink(&block->list); // only unlink if it came from free list
    block->free = ALLOCATED;
    printf("Size of allocated block if found %zu\n", block->payload);
    printf("-----------------------------------\n");
    return block + 1;
}
void my_free(void *ptr)
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

    int *y = my_malloc(sizeof(int));
    // printf("Allocated Y: %zu blocks\n", list_length(&head.list));
    // printf("-----------------------------------\n");
    printf("Address of y is at %p\n", y);
    int *x = my_malloc(sizeof(int));
    // printf("Allocated X: %zu blocks\n", list_length(&head.list));
    // printf("-----------------------------------\n");
    printf("Address of x is at %p\n", x);
    double *z = my_malloc(sizeof(double));
    // printf("Allocated Z: %zu blocks\n", list_length(&head.list));
    // printf("-----------------------------------\n");
    printf("Address of z is at %p\n", z);

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