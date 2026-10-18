/* printf, fprintf, sprintf and snprintf: the conversions the runtime uses
   and the usual flags, widths and precisions, each argument decoded by its
   conversion's type. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    printf("plain text\n");
    printf("%d %i %u\n", 42, -42, 42u);
    printf("%d %d\n", INT32_MIN, INT32_MAX);
    printf("%u %x %X %o\n", UINT32_MAX, 255, 255, 8);
    printf("%ld %lld %lu %llu %zu\n", INT64_MIN, INT64_MAX, UINT64_MAX, UINT64_MAX, (size_t)123456789012345);
    printf("%lx %llX\n", UINT64_MAX, 0xdeadbeefcafeULL);
    printf("%hhd %hd %hhu %hu\n", 300, 70000, 300, 70000);
    printf("[%5d] [%-5d] [%05d] [%+d] [% d]\n", 42, 42, 42, 42, 42);
    printf("[%*d] [%-*d] [%.*d]\n", 6, 7, 6, 7, 4, 7);
    printf("[%#x] [%#o] [%.3d] [%8.3d]\n", 255, 8, 5, 5);
    printf("%c%c%c\n", 'a', 98, 'c');
    printf("[%s] [%10s] [%-10s] [%.3s] [%.*s]\n", "text", "right", "left", "truncated", 2, "ab-not-this");
    char raw[4] = { 'r', 'a', 'w', 'x' };
    printf("[%.3s] [%.4s]\n", raw, raw);
    printf("%s\n", (char *)0);
    printf("100%% done\n");
    printf("%f %.2f %10.3f %-10.1f| %e %g %g\n", 3.5, 3.14159, 2.5, 2.5, 1234.5, 0.0001, 1e20);
    printf("%f %f\n", (float)1.5f, 1.0 / 4);
    const char *p = "x";
    printf("%d\n", (int)(strlen(p) + 1));
    printf("%s %d %s %lld\n", "mixed", 1, "args", 2ll);

    fprintf(stdout, "to stdout %d\n", 1);
    fprintf(stderr, "to stderr %d\n", 2);
    int n = fprintf(stdout, "%s", "returns the count");
    printf(" = %d\n", n);

    char buf[16];
    n = snprintf(buf, sizeof buf, "%d-%s", 12345, "abcdefghij");
    printf("snprintf -> %d \"%s\" len %d\n", n, buf, (int)strlen(buf));
    n = snprintf(buf, 0, "%d", 1);
    printf("snprintf(0) -> %d, buf untouched \"%s\"\n", n, buf);
    n = snprintf(buf, 1, "%d", 1);
    printf("snprintf(1) -> %d \"%s\"\n", n, buf);
    n = snprintf(0, 0, "%s and %s", "nothing", "no one");
    printf("snprintf(NULL, 0) -> %d\n", n);
    char big[64];
    n = sprintf(big, "%s|%5d|%-3c|%x", "sprintf", -7, 'z', 4096);
    printf("sprintf -> %d \"%s\"\n", n, big);
    return 0;
}
