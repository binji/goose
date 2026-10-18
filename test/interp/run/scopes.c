/* Block scopes: shadowing, a typedef shadowed by a variable and a variable
   by a typedef, locals of inner blocks, declarations in for, record tags
   local to a block, and locals keeping their slots across iterations. */
#include "inc/print.h"
typedef int T;
static int x = 1;
int main(void) {
    show("global x", x);
    {
        int x = 2;
        show("block x", x);
        {
            int x = 3;
            show("inner x", x);
        }
        show("block x again", x);
    }
    show("global x again", x);
    T t = 5;
    {
        int T = 7;   /* the typedef is shadowed */
        show("T as variable", T);
    }
    T u = t + 1;
    show("T as type again", u);
    for (int i = 0; i < 3; i++) {
        int acc;   /* uninitialized but assigned before use */
        acc = i * 2;
        struct Local { int v; } l = { acc };
        show("local record", l.v);
    }
    {
        typedef struct Pair { int a, b; } Pair;
        Pair p = { 1, 2 };
        show("block typedef", p.a + p.b);
    }
    {
        struct Pair { int q; } other = { 9 };
        show("another Pair", other.q);
    }
    {
        int y = 1;
        int (*fp)(void) = main;
        show("main's address is not null", fp != 0);
        show("y", y);
    }
    return 0;
}
