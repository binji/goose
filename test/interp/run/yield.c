/* A native yield point: gs_host_yield hands control back to the VM's
   caller, who resumes the program where it left off. The output is the
   same however often the run is suspended; the unit test counts the
   resumes. */
#include <stdio.h>
void gs_host_yield(void);

static int counter;

static int step(void) {
    counter++;
    gs_host_yield();
    return counter * 10;
}

int main(void) {
    printf("before\n");
    int a = step();
    printf("after one: %d\n", a);
    int b = step() + step();
    printf("after three: %d, counter %d\n", b, counter);
    return counter;
}
