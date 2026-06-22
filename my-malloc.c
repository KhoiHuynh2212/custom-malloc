#include<unistd.h>
#include<stdio.h> 
#include<assert.h>
#include "list.h"

#define HEAP_SIZE        4096
#define ALIGN            _Alignof(max_align_t)
#define ALIGN_UP(n) (((n) + ALIGN - 1) & ~(ALIGN - 1))

#define FREE 1
#define ALLOCATED 0

typedef struct Block {
    size_t size;
    int free;
    list lst; 
} Block; 

static Block head = {
    .size = 0,
    .free = ALLOCATED,
    .lst = {&head.lst, &head.lst}
};

void heap_init() {
    void* start = sbrk(HEAP_SIZE);
    if(start == (void*) -1) {
        return;
    }
    Block* first = (Block*) start;
    first->size = HEAP_SIZE - sizeof(Block);
    first->free = FREE;
    list_add_after(&head.lst, &first->lst);
}

Block* find_suitable_block(size_t requestSize) {
    list* curr = head.lst.next;
    while(curr != &head.lst) {
        Block* blk = list_entry(curr, Block, lst);
        if(blk->size >= requestSize) {
            return blk;
        }
        curr = curr->next;
    }
    return NULL;
}

// request OS to give more memory if there is no free block 
Block* requestBlock(size_t size) {
    
    Block* newBlock = NULL;
    void* request = sbrk(sizeof(Block) + size);
    if(request == (void*) -1) {
        return NULL;
    } 

    newBlock = (Block*) request;
    newBlock->free = ALLOCATED;
    newBlock->size = size;
    list_init(&newBlock->lst);
    return newBlock;
}
void * my_malloc(size_t size) {
    size_t align = ALIGN_UP(size);

    Block* blk = find_suitable_block(align);
    if(blk == NULL) {
        blk = requestBlock(align);

        if(blk == NULL) {
            return NULL;
        }
        return blk + 1;
    } 
    
    list_unlink(&blk->lst); // only unlink if it came from free list
    return blk + 1;
} 


void my_free(void* ptr) {
    if (ptr == NULL) return;
    Block* header = (Block* )ptr - 1;
    header->free = FREE; 
    list_add_after(&head.lst, &header->lst);
}
int main() {

    heap_init();
    
    int * y = my_malloc(sizeof(int));
    int * x = my_malloc(sizeof(int));

    double * z = my_malloc(sizeof(double));

    my_free(y);
    my_free(z);

    list* curr = head.lst.next; 

    while(curr != &head.lst) {
        Block* blk = list_entry(curr, Block, lst);
        printf("free block — size: %zu\n", blk->size);
        curr = curr->next;
    }
    return 0;
}