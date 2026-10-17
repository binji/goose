/* #if arithmetic as runtime.h writes it: defined in both forms, unsigned
   suffixes and shifts, comparisons, the ternary, unknown names as 0,
   __has_builtin, nesting, #elif chains, and a dead #error. */
#define GS_STACK_RESERVE (1ull << 31)
#define GS_DEBUG 0
#if GS_STACK_RESERVE > (1ull << 48)
#error "too large"
#endif
#if GS_STACK_RESERVE > (1ull << 30)
int reserve_over_1g;
#endif
#if defined(__GNUC__) || defined(__clang__)
int gnu;
#elif defined(_MSC_VER)
int msvc;
#else
int plain;
#endif
#if defined __GOOSE_INTERP__ && !defined(__TINYC__)
int interp;
#endif
#if defined(__has_builtin)
#if __has_builtin(__builtin_elementwise_sqrt)
int builtin_sqrt;
#else
int plain_sqrt;
#endif
#endif
#if GS_DEBUG
int debug;
#elif GS_DEBUG == 0 && UNKNOWN_NAME == 0
int release;
#else
int neither;
#endif
#ifdef GS_DEBUG
int debug_defined;
#endif
#ifndef GS_NEED_THREADS
#define GS_NEED_THREADS 0
#endif
#if GS_NEED_THREADS
int threads;
#endif
/* Arithmetic: signed division and modulo, unsigned comparison, shifts past
   the width, the ternary, character constants, octal and hex. */
#if -7 / 2 == -3 && -7 % 2 == -1
int division;
#endif
#if -1 < 0 && -1u > 0 && (0u - 1) > 1000
int unsigned_compare;
#endif
#if (1 << 63) < 0 && (1u << 31) == 0x80000000 && 0777 == 511 && 'A' == 65 && '\n' == 10
int shifts_and_constants;
#endif
#if 1 ? 2 : (1 / 0)
int ternary_short;
#endif
#if (3 > 2) + (2 > 3) == 1 && ~0 == -1 && !0 == 1 && (6 & 3) == 2 && (6 | 3) == 7 && (6 ^ 3) == 5
int logic;
#endif
#if 0
#error "dead branches are not evaluated"
#if 1
int skipped_nested;
#else
int skipped_too;
#endif
#elif 1
int taken_elif;
#elif 1
int second_elif_not_taken;
#else
int not_else;
#endif
/* A string in a dead branch may hold an apostrophe; the group is still
   tokenized. */
#if 0
char *s = "don't";
int x = '"';
#endif
int end;
