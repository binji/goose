/* The smallest program: a write to stdout through the library, and main's
   status as the process's. */
typedef struct FILE FILE;
extern FILE *stdout;
unsigned long fwrite(const void *p, unsigned long size, unsigned long n, FILE *f);

int main(void) {
    const char msg[] = "hello, interpreter\n";
    fwrite(msg, 1, sizeof msg - 1, stdout);
    return 0;
}
