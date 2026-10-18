/* string.h and the number conversions: the adapters decode each string by
   its own rule, and hand pointers back as handles into the argument's
   segment. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

static int sign(int v) { return v < 0 ? -1 : v > 0; }

int main(void) {
    char buf[32];
    strcpy(buf, "hello");
    printf("strcpy: %s (%d)\n", buf, (int)strlen(buf));
    strcat(buf, ", world");
    printf("strcat: %s (%d)\n", buf, (int)strlen(buf));
    printf("strcmp: %d %d %d %d\n", sign(strcmp("a", "b")), sign(strcmp("b", "a")), strcmp("same", "same"),
           sign(strcmp("ab", "abc")));
    printf("strncmp: %d %d %d\n", strncmp("abcd", "abzz", 2), sign(strncmp("abcd", "abzz", 3)), strncmp("", "", 5));
    char unterminated[3] = { 'x', 'y', 'z' };
    printf("strncmp unterminated: %d\n", strncmp(unterminated, "xyz", 3));
    const char *s = "find the needle in the haystack";
    printf("strchr: %s\n", strchr(s, 'n'));
    printf("strrchr: %s\n", strrchr(s, 'h'));
    printf("strchr none: %d\n", strchr(s, 'q') == NULL);
    printf("strchr NUL: %d\n", (int)(strchr(s, 0) - s));
    printf("strstr: %s\n", strstr(s, "needle"));
    printf("strstr none: %d\n", strstr(s, "pin") == NULL);
    printf("strstr empty: %d\n", (int)(strstr(s, "") - s));
    printf("memchr: %s\n", (char *)memchr(s, 'i', strlen(s)));
    printf("memchr none: %d\n", memchr(s, 'z', 5) == NULL);
    char padded[8];
    strncpy(padded, "ab", sizeof padded);
    int zeros = 0;
    for (int i = 2; i < 8; i++) zeros += padded[i] == 0;
    printf("strncpy: %s, %d zero bytes after\n", padded, zeros);
    strncpy(padded, "longer than eight", 8);
    printf("strncpy cut: %.8s\n", padded);
    char *dup = strdup("duplicate");
    dup[0] = 'D';
    printf("strdup: %s\n", dup);
    free(dup);
    printf("strerror(ENOENT): %s\n", strerror(ENOENT));

    printf("atoi: %d %d %d\n", atoi("123"), atoi("  -45xyz"), atoi("nope"));
    printf("atol: %ld\n", atol("9000000000"));
    char *end;
    long v = strtol("  0x1fz", &end, 16);
    printf("strtol hex: %ld, rest \"%s\"\n", v, end);
    v = strtol("0777", &end, 0);
    printf("strtol auto octal: %ld, rest \"%s\"\n", v, end);
    v = strtol("-1010", &end, 2);
    printf("strtol binary: %ld\n", v);
    v = strtol("abc", &end, 10);
    printf("strtol none: %ld, end at start %d\n", v, end == (char *)"abc" || *end == 'a');
    printf("strtoll: %lld\n", strtoll("-9223372036854775808", NULL, 10));
    printf("strtoull: %llu\n", strtoull("18446744073709551615", NULL, 10));
    errno = 0;
    printf("strtoul range: %lu, ERANGE %d\n", strtoul("99999999999999999999", NULL, 10), errno == ERANGE);
    double d = strtod("3.5e2 tail", &end);
    printf("strtod: %g, rest \"%s\"\n", d, end);
    printf("atof: %g\n", atof("-0.25"));

    printf("ctype: %d%d%d%d%d%d %c%c\n", isdigit('7'), isalpha('x'), isspace('\n'), isupper('a'), isxdigit('F'),
           ispunct('!'), toupper('q'), tolower('Q'));
    return 0;
}
