/* Division by zero traps natively; here it is a fault with a location. */
#include "inc/print.h"
static int divide(int a, int b) { return a / b; }
int main(void) {
    show("divide", divide(7, 2));
    show("divide", divide(7, 0));
    return 0;
}
