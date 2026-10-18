/* A function the unit declares and nothing defines is a fault when called,
   as an unresolved symbol would be at link time, not before. */
#include "inc/print.h"
int nothing_defines_this(int x);
int main(void) {
    show("before", 1);
    return nothing_defines_this(2);
}
