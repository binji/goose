/* Standard input: fgets, fgetc, getchar and fread on stdin, which the
   runner feeds from stdin.stdin. */
#include <stdio.h>
#include <string.h>

int main(void) {
    char line[64];
    printf("fgets: %s", fgets(line, sizeof line, stdin));
    printf("fgets short buffer: %s|\n", fgets(line, 5, stdin));
    printf("rest of that line: %s", fgets(line, sizeof line, stdin));
    printf("fgetc: %c%c%c\n", fgetc(stdin), fgetc(stdin), getchar());
    char buf[8];
    size_t n = fread(buf, 1, sizeof buf, stdin);
    printf("fread: %d bytes \"%.*s\"\n", (int)n, (int)n, buf);
    printf("feof: %d\n", feof(stdin));
    n = fread(buf, 1, sizeof buf, stdin);
    printf("fread at end: %d, feof %d, fgets null %d, fgetc EOF %d\n", (int)n, feof(stdin),
           fgets(line, sizeof line, stdin) == NULL, fgetc(stdin) == EOF);
    return 0;
}
