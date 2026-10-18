/* The difference of pointers into two objects has no meaning. */
#include "inc/print.h"
static int a[4], b[4];
int main(void) {
    show("within", &a[3] - &a[1]);
    show("across", &b[0] - &a[0]);
    return 0;
}
