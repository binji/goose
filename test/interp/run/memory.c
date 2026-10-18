/* Segments: heap blocks from malloc, calloc and realloc, the library's
   memory functions over them, and a stack segment that grows and moves
   while handles into it stay valid, which is what Goose's data stacks
   need from the interpreter. */
#include "inc/print.h"
#include <stdint.h>

void *malloc(unsigned long n);
void *calloc(unsigned long n, unsigned long size);
void *realloc(void *p, unsigned long n);
void free(void *p);
void *memset(void *d, int c, unsigned long n);
void *memmove(void *d, const void *s, unsigned long n);
int memcmp(const void *a, const void *b, unsigned long n);
unsigned long strlen(const char *s);
void *gs_host_reserve(unsigned long size);
void gs_host_release(void *p);

typedef struct { int64_t id; int64_t data[3]; } Rec;

int main(void) {
    int *a = malloc(4 * sizeof(int));
    for (int i = 0; i < 4; i++) a[i] = i * i;
    show("a[3]", a[3]);
    int *b = calloc(8, sizeof(int));
    show("calloc zero", b[7]);
    memcpy(b, a, 4 * sizeof(int));
    show("b[2] after memcpy", b[2]);
    show("memcmp equal", memcmp(a, b, 16));
    b[1] = 99;
    show("memcmp less", memcmp(a, b, 16));
    show("memcmp greater", memcmp(b, a, 16));
    a = realloc(a, 16 * sizeof(int));
    show("a[3] after realloc", a[3]);
    for (int i = 4; i < 16; i++) a[i] = i * 10;
    a[15] = 7;
    show("a[15]", a[15]);
    memset(a, 0xff, 8);
    show("memset bytes", a[1]);
    show("untouched", a[2]);
    memmove(a + 1, a, 8 * sizeof(int));
    show("memmove overlap a[2]", a[2]);
    show("memmove overlap a[8]", a[8]);
    free(a);
    free(b);
    free(0);
    void *empty = malloc(0);
    show("malloc(0) is not null", empty != 0);
    free(empty);
    show("strlen", strlen("twelve chars"));
    char *s = malloc(6);
    memcpy(s, "hello", 6);
    show("strlen heap", strlen(s));
    free(s);

    /* A stack segment: nothing is accessible until written, then it grows
       to the access, zero-filled, as the guard page does natively. */
    uint8_t *stack = gs_host_reserve(1 << 20);
    Rec *first = (Rec *)stack;
    first->id = 1;
    first->data[0] = 10;
    Rec *far = (Rec *)(stack + 600000);
    far->id = 2;
    far->data[2] = 20;
    show("first->id after growth", first->id);
    show("first->data[0]", first->data[0]);
    show("far->id", far->id);
    show("zero between", stack[300000]);
    show("far - first", (uint8_t *)far - (uint8_t *)first);
    int64_t *cell = &far->data[1];
    *cell = 33;
    uint8_t *top = stack + (1 << 20) - 1;
    *top = 44;   /* the last byte of the reservation */
    show("*cell after moving again", *cell);
    show("*top", *top);
    show("top - stack", top - stack);
    for (int i = 0; i < 1000; i++) ((int32_t *)stack)[i] = i;
    int64_t total = 0;
    for (int i = 0; i < 1000; i++) total += ((int32_t *)stack)[i];
    show("sum of a thousand", total);
    gs_host_release(stack);

    uint8_t *small = gs_host_reserve(100);
    small[99] = 1;
    show("small[99]", small[99]);
    gs_host_release(small);
    return 0;
}
