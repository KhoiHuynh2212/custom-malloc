#ifndef DEBUG_H
#define DEBUG_H
#include "internal.h"   

#ifdef DEBUG

static void check_any_chunk(mblockptr* block);

void check_heap(void);

void check_bins(void);

void check_top_chunk(void);

void check_heap_bin_consistency(void);

void check_malloced_chunk(void *ptr, size_t size);

void check_mmapped_chunk(mblockptr* block);
#else
#define check_heap() ((void)0)
#define check_bins() ((void)0)
#define check_top_chunk() ((void)0)
#define check_heap_bin_consistency()((void) 0)
#define check_malloced_chunk(ptr, size) ((void)0)
#define check_mmapped_chunk() ((void)0)
#endif

#endif