/* Function-like macros: arguments with commas inside parentheses, arguments
   spanning lines, nesting, pre-expansion of arguments, empty arguments, a
   name without parentheses, and the runtime's shapes. */
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define CALL(f, x) f(x)
#define ID(x) x
#define NOARGS() 7
#define ONEARG(x) [x]
int a = MAX(1, 2);
int b = MAX(MAX(1, 2), 3);
int c = MAX((1, 2), f(3, 4));
int d = MAX(1,
            2);
int e = CALL(ID, 5);
int f = NOARGS();
int g = ONEARG();
int h = ONEARG( );
/* The name alone is not an invocation; the parenthesis may come later. */
int (*MAX)(int, int);
int i = MAX
    (8, 9);
/* An argument is expanded before substitution, so the hide set of an inner
   expansion does not reach the outer one. */
#define TEN 10
#define TWICE(x) (x + x)
int j = TWICE(TEN);
/* An argument that names the macro itself expands fully once. */
int k = TWICE(TWICE(1));
/* Nested function-like call written in the argument of another. */
int l = ID(MAX(1, ID(2)));
/* gs_add_i32's release form and GS_IDX's shape. */
#define gs_add_i32(a, b, f, l) ((int32_t)((int64_t)(a) + (int64_t)(b)))
#define GS_IDX(i, n, f, l) \
    ((uint64_t)(i) < (uint64_t)(n) ? (i) : (gs_idxfail((int64_t)(i), (n), (f), (l)), (int64_t)0))
int32_t m = gs_add_i32(x, 1, gs_file0, 12);
int64_t n = GS_IDX(k, len, gs_file1, 34);
/* A macro whose expansion ends in a function-like name takes the parenthesis
   that follows the invocation. */
#define F ID
int o = F(6);
