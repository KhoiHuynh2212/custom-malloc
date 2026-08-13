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

#define ALIGN _Alignof(max_align_t) // return system align
#define ALIGN_UP(n) (((n) + ALIGN - 1) & ~(ALIGN - 1))
#define MINBLOCKSIZE (HEADER_SIZE + FOOTER_SIZE + ALIGN) // minimum size to split
#define FREE_BIT (1 << 0)                                // bit 0: 1 = free, 0 = allocated
#define MMAP_BIT (1 << 1)                                // bit 1: 1 = mmapped, 0 = sbrk'd

#define SET_FREE(b) ((b)->flags |= FREE_BIT)       // set the block is free
#define SET_ALLOCATED(b) ((b)->flags &= ~FREE_BIT) // set the block is allocated

#define IS_FREE(b) ((b)->flags & FREE_BIT) // check the block is free
#define IS_MMAP(b) ((b)->flags & MMAP_BIT) // check the block is from mmap

#define SET_MMAP(b) ((b)->flags |= MMAP_BIT)  // set the block is from mmap
#define SET_SBRK(b) ((b)->flags &= ~MMAP_BIT) // set the block is from heap

extern long g_sbrk_calls;
extern long g_scan_steps;

typedef struct Block_Header
{
    size_t payload;
    unsigned int flags;
    list list;
} mblockptr; // block header structure 

#define CHUNK_SIZE ((size_t)64U * (size_t)1024U) // Chunk size is 64 KB
#define TOP_PAD_SIZE (CHUNK_SIZE / (size_t)4U)    //
#define LINUX_PAGE sysconf(_SC_PAGESIZE)
#define INITIAL_TOP_SIZE ((size_t)64U * (size_t)1024U)
#define SMALL_BIN_MAX 1024
#define NUM_SMALL_BINS 64
#define LARGE_BIN_MIN_EXP 10 // 2^ 10 = 1024
#define LARGE_BIN_MAX_EXP 16 // 2^ 16 = 65536
#define NUM_LARGE_BINS (LARGE_BIN_MAX_EXP - LARGE_BIN_MIN_EXP + 1)
#define NUM_BINS (NUM_SMALL_BINS + NUM_LARGE_BINS)

#define HEADER_SIZE (sizeof(mblockptr))
#define FOOTER_SIZE ALIGN_UP(sizeof(size_t))
#define ALIGN_HEADER_FOOTER ALIGN_UP(HEADER_SIZE + FOOTER_SIZE)

#define REQUEST_CHUNK(s) ((s) + (HEADER_SIZE) + (FOOTER_SIZE))
#define ABSORB(s) (REQUEST_CHUNK(s))
#define MMAP_THRESHOLD ((size_t)128U * (size_t)1024U) // Trigger mmap allocation

/** Shrink threshold must stay strictly greater than one grow_top() increment
(CHUNK_SIZE) to avoid heap "flapping": growing by CHUNK_SIZE then
immediately freeing it must NOT trigger an immediate shrink back to the OS,
or every alloc/free pair near this size would cost two sbrk() syscalls.
Expressed as CHUNK_SIZE * 2 (not a fixed byte count) so the margin scales
automatically if CHUNK_SIZE is retuned. Currently: 2 * 64 KB = 128 KB.
**/
#define SHRINK_THRESHOLD (CHUNK_SIZE * 2) // 12 KB

#define BLOCK_NEXT_HEADER(curr, payload) \
    ((mblockptr *)((char *)((curr) + 1) + (payload) + FOOTER_SIZE))

#define BLOCK_PREV_HEADER(curr, prev_size) \
    ((mblockptr *)((char *)curr - FOOTER_SIZE - prev_size - HEADER_SIZE))


typedef struct malloc_state {
    mblockptr *topchunkptr; 
    list bins[NUM_BINS];
    size_t topsize; 
    char *heap_start;
    char *heap_end;
} malloc_state;    

// function prototypes
void heap_init(void);
mblockptr *find_suitable_block(size_t request_size);
mblockptr *grow_top(size_t size);
mblockptr *split(mblockptr *block, size_t request_payload);
mblockptr *coalesce(mblockptr *curr);
mblockptr *try_expand(mblockptr *curr, size_t new_payload);

int get_bin_bucket(size_t payload);
size_t get_MSB_bit(size_t x);


static inline void set_footer(mblockptr *block)
{        
    size_t *footer =
        (size_t *)((char *)(block + 1) + block->payload);

    *footer = block->payload;
    assert(*footer == block->payload);
}

void insert_small_chunk(mblockptr * chunk, size_t size);
void insert_large_chunk(mblockptr * chunk, size_t size);

void *my_malloc(size_t size);
void *my_realloc(void *ptr, size_t size);
void *my_calloc(size_t num, size_t size);
void my_free(void *ptr); 

#ifdef DEBUG
const malloc_state *debug_get_state(void);
#endif

#endif // MY_MALLOC_H 

