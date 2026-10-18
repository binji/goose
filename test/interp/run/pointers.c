/* Pointers as handles: arithmetic, differences, comparisons, arrays of
   records, packed records written through a byte cursor and read back as
   fields, unions, pointers to pointers, and arrays of arrays. */
#include "inc/print.h"
#include <stdint.h>

#pragma pack(push, 1)
typedef struct { uint8_t tag; int64_t id; double w; uint8_t flag; } Item;
typedef struct { uint8_t *data; int64_t len; } sl_u8;
typedef struct {
    uint8_t tag;
    union {
        struct { double v; } num;
        struct { uint8_t v; } boolean;
        struct { sl_u8 s; } str;
    } u;
} Json;
#pragma pack(pop)

typedef struct Node Node;
struct Node { int value; Node *next; };

static int table[5] = { 10, 20, 30, 40, 50 };
static int grid[3][4];
static Item items[3];
static uint8_t bytes[64];

static void store64(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i));
}

int main(void) {
    /* Arithmetic and differences within one segment. */
    int *p = table, *q = &table[4];
    show("q - p", q - p);
    show("*(p + 2)", *(p + 2));
    show("2[p]", 2[p]);
    show("*(q - 1)", *(q - 1));
    show("p < q", p < q);
    show("p == table", p == table);
    show("q == table + 4", q == table + 4);
    show("&table[5] - table", &table[5] - table);   /* one past the end */
    p += 3;
    show("*p after += 3", *p);
    p -= 2;
    show("*p after -= 2", *p);
    show("*p++", *p++);
    show("*--p", *--p);
    show("(char *)q - (char *)p", (char *)q - (char *)p);
    uintptr_t up = (uintptr_t)q, ub = (uintptr_t)table;
    show_u("(uintptr_t) difference", up - ub);
    show("range test inside", up - ub < sizeof table);
    show("range test outside", (uintptr_t)grid - ub < sizeof table);
    show("ptr & 7", (int)((uintptr_t)&table[1] & 7));
    show("null is false", (int *)0 ? 1 : 0);
    int *np = 0;
    show("np == 0", np == 0);
    show("!np", !np);

    /* Arrays of arrays. */
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 4; j++) grid[i][j] = i * 10 + j;
    show("grid[2][3]", grid[2][3]);
    show("(*(grid + 1))[2]", (*(grid + 1))[2]);
    int (*row)[4] = grid + 2;
    show("row[0][1]", row[0][1]);
    show("sizeof grid", sizeof grid);
    show("sizeof *row", sizeof *row);
    show("row - grid", row - grid);
    int *flat = &grid[0][0];
    show("flat[7]", flat[7]);

    /* Records through a byte cursor, as codegen writes them. */
    uint8_t *cur = (uint8_t *)&items[1];
    *cur = 7;
    store64(cur + 1, (uint64_t)-42);
    double w = 2.5;
    memcpy(cur + 9, &w, 8);
    cur[17] = 1;
    show("items[1].tag", items[1].tag);
    show("items[1].id", items[1].id);
    show_f("items[1].w", items[1].w);
    show("items[1].flag", items[1].flag);
    show("sizeof(Item)", sizeof(Item));
    show("&items[2] - &items[0]", &items[2] - &items[0]);
    show("(uint8_t *)&items[2] - (uint8_t *)items", (uint8_t *)&items[2] - (uint8_t *)items);
    Item *ip = &items[1];
    show("ip->id", ip->id);
    ip->id += 2;
    show("ip->id += 2", ip->id);
    show("(*ip).flag", (*ip).flag);

    /* Unions in a packed ADT. */
    Json j;
    j.tag = 1;
    j.u.num.v = 1.5;
    show_f("num", j.u.num.v);
    j.tag = 2;
    j.u.boolean.v = 1;
    show("bool byte", j.u.boolean.v);
    show("num's low byte overwritten", (int)(j.u.num.v == 1.5));
    j.tag = 3;
    j.u.str.s.data = bytes;
    j.u.str.s.len = 5;
    memcpy(bytes, "hello", 5);
    show("str len", j.u.str.s.len);
    print_str("str = ");
    fwrite(j.u.str.s.data, 1, (unsigned long)j.u.str.s.len, stdout);
    print_line();
    show("sizeof(Json)", sizeof(Json));

    /* Linked records on the frame and the heap-free path: locals' addresses. */
    Node c = { 3, 0 }, b = { 2, &c }, a = { 1, &b };
    int sum = 0;
    for (Node *n = &a; n; n = n->next) sum += n->value;
    show("list sum", sum);
    Node **pp = &a.next;
    show("(*pp)->value", (*pp)->value);
    *pp = &c;
    show("a.next->value after relink", a.next->value);

    /* Pointers into a local array survive being passed around. */
    int local[4] = { 1, 2, 3, 4 };
    int *end = local + 4;
    int acc = 0;
    for (int *it = local; it != end; it++) acc = acc * 10 + *it;
    show("local digits", acc);
    show("end - local", end - local);

    /* Casting an integer to a pointer and back is the identity. */
    uintptr_t asint = (uintptr_t)&local[2];
    int *back = (int *)asint;
    show("*back", *back);
    show("(void *)16 arithmetic", (int)((uintptr_t)((char *)16 + 8)));
    const char *s = "pointer to a literal";
    show("s[11]", s[11]);
    show("*s", *s);
    show_s("s + 11", s + 11);
    return 0;
}
