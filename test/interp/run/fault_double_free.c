#include "inc/print.h"
void *malloc(unsigned long n);
void free(void *p);
int main(void) {
    void *p = malloc(8);
    free(p);
    free(p);
    return 0;
}
