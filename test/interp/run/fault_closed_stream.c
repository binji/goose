/* Writing to a closed stream is a fault, as is a FILE * that is no stream. */
#include <stdio.h>
int main(int argc, char **argv) {
    char path[4096];
    snprintf(path, sizeof path, "%s/f", argv[1]);
    FILE *f = fopen(path, "wb");
    fputs("once\n", f);
    fclose(f);
    fputs("twice\n", f);
    return 0;
}
