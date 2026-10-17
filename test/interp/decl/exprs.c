/* Expressions in initializers and sizes: constant folding with C's types,
   casts, sizeof, strings, characters, addresses, and what is not folded. */
#include <stdint.h>
#include <stddef.h>
static const int a[] = { 1, 2, 3 };
int n = sizeof a / sizeof a[0];
int neg = -7 / 2, mod = -7 % 2, shifted = 1 << 20, big = 1 << 31;
unsigned u = 1u << 31, wrapped = (unsigned)-1, mixed = -1 < 0u;
long l = 1L << 40, cast_down = (int8_t)200, cast_up = (uint8_t)-1;
int64_t lit = 123456789012345678, hexlit = 0x7fffffff, octal = 0777;
unsigned long sz = sizeof(uint64_t) * 8 + sizeof(struct S { char c; int i; });
int ch = 'A' + 1, esc = '\n', hex = '\x41', oct = '\101';
char text[] = "ab" "cd\n";
const char *p = "literal";
const uint8_t s6[6] = " nodes";
int cond = 1 ? 2 : 3, logic = (3 > 2) + !0 + (1 && 0) + (0 || 2);
double d = 1.5, e = 1e+15, f = 1.0f / 4, g = (double)3;
float fl = 2.5f;
int *ap = &a[1], *dp = a + 2;
long diff = (long)(&a[2] - &a[0]);
int deref = *a, sub = a[1], member = sizeof(((struct S *)0)->i);
int (*fp)(int) = 0;
void *vp = NULL, *vq = (void *)0;
int folded_compare = 5 > 3 && 2 == 2, unsigned_compare = -1 > 0u;
int64_t rhs = (int64_t)(uint32_t)-1;
uint8_t narrow = 300;
int comma = (1, 2);
struct S s = { 'x', 2 };
struct S arr[2] = { { 'a', 1 }, { 'b', 2 } };
int desig[4] = { [2] = 1, [0] = 2 };
struct S desig2 = { .i = 3, .c = 'c' };
void f1(void);
int call_in_init(void) { return 0; }
/* Initializers typed against their types: conversions, elided braces,
   partial lists, unions, strings in arrays of arrays. */
int m[2][3] = { 1, 2, 3, 4, 5, 6 };
int m2[2][3] = { { 1 }, { 4, 5 } };
struct S ss[2] = { 'a', 1, 'b', 2 };
struct Natural { char a; int b; double c; } nat = { 1 };
union U { int i; double d; } uu = { 5 }, ud = { .d = 2.5 };
int scalar_braced = { 7 };
char cs[2][4] = { "ab", "cd" };
struct Holder { struct S s; int tail[2]; } holder = { { 'q', 9 }, 1 };
struct Holder holder2 = { 'q', 9, 1, 2 };
double from_int = 3, *null_double = 0;
