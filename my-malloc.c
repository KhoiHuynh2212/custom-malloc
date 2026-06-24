#define _GNU_SOURCE
#include<unistd.h>
#include<stdio.h> 
#include<assert.h>
#include "list.h"

#define ALIGN            _Alignof(max_align_t)
#define ALIGN_UP(n) (((n) + ALIGN - 1) & ~(ALIGN - 1))
#define FREE 1
#define ALLOCATED 0

typedef struct Block {
    size_t size;
    int free;
    list list; 
} Block; 

_Static_assert(
    sizeof(Block) % ALIGN  == 0, "Block header must be aligned"
);

const size_t BLOCK_SIZE = sizeof(Block);
#define HEAP_SIZE        128 * 1024 + BLOCK_SIZE

static Block head = {
    .size = 0,
    .free = ALLOCATED,
    .list = LIST_INIT(head.list)
}; // sentinel header block

void heap_init() {
    void* start = sbrk(HEAP_SIZE);
    if(start == (void*) -1) {
        return;
    }
    Block* first = (Block*) start;
    first->size = HEAP_SIZE - BLOCK_SIZE;
    first->free = FREE;
    list_init(&first->list);
    list_add_after(&head.list, &first->list);
}

Block* find_suitable_block(size_t requestSize) {
    list* curr = head.list.next;
    while(curr != &head.list) {
        Block* block = list_entry(curr, Block, list);
        if(block->size >= requestSize) {
            return block;
        }
        curr = curr->next;
    }
    return NULL;
}

// request OS to give more memory if there is no free block 
Block* requestBlock(size_t size) {
    
    Block* newBlock = NULL;
    void* request = sbrk(BLOCK_SIZE + size);
    if(request == (void*) -1) {
        return NULL;
    } 

    newBlock = (Block*) request;
    newBlock->free = ALLOCATED;
    newBlock->size = size;
    list_init(&newBlock->list);
    return newBlock;
}

Block* split(Block* block, size_t totalAllocSize) {
    Block* remainder = (Block*)((char*)(block + 1) + totalAllocSize);
    remainder->size = block->size - totalAllocSize - BLOCK_SIZE;
    remainder->free = FREE;
    list_init(&remainder->list);
    list_add_after(&head.list, &remainder->list);

    // block get trimmed and given to the caller
    block->size = totalAllocSize;
    block->free = ALLOCATED;
    list_unlink(&block->list); 

    return block;
}
void * my_malloc(size_t size) {

    if(size <= 0) return NULL; 

    size_t align = ALIGN_UP(size); // find align block

    Block* block = find_suitable_block(align);

    if(block == NULL) {
        block = requestBlock(align);

        if(block == NULL) {
            return NULL;
        }
        return block + 1;
    }

    if(block->size >= align +  BLOCK_SIZE + ALIGN) {
        block = split(block, align);
        return block + 1;
    }
    list_unlink(&block->list); // only unlink if it came from free list
    block->free = ALLOCATED; 
    return block + 1;
} 

void coalesce(Block* block) {
    // merge with next if free
    Block* next = list_entry(block->list.next, Block, list);
    if(next != &head && next->free == FREE ) {
        block->size += BLOCK_SIZE + next->size;
        list_unlink(&next->list);
    } 

    // merge with prev if free
    Block* prev = list_entry(block->list.prev, Block, list);
    if(prev != &head && prev->free == FREE) {
        block->size += BLOCK_SIZE + prev->size;
        list_unlink(&block->list);
        block = prev;
    }
}
void my_free(void* ptr) {
    if (ptr == NULL) return;
    Block* header = (Block* )ptr - 1;
    header->free = FREE; 
    list_add_after(&head.list, &header->list);
    coalesce(header);
}
int main() {

    heap_init();

    printf("Size of Block header %zu\n", BLOCK_SIZE);
    int * y = my_malloc(sizeof(int));
    printf("After allocated y: %zu blocks\n", list_length(&head.list));
    int * x = my_malloc(sizeof(int));
    printf("After allocated x: %zu blocks\n", list_length(&head.list));
    double * z = my_malloc(sizeof(double));
    printf("After allocated z: %zu blocks\n", list_length(&head.list));

    my_free(y);
    my_free(z);
    my_free(x);

    list* curr = head.list.next; 

    while(curr != &head.list) {
        Block* blk = list_entry(curr, Block, list);
        printf("free block — size: %zu\n", blk->size);
        curr = curr->next;
    }
    printf("Total length is %zu\n", list_length(&head.list));
    return 0;
}