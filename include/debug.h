#include "my-malloc.h"

#ifdef DEBUG
const malloc_state *debug_get_state(void);

void check_heap(void);
void check_bins(void);
void check_top_chunk(void);
void check_heap_bin_consistency(void);
void check_malloced_chunk(void *ptr, size_t requested_size);
#else

#define check_heap() ((void)0)
#define check_bins() ((void)0)
#define check_top_chunk() ((void)0)


#endif