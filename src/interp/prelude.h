// Goose compiler — the interpreter's prelude (docs/design/interpreter.md §6,
// step 4 of §14): the standard headers the interpreted C includes, as text
// the preprocessor serves for `#include <name>`. They declare the C library
// the VM binds (vm.h) and define the few things that are code rather than
// adapters: qsort, whose comparison is interpreted, and rand. The target is
// the one ctypes.h lays out, LP64, so `long` is the 64-bit type throughout.
// Constants that name OS facts (O_CREAT, EEXIST, S_IFDIR) have Linux's
// values; the adapters translate them for the host, so a program sees the
// same values on every platform the interpreter runs on.
#pragma once

namespace goose {
namespace interp {

namespace prelude {

// Shared by the headers that need them, once.
inline const char *types = R"(
#ifndef GS_PRELUDE_TYPES
#define GS_PRELUDE_TYPES
typedef unsigned long size_t;
typedef long ssize_t;
typedef long ptrdiff_t;
typedef long intptr_t;
typedef unsigned long uintptr_t;
typedef long off_t;
typedef long time_t;
typedef unsigned int mode_t;
typedef int clockid_t;
typedef struct FILE FILE;
#define NULL ((void *)0)
#endif
)";

inline const char *stdint_h = R"(
#ifndef GS_PRELUDE_STDINT
#define GS_PRELUDE_STDINT
#include <__types.h>
typedef signed char int8_t;
typedef unsigned char uint8_t;
typedef short int16_t;
typedef unsigned short uint16_t;
typedef int int32_t;
typedef unsigned int uint32_t;
typedef long int64_t;
typedef unsigned long uint64_t;
typedef long intmax_t;
typedef unsigned long uintmax_t;
#define INT8_MIN (-128)
#define INT8_MAX 127
#define UINT8_MAX 255
#define INT16_MIN (-32768)
#define INT16_MAX 32767
#define UINT16_MAX 65535
#define INT32_MIN (-2147483647 - 1)
#define INT32_MAX 2147483647
#define UINT32_MAX 4294967295U
#define INT64_MIN (-9223372036854775807L - 1)
#define INT64_MAX 9223372036854775807L
#define UINT64_MAX 18446744073709551615UL
#define INTPTR_MIN INT64_MIN
#define INTPTR_MAX INT64_MAX
#define UINTPTR_MAX UINT64_MAX
#define INTMAX_MIN INT64_MIN
#define INTMAX_MAX INT64_MAX
#define UINTMAX_MAX UINT64_MAX
#define PTRDIFF_MIN INT64_MIN
#define PTRDIFF_MAX INT64_MAX
#define SIZE_MAX UINT64_MAX
#define INT8_C(c) c
#define INT16_C(c) c
#define INT32_C(c) c
#define INT64_C(c) c##L
#define UINT8_C(c) c
#define UINT16_C(c) c
#define UINT32_C(c) c##U
#define UINT64_C(c) c##UL
#define INTMAX_C(c) c##L
#define UINTMAX_C(c) c##UL
#endif
)";

inline const char *stddef_h = R"(
#ifndef GS_PRELUDE_STDDEF
#define GS_PRELUDE_STDDEF
#include <__types.h>
#define offsetof(t, f) ((size_t)&((t *)0)->f)
#endif
)";

inline const char *stdbool_h = R"(
#ifndef GS_PRELUDE_STDBOOL
#define GS_PRELUDE_STDBOOL
#define bool _Bool
#define true 1
#define false 0
#define __bool_true_false_are_defined 1
#endif
)";

inline const char *limits_h = R"(
#ifndef GS_PRELUDE_LIMITS
#define GS_PRELUDE_LIMITS
#define CHAR_BIT 8
#define SCHAR_MIN (-128)
#define SCHAR_MAX 127
#define UCHAR_MAX 255
#define CHAR_MIN SCHAR_MIN
#define CHAR_MAX SCHAR_MAX
#define SHRT_MIN (-32768)
#define SHRT_MAX 32767
#define USHRT_MAX 65535
#define INT_MIN (-2147483647 - 1)
#define INT_MAX 2147483647
#define UINT_MAX 4294967295U
#define LONG_MIN (-9223372036854775807L - 1)
#define LONG_MAX 9223372036854775807L
#define ULONG_MAX 18446744073709551615UL
#define LLONG_MIN (-9223372036854775807LL - 1)
#define LLONG_MAX 9223372036854775807LL
#define ULLONG_MAX 18446744073709551615ULL
#endif
)";

inline const char *string_h = R"(
#ifndef GS_PRELUDE_STRING
#define GS_PRELUDE_STRING
#include <__types.h>
void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);
void *memchr(const void *p, int c, size_t n);
size_t strlen(const char *s);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);
char *strstr(const char *s, const char *sub);
char *strcpy(char *dst, const char *src);
char *strncpy(char *dst, const char *src, size_t n);
char *strcat(char *dst, const char *src);
char *strdup(const char *s);
char *strerror(int err);
#endif
)";

inline const char *stdio_h = R"(
#ifndef GS_PRELUDE_STDIO
#define GS_PRELUDE_STDIO
#include <__types.h>
#define EOF (-1)
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#define _IOFBF 0
#define _IOLBF 1
#define _IONBF 2
#define BUFSIZ 8192
extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;
FILE *fopen(const char *path, const char *mode);
int fclose(FILE *f);
size_t fread(void *p, size_t size, size_t n, FILE *f);
size_t fwrite(const void *p, size_t size, size_t n, FILE *f);
int fgetc(FILE *f);
char *fgets(char *s, int n, FILE *f);
int fputc(int c, FILE *f);
int fputs(const char *s, FILE *f);
int getchar(void);
int putchar(int c);
int puts(const char *s);
int fflush(FILE *f);
int feof(FILE *f);
int ferror(FILE *f);
int fseek(FILE *f, long off, int whence);
long ftell(FILE *f);
int setvbuf(FILE *f, char *buf, int mode, size_t size);
int remove(const char *path);
int rename(const char *from, const char *to);
int printf(const char *fmt, ...);
int fprintf(FILE *f, const char *fmt, ...);
int sprintf(char *s, const char *fmt, ...);
int snprintf(char *s, size_t n, const char *fmt, ...);
#define getc(f) fgetc(f)
#define putc(c, f) fputc(c, f)
#endif
)";

inline const char *stdlib_h = R"(
#ifndef GS_PRELUDE_STDLIB
#define GS_PRELUDE_STDLIB
#include <__types.h>
#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#define RAND_MAX 2147483647
void *malloc(size_t n);
void *calloc(size_t n, size_t size);
void *realloc(void *p, size_t n);
void free(void *p);
void exit(int status);
void _exit(int status);
void abort(void);
char *getenv(const char *name);
int abs(int x);
long labs(long x);
long long llabs(long long x);
int atoi(const char *s);
long atol(const char *s);
double atof(const char *s);
long strtol(const char *s, char **end, int base);
long long strtoll(const char *s, char **end, int base);
unsigned long strtoul(const char *s, char **end, int base);
unsigned long long strtoull(const char *s, char **end, int base);
double strtod(const char *s, char **end);
void *bsearch(const void *key, const void *base, size_t n, size_t size,
              int (*cmp)(const void *, const void *));
void qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *));
int rand(void);
void srand(unsigned seed);

/* qsort runs interpreted, since its comparison does: a heap sort, in place
   and without recursion, over elements swapped a byte at a time. */
static void gs_prelude_swap(char *a, char *b, size_t size) {
    while (size--) { char t = *a; *a++ = *b; *b++ = t; }
}
static void gs_prelude_sift(char *base, size_t size, size_t root, size_t n,
                            int (*cmp)(const void *, const void *)) {
    for (;;) {
        size_t child = 2 * root + 1;
        if (child >= n) return;
        if (child + 1 < n && cmp(base + child * size, base + (child + 1) * size) < 0) child++;
        if (cmp(base + root * size, base + child * size) >= 0) return;
        gs_prelude_swap(base + root * size, base + child * size, size);
        root = child;
    }
}
void qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *)) {
    char *b = (char *)base;
    if (n < 2 || size == 0) return;
    for (size_t i = n / 2; i-- > 0;) gs_prelude_sift(b, size, i, n, cmp);
    for (size_t end = n - 1; end > 0; end--) {
        gs_prelude_swap(b, b + end * size, size);
        gs_prelude_sift(b, size, 0, end, cmp);
    }
}
void *bsearch(const void *key, const void *base, size_t n, size_t size,
              int (*cmp)(const void *, const void *)) {
    const char *b = (const char *)base;
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int c = cmp(key, b + mid * size);
        if (c == 0) return (void *)(b + mid * size);
        if (c < 0) hi = mid; else lo = mid + 1;
    }
    return NULL;
}
/* The C standard's example generator, so a seed gives the same sequence
   everywhere. */
static unsigned long gs_prelude_rand_next = 1;
int rand(void) {
    gs_prelude_rand_next = gs_prelude_rand_next * 1103515245 + 12345;
    return (int)((gs_prelude_rand_next / 65536) % 32768);
}
void srand(unsigned seed) { gs_prelude_rand_next = seed; }
#endif
)";

inline const char *math_h = R"(
#ifndef GS_PRELUDE_MATH
#define GS_PRELUDE_MATH
#define HUGE_VAL 1e999
#define HUGE_VALF ((float)1e999)
#define INFINITY ((float)1e999)
#define NAN (INFINITY - INFINITY)
#define M_E 2.7182818284590452354
#define M_PI 3.14159265358979323846
#define M_PI_2 1.57079632679489661923
#define M_SQRT2 1.41421356237309504880
double sqrt(double x); double cbrt(double x);
double sin(double x); double cos(double x); double tan(double x);
double asin(double x); double acos(double x); double atan(double x);
double sinh(double x); double cosh(double x); double tanh(double x);
double exp(double x); double exp2(double x); double expm1(double x);
double log(double x); double log2(double x); double log10(double x); double log1p(double x);
double fabs(double x); double trunc(double x); double floor(double x); double ceil(double x);
double round(double x); double rint(double x); double nearbyint(double x);
double pow(double x, double y); double fmod(double x, double y); double atan2(double y, double x);
double hypot(double x, double y); double fmin(double x, double y); double fmax(double x, double y);
double copysign(double x, double y);
double frexp(double x, int *e); double ldexp(double x, int e); double modf(double x, double *i);
float sqrtf(float x); float cbrtf(float x);
float sinf(float x); float cosf(float x); float tanf(float x);
float asinf(float x); float acosf(float x); float atanf(float x);
float sinhf(float x); float coshf(float x); float tanhf(float x);
float expf(float x); float exp2f(float x); float expm1f(float x);
float logf(float x); float log2f(float x); float log10f(float x); float log1pf(float x);
float fabsf(float x); float truncf(float x); float floorf(float x); float ceilf(float x);
float roundf(float x); float rintf(float x); float nearbyintf(float x);
float powf(float x, float y); float fmodf(float x, float y); float atan2f(float y, float x);
float hypotf(float x, float y); float fminf(float x, float y); float fmaxf(float x, float y);
float copysignf(float x, float y);
float frexpf(float x, int *e); float ldexpf(float x, int e); float modff(float x, float *i);
/* Functions where the standard header has macros; a float argument is
   promoted, which changes none of the answers. */
int isnan(double x); int isinf(double x); int isfinite(double x); int signbit(double x);
#endif
)";

inline const char *ctype_h = R"(
#ifndef GS_PRELUDE_CTYPE
#define GS_PRELUDE_CTYPE
static int isdigit(int c) { return c >= '0' && c <= '9'; }
static int isupper(int c) { return c >= 'A' && c <= 'Z'; }
static int islower(int c) { return c >= 'a' && c <= 'z'; }
static int isalpha(int c) { return isupper(c) || islower(c); }
static int isalnum(int c) { return isalpha(c) || isdigit(c); }
static int isxdigit(int c) { return isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
static int isspace(int c) { return c == ' ' || (c >= '\t' && c <= '\r'); }
static int isprint(int c) { return c >= 32 && c < 127; }
static int ispunct(int c) { return isprint(c) && c != ' ' && !isalnum(c); }
static int toupper(int c) { return islower(c) ? c - 32 : c; }
static int tolower(int c) { return isupper(c) ? c + 32 : c; }
#endif
)";

inline const char *time_h = R"(
#ifndef GS_PRELUDE_TIME
#define GS_PRELUDE_TIME
#include <__types.h>
#define CLOCK_REALTIME 0
#define CLOCK_MONOTONIC 1
struct timespec { time_t tv_sec; long tv_nsec; };
int clock_gettime(clockid_t clock, struct timespec *ts);
int nanosleep(const struct timespec *req, struct timespec *rem);
time_t time(time_t *t);
#endif
)";

inline const char *errno_h = R"(
#ifndef GS_PRELUDE_ERRNO
#define GS_PRELUDE_ERRNO
int *__errno_location(void);
#define errno (*__errno_location())
#define EPERM 1
#define ENOENT 2
#define EINTR 4
#define EIO 5
#define EBADF 9
#define EAGAIN 11
#define ENOMEM 12
#define EACCES 13
#define EEXIST 17
#define ENOTDIR 20
#define EISDIR 21
#define EINVAL 22
#define EMFILE 24
#define ENOSPC 28
#define ERANGE 34
#define ENOTEMPTY 39
#endif
)";

inline const char *fcntl_h = R"(
#ifndef GS_PRELUDE_FCNTL
#define GS_PRELUDE_FCNTL
#include <__types.h>
#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR 2
#define O_CREAT 0100
#define O_EXCL 0200
#define O_TRUNC 01000
#define O_APPEND 02000
int open(const char *path, int flags, ...);
#endif
)";

inline const char *unistd_h = R"(
#ifndef GS_PRELUDE_UNISTD
#define GS_PRELUDE_UNISTD
#include <__types.h>
ssize_t read(int fd, void *buf, size_t n);
ssize_t write(int fd, const void *buf, size_t n);
int close(int fd);
int fsync(int fd);
int unlink(const char *path);
int rmdir(const char *path);
#endif
)";

inline const char *sys_stat_h = R"(
#ifndef GS_PRELUDE_SYS_STAT
#define GS_PRELUDE_SYS_STAT
#include <__types.h>
#define S_IFMT 0170000
#define S_IFDIR 0040000
#define S_IFREG 0100000
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
struct stat { mode_t st_mode; off_t st_size; time_t st_mtime; };
int stat(const char *path, struct stat *st);
int mkdir(const char *path, mode_t mode);
#endif
)";

inline const char *dirent_h = R"(
#ifndef GS_PRELUDE_DIRENT
#define GS_PRELUDE_DIRENT
#include <__types.h>
typedef struct DIR DIR;
struct dirent { char d_name[256]; };
DIR *opendir(const char *path);
struct dirent *readdir(DIR *d);
int closedir(DIR *d);
#endif
)";

inline const char *empty_h = "\n";

}  // namespace prelude

// The text of a standard header, or null for a name the prelude lacks.
inline const char *PreludeHeader(string_view name) {
    static const pair<string_view, const char *> table[] = {
        { "__types.h", prelude::types },
        { "stdint.h", prelude::stdint_h },
        { "stddef.h", prelude::stddef_h },
        { "stdbool.h", prelude::stdbool_h },
        { "limits.h", prelude::limits_h },
        { "string.h", prelude::string_h },
        { "stdio.h", prelude::stdio_h },
        { "stdlib.h", prelude::stdlib_h },
        { "math.h", prelude::math_h },
        { "ctype.h", prelude::ctype_h },
        { "time.h", prelude::time_h },
        { "errno.h", prelude::errno_h },
        { "fcntl.h", prelude::fcntl_h },
        { "unistd.h", prelude::unistd_h },
        { "sys/stat.h", prelude::sys_stat_h },
        { "dirent.h", prelude::dirent_h },
        // Headers the runtime includes for things the prelude declares elsewhere.
        { "sys/types.h", prelude::types },
        { "inttypes.h", prelude::stdint_h },
        { "assert.h", prelude::empty_h },
        { "stdarg.h", prelude::empty_h },
    };
    for (auto &[n, text] : table) if (n == name) return text;
    return nullptr;
}

// An include resolver: `<name>` from the prelude, and anything else, a
// `"name"` or a system header the prelude lacks, through `rest` if given.
inline PPIncludeFn PreludeIncluder(PPIncludeFn rest = nullptr) {
    return [rest](const string &name, bool system, const string &from, string &text, string &path) {
        if (system) {
            if (auto h = PreludeHeader(name)) {
                text = h;
                path = cat("<", name, ">");
                return true;
            }
        }
        return rest && rest(name, system, from, text, path);
    };
}

}  // namespace interp
}  // namespace goose
