/* Unbounded recursion exhausts the frame segment: the native call stack
   overflow the runtime promises, not a host crash. */
#include "inc/print.h"
static int down(int n) { return n == 0 ? 0 : 1 + down(n + 1); }
int main(void) {
    show("depth", down(1));
    return 0;
}
