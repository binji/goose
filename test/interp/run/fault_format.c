/* A format with more conversions than arguments is a fault, not a read of
   whatever lies beyond them. */
#include <stdio.h>
int main(void) {
    printf("fine: %d\n", 1);
    printf("%d and %s\n", 2);
    return 0;
}
