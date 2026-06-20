#include<unistd.h>
#include<stdio.h> 
#include<assert.h>

typedef struct block_t {
    size_t size;
    struct block_t * next;
    struct block_t * prev;  
} block_t;

void* my_malloc(size_t size) {
    void*   ptr = sbrk(0);

    void* request = sbrk(size);

    if(request == (void*) -1) {
        return NULL;
    } else {
        assert(ptr == request);
        return ptr;
    }

}
int main() {

    int * m = my_malloc(sizeof(int));
    * m = 50;

    printf("Value is %d\n", *m);
    return 0;
}