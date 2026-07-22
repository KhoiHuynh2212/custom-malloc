#ifndef MY_MALLOC_H
#define MY_MALLOC_H

#define _GNU_SOURCE
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/mman.h>
#include "list.h"

#define ALIGN _Alignof(max_align_t)
#define ALIGN_UP(n) (((n) + ALIGN - 1) & ~(ALIGN - 1))
#define MIN_FREE_BLOCK (HEADER_SIZE + FOOTER_SIZE + ALIGN)
#define CHUNK_SIZE 64 * 1024 // 64 KB
#define SHRINK_KEEP (CHUNK_SIZE / 4) 
#define LINUX_PAGE sysconf(_SC_PAGESIZE)

#define FREE_BIT      (1 << 0)   // bit 0: 1 = free, 0 = allocated
#define MMAP_BIT      (1 << 1)   // bit 1: 1 = mmapped, 0 = sbrk'd

#define SET_FREE(b)      ((b)->free |= FREE_BIT)
#define SET_ALLOCATED(b) ((b)->free &= ~FREE_BIT)

#define IS_FREE(b)        ((b)->free & FREE_BIT) 
#define IS_MMAP(b)        ((b)->free & MMAP_BIT) 

#define SET_MMAP(b)       ((b)->free |= MMAP_BIT)
#define SET_SBRK(b)       ((b)->free &= ~MMAP_BIT)  
 
extern long g_sbrk_calls; 
extern long g_scan_steps; 

typedef struct Block
{
    size_t payload;
    int free;
    list list;
} Block;

#define HEADER_SIZE (sizeof(Block))
#define FOOTER_SIZE ALIGN_UP(sizeof(size_t)) 

#define ALIGN_HEADER_FOOTER ALIGN_UP(HEADER_SIZE + FOOTER_SIZE)
#define MMAP_THRESHOLD 128 * 1024 // 128 KB
#define SHRINK_THRESHOLD (CHUNK_SIZE * 2)  // 12 KB
#define BLOCK_NEXT_HEADER(curr, payload) \
    ((Block *)((char *)((curr) + 1) + (payload) + FOOTER_SIZE))

#define BLOCK_PREV_HEADER(curr, prev_size) \
    ((Block *)((char *)curr - FOOTER_SIZE - prev_size - HEADER_SIZE))

// function prototypes
void heap_init(void);
Block *find_suitable_block(size_t request_size);
Block *extend_heap(size_t size);
Block *split(Block *block, size_t request_payload);
Block *coalesce(Block *curr);
Block* try_expand(Block *curr, size_t new_payload);

void my_free(void *ptr);

static inline void set_footer(Block *block)
{
    size_t *footer =
        (size_t *)((char *)(block + 1) + block->payload);

    *footer = block->payload;
    assert(*footer == block->payload);
}
void *my_malloc(size_t size);
void *my_realloc(void *ptr, size_t size);
void *my_calloc(size_t num, size_t size);



#endif // MY_MALLOC_H