/* Calls: recursion deep enough to need the explicit frames, records by
   value in and out, pointers to functions, static locals, mutual
   recursion, compound literals, and arguments evaluated once. */
#include "inc/print.h"

typedef struct { long long x, y; } Point;
typedef struct { Point a, b; char tag; } Segment;

static int depth(int n) { return n == 0 ? 0 : 1 + depth(n - 1); }
static long long fib(int n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }
static int is_even(int n);
static int is_odd(int n) { return n == 0 ? 0 : is_even(n - 1); }
static int is_even(int n) { return n == 0 ? 1 : is_odd(n - 1); }

static Point make(long long x, long long y) {
    Point p;
    p.x = x;
    p.y = y;
    return p;
}
static Point add(Point a, Point b) {
    a.x += b.x;   /* modifies the copy */
    a.y += b.y;
    return a;
}
static long long manhattan(Segment s) { return (s.b.x - s.a.x) + (s.b.y - s.a.y); }
static Segment stretch(Segment s, long long by) {
    s.b = add(s.b, make(by, by));
    s.tag = 's';
    return s;
}
static void move_by(Point *p, Point d) { *p = add(*p, d); }

static int counter(void) {
    static int count = 100;
    return ++count;
}

static int apply(int (*f)(int), int v) { return f(v); }
static int twice(int v) { return 2 * v; }
static int square(int v) { return v * v; }
static int (*pick(int which))(int) { return which ? square : twice; }

static int evaluated;
static int once(void) { return ++evaluated; }
static int three(int a, int b, int c) { return a * 100 + b * 10 + c; }

static int sum_to(int n) {
    if (n == 0) return 0;
    return n + sum_to(n - 1);
}

int main(void) {
    show("depth 20000", depth(20000));
    show("fib 20", fib(20));
    show("sum_to 10000", sum_to(10000));
    show("is_even 1001", is_even(1001));
    show("is_odd 1001", is_odd(1001));

    Point p = make(3, 4);
    Point q = add(p, make(10, 20));
    show("p.x unchanged", p.x);
    show("q.x", q.x);
    show("q.y", q.y);
    Segment s = { { 1, 2 }, { 4, 6 }, 'o' };
    show("manhattan", manhattan(s));
    Segment t = stretch(s, 5);
    show("stretched manhattan", manhattan(t));
    show("original tag", s.tag);
    show("stretched tag", t.tag);
    show("chained member of a result", stretch(s, 1).b.y);
    move_by(&p, make(1, 1));
    show("moved p.y", p.y);
    Point arr[2] = { make(1, 1), add(make(1, 1), make(2, 2)) };
    show("arr[1].x", arr[1].x);
    Point assigned;
    assigned = arr[0] = make(7, 8);
    show("assigned.y", assigned.y);
    show("arr[0].x", arr[0].x);

    show("counter", counter());
    show("counter", counter());
    show("counter", counter());

    show("apply twice", apply(twice, 21));
    show("apply square", apply(square, 9));
    show("pick(1)(5)", pick(1)(5));
    show("(*pick(0))(5)", (*pick(0))(5));
    int (*table[2])(int) = { twice, square };
    show("table[1](6)", table[1](6));
    show("fn == fn", apply == apply);
    show("fn != fn", (void *)twice == (void *)square);
    int (*fp)(int) = 0;
    show("null fn pointer", fp == 0);

    show("args once", three(once(), once(), once()) % 1000 >= 0);
    show("evaluated", evaluated);

    Point *lit = &(Point){ 5, 6 };
    show("compound literal", lit->x + lit->y);
    show("compound in call", manhattan((Segment){ { 0, 0 }, { 2, 3 } }));
    for (int i = 0; i < 2; i++) {
        Point *fresh = &(Point){ i, 0 };
        show("literal per iteration", fresh->x);
    }
    show("(int){ 9 }", (int){ 9 });
    return depth(3) + 4;
}
