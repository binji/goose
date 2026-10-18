/* Static data: globals with initializers of every form, strings in arrays
   and behind pointers, addresses of globals and of their members in
   initializers, zero-filled remainders, tentative definitions, and static
   locals keeping state. */
#include "inc/print.h"
#include <stdint.h>

typedef struct { const char *name; int value; uint8_t bytes[4]; } Entry;
typedef struct { uint32_t len; int32_t e[7]; } l7_i32;

int tentative;
int tentative;
extern int declared_then_defined;
int declared_then_defined = 42;
static const Entry entries[] = {
    { "one", 1, { 1 } },
    { "two", 2, { 1, 2 } },
    { .value = 3, .name = "three" },
};
static int *const first_value = (int *)&entries[0].value;
static const char *const names[] = { "zero", "one", 0, "three" };
static char greeting[] = "hi";
static char padded[8] = "ab";
static const uint8_t exact[6] = " nodes";
static l7_i32 list = { 3, { 10, 20, 30 } };
static l7_i32 lists[2] = { { 1, { 5 } } };
static int matrix[2][3] = { { 1, 2, 3 }, { 4 } };
static int flat[2][3] = { 1, 2, 3, 4, 5, 6 };
static double dpi = 3.25, *dptr = &dpi;
static int (*fn)(void);
static union { int i; unsigned char b[4]; } pun = { 0x01020304 };
static int order[3] = { [2] = 7, [0] = 5 };
static char *mid = greeting + 1;
static uint8_t narrow = 300;
static long neglong = -1;
static unsigned long long maxu = 18446744073709551615ull;
static float f32 = 0.1f;
static int (*ptr_to_arr)[3] = &matrix[1];

static int next_id(void) {
    static int id;
    static const char tags[] = "abc";
    id++;
    return tags[(id - 1) % 3] * 1000 + id;
}

int main(void) {
    show("tentative", tentative);
    show("declared_then_defined", declared_then_defined);
    show("entries count", sizeof entries / sizeof entries[0]);
    for (unsigned i = 0; i < sizeof entries / sizeof entries[0]; i++) {
        show_s("name", entries[i].name);
        show("value", entries[i].value);
        show("bytes sum", entries[i].bytes[0] + entries[i].bytes[1] + entries[i].bytes[2] + entries[i].bytes[3]);
    }
    show("*first_value", *first_value);
    show("names[2] is null", names[2] == 0);
    show_s("names[3]", names[3]);
    show_s("greeting", greeting);
    show("sizeof greeting", sizeof greeting);
    show_s("padded", padded);
    show("padded[7]", padded[7]);
    show("exact[5]", exact[5]);
    show("list.len", list.len);
    show("list.e[2]", list.e[2]);
    show("list.e[6]", list.e[6]);
    show("lists[1].len", lists[1].len);
    show("matrix[1][0]", matrix[1][0]);
    show("matrix[1][2]", matrix[1][2]);
    show("flat[1][2]", flat[1][2]);
    show_f("*dptr", *dptr);
    show("fn is null", fn == 0);
    show("pun.b[0]", pun.b[0]);
    show("pun.b[3]", pun.b[3]);
    show("order", order[0] * 100 + order[1] * 10 + order[2]);
    show_s("mid", mid);
    show("narrow", narrow);
    show("neglong", neglong);
    show_u("maxu", maxu);
    show_f("f32", f32);
    show("(*ptr_to_arr)[0]", (*ptr_to_arr)[0]);
    show("next_id", next_id());
    show("next_id", next_id());
    show("next_id", next_id());
    show("next_id", next_id());
    greeting[0] = 'H';
    show_s("greeting written", greeting);
    show_s("mid sees it", mid - 1);
    return 0;
}
