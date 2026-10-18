/* A write one element past a heap block is a fault, where natively it
   would corrupt the allocator or the neighbor. */
#include "inc/print.h"
void *malloc(unsigned long n);
int main(void) {
    int *a = malloc(4 * sizeof(int));
    for (int i = 0; i <= 4; i++) {
        show("i", i);
        a[i] = i;
    }
    return 0;
}
