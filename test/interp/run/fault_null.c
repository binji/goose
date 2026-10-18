/* A store through a null pointer, after a field offset. */
#include "inc/print.h"
typedef struct { int pad; int value; } S;
int main(void) {
    S *s = 0;
    print_str("before\n");
    s->value = 1;
    print_str("after\n");
    return 0;
}
