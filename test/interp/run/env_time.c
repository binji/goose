/* getenv and the clocks: the runner sets INTERP_RUN_VAR. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int main(void) {
    const char *v = getenv("INTERP_RUN_VAR");
    printf("INTERP_RUN_VAR = %s\n", v ? v : "(unset)");
    printf("unset variable is null: %d\n", getenv("INTERP_RUN_SURELY_UNSET_VARIABLE") == NULL);

    struct timespec a, b;
    printf("clock_gettime: %d %d\n", clock_gettime(CLOCK_MONOTONIC, &a), clock_gettime(CLOCK_REALTIME, &b));
    printf("sizeof timespec: %d\n", (int)sizeof a);
    printf("realtime after 2020: %d\n", b.tv_sec > 1577836800L);
    printf("nanoseconds in range: %d %d\n", a.tv_nsec >= 0 && a.tv_nsec < 1000000000L, b.tv_nsec >= 0 && b.tv_nsec < 1000000000L);
    struct timespec nap = { 0, 2000000 };
    printf("nanosleep: %d\n", nanosleep(&nap, NULL));
    struct timespec c;
    clock_gettime(CLOCK_MONOTONIC, &c);
    long long elapsed = (c.tv_sec - a.tv_sec) * 1000000000LL + (c.tv_nsec - a.tv_nsec);
    printf("monotonic advanced by at least 2 ms: %d\n", elapsed >= 2000000);
    time_t now = time(NULL), also;
    time(&also);
    printf("time agrees with realtime: %d, stores through its pointer: %d\n", now - b.tv_sec <= 1 && now - b.tv_sec >= -1,
           also - now <= 1 && also >= now);
    return 0;
}
