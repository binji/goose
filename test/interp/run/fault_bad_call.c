/* A call through a pointer that is not a function. */
#include "inc/print.h"
static int data[2];
int main(void) {
    int (*f)(void) = (int (*)(void))data;
    return f();
}
