#ifndef MY_MALLOC_H
#define MY_MALLOC_H

#define _GNU_SOURCE
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <stdlib.h>
#include "list.h"

#define ALIGN _Alignof(max_align_t)
#define ALIGN_UP(n) (((n) + ALIGN - 1) & ~(ALIGN - 1))
#define MIN_FREE_BLOCK (HEADER_SIZE + FOOTER_SIZE + ALIGN)
#define CHUNK_SIZE 4096
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
#define HEAP_SIZE 128 * 1024

#define BLOCK_NEXT_HEADER(curr, payload) \
    ((Block *)((char *)((curr) + 1) + (payload) + FOOTER_SIZE))

#define BLOCK_PREV_HEADER(curr, prev_size) \
    ((Block *)((char *)curr - FOOTER_SIZE - prev_size - HEADER_SIZE))

// function prototypes
void heap_init(void);
Block *find_suitable_block(size_t requestSize);
Block *request_block(size_t size);
Block *split(Block *block, size_t requestPayload);
Block *coalesce(Block *curr);
void set_footer(Block *block);
bool try_expand(Block *block, size_t newPayload);
void my_free(void *ptr);
void* my_malloc(size_t size);
void *my_realloc(void *ptr, size_t size);
bool try_expand(Block *curr, size_t newPayload);
void *my_calloc(size_t num, size_t size);

#endif // MY_MALLOC_H