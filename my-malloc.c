#include<unistd.h>
#include<stdio.h> 
#include<assert.h>
#include "list.h"

#define HEAP_SIZE        4096
#define ALIGN            _Alignof(max_align_t)
#define ALIGN_UP(n) (((n) + ALIGN - 1) & ~(ALIGN - 1))


typedef struct Block {
    size_t size;
    int free;
    list lst; 
} Block; 

static Block head = {
    .size = 0,
    .free = 1,
    .lst = {&head.lst, &head.lst}
};

void heap_init() {
    void* start = sbrk(HEAP_SIZE);
    if(start == (void*) -1) {
        return;
    }
    Block* first = (Block*) start;
    first->size = HEAP_SIZE - sizeof(Block);
    first->free = 1;
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
int main() {
    printf("Current program break: %10p\n", sbrk(0));
    heap_init(); 
    printf("After init heap: %10p\n", sbrk(0));
    return 0;
}