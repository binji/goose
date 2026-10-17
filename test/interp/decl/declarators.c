/* Declarators: pointers, arrays, functions, groupings, qualifiers dropped,
   typedefs of each, prototypes with and without names, variadic and
   unspecified parameter lists, and parameter adjustment. */
#include <stdint.h>
int a;
int *b, **c, d[3], *e[4], (*f)[4];
const char *const g;
volatile int h;
int (*fp)(int, char *);
int (*fpa[2])(void);
void (*(*nested)(int))(double);
typedef int Arr3[3];
typedef Arr3 *PArr3;
typedef int (*Callback)(void *item, int64_t n);
Arr3 arr;
PArr3 parr;
Callback cb;
int proto1(int x, char *y);
int proto2(int, char *);
int proto3(void);
int proto4();
int proto5(const char *fmt, ...);
void takes_array(int xs[10], int ys[], int (*zs)[3]);
void takes_function(int f(int), int (*g)(int));
void takes_pointer_to_function_returning_pointer(char *(*h)(void));
static inline int64_t helper(int64_t v) { return v + 1; }
int defined_later(int n, char c)
{
    return n + c;
}
extern int external[];
int sized_by_init[] = { 1, 2, 3 };
char text[] = "abc";
char exact[3] = "abc";
unsigned long sz = sizeof(int (*)[4]), al = _Alignof(int64_t);
