/* An access past a stack segment's reservation is the data stack overflow
   a guard page catches natively. */
#include "inc/print.h"
void *gs_host_reserve(unsigned long size);
int main(void) {
    char *stack = gs_host_reserve(4096);
    stack[4095] = 1;
    show("last byte", stack[4095]);
    stack[4096] = 2;
    show("unreachable", 0);
    return 0;
}
