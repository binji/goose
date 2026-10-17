/* # and ##: the runtime's GS_CAT and GS_LIBM_SYM2, pasting that forms
   identifiers and numbers, a placemarker from an empty argument, and
   stringizing with spaces, strings and escapes. */
#define GS_CAT_(a, b) a##b
#define GS_CAT(a, b) GS_CAT_(a, b)
#define VERSION 1a2b
void GS_CAT(gs_rt_start_, VERSION)(void);
int GS_CAT_(x, VERSION);
int GS_CAT_(1, 5) = GS_CAT_(0x, 1F);
#define TRIPLE(a, b, c) a##b##c
int TRIPLE(p, q, r);
int TRIPLE(p, , r);
int TRIPLE(, , r);
#define SUFFIX(x) x##_g
int SUFFIX(main)(void);
#define STR(x) #x
#define XSTR(x) STR(x)
const char *a = STR(hello);
const char *b = STR( a +   b  );
const char *c = STR("quoted \n text");
const char *d = STR('\'');
const char *e = STR(VERSION);
const char *f = XSTR(VERSION);
const char *g = STR();
#define GS_LIBM_SYM2(p, f) #p #f
const char *h = GS_LIBM_SYM2(gs_, sin);
/* The pasted token is not rescanned for the macro it came from, but is for
   other macros. */
#define AB 99
#define MAKE_AB(x) x##B
int i = MAKE_AB(A);
