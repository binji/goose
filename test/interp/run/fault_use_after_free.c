/* A freed block stays dead; its handle is never reused. */
#include "inc/print.h"
void *malloc(unsigned long n);
void free(void *p);
static int read_it(const int *p) { return *p; }
int main(void) {
    int *a = malloc(sizeof(int));
    *a = 5;
    free(a);
    show("after free", read_it(a));
    return 0;
}
