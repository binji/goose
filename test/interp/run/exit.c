/* exit() ends the program from any depth with its status; output before
   it stays. */
#include "inc/print.h"
void exit(int status);
static void deep(int n) {
    if (n == 0) {
        print_str("exiting\n");
        exit(3);
    }
    deep(n - 1);
}
int main(void) {
    print_str("start\n");
    deep(10);
    print_str("not reached\n");
    return 0;
}
