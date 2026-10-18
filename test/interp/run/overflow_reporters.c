/* The two overflows the runtime reports itself: where the program defines
   gs_native_overflow and gs_data_overflow (the runtime's GS_INTERP branch
   does), the VM calls them in place of its own fault. */
#include <stdio.h>
#include <stdlib.h>

void *gs_host_reserve(unsigned long usable);
static unsigned char *base;

void gs_native_overflow(void) {
    puts("native overflow reported by the program");
    /* The data stack: the handle the VM passes is the reservation's base. */
    base[300] = 1;
    puts("not reached");
    exit(2);
}

void gs_data_overflow(const unsigned char *b) {
    printf("data overflow reported by the program at its base: %d\n", b == base);
    exit(3);
}

static int descend(int n) { return n == 0 ? 0 : 1 + descend(n + 1); }

int main(void) {
    base = gs_host_reserve(200);
    base[199] = 1;
    puts("within the reservation");
    return descend(1);
}
