#include "my-malloc.h"

#ifdef DEBUG

void check_heap(void);
void check_bins(void);
void check_top_chunk(void);

#else

#define check_heap() ((void)0)
#define check_bins() ((void)0)
#define check_top_chunk() ((void)0)


#endif