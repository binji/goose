/* Every header of the prelude includes cleanly, more than once, together,
   and defines what programs reach for. */
#include <assert.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <stdint.h>
#include <stdio.h>

struct Rec { char c; int64_t v; uint8_t tail[3]; };

int main(void) {
    printf("sizes: %zu %zu %zu %zu %zu %zu\n", sizeof(size_t), sizeof(ptrdiff_t), sizeof(intptr_t), sizeof(int64_t),
           sizeof(time_t), sizeof(off_t));
    printf("offsetof: %zu %zu %zu\n", offsetof(struct Rec, c), offsetof(struct Rec, v), offsetof(struct Rec, tail));
    printf("NULL is zero: %d, EOF %d\n", NULL == 0, EOF);
    bool t = true, f = false;
    printf("bool: %d %d %zu\n", t, f, sizeof(bool));
    printf("limits: %d %d %u %lld %llu %d\n", INT_MIN, INT_MAX, UINT_MAX, LLONG_MIN, ULLONG_MAX, CHAR_BIT);
    printf("stdint: %d %d %u %lld %llu %llu\n", INT8_MIN, INT16_MAX, UINT32_MAX, INT64_MIN, UINT64_MAX, SIZE_MAX);
    printf("stdint constants: %lld %llu\n", INT64_C(-5000000000), UINT64_C(18446744073709551615));
    printf("exit codes: %d %d, RAND_MAX %d\n", EXIT_SUCCESS, EXIT_FAILURE, RAND_MAX);
    printf("seek: %d %d %d, buffering %d %d %d\n", SEEK_SET, SEEK_CUR, SEEK_END, _IOFBF, _IOLBF, _IONBF);
    printf("open flags: %d %d %d %d\n", O_RDONLY, O_WRONLY | O_CREAT, O_EXCL, O_APPEND);
    printf("errno values: %d %d %d, starts at %d\n", ENOENT, EEXIST, EINTR, errno);
    printf("S_ISDIR: %d %d\n", S_ISDIR(S_IFDIR | 0755), S_ISDIR(S_IFREG | 0644));
    printf("clocks: %d %d\n", CLOCK_REALTIME, CLOCK_MONOTONIC);
    printf("math constants: %.5f %.5f\n", M_PI, M_E);
    printf("dirent name room: %zu\n", sizeof(((struct dirent *)0)->d_name));
    printf("stream handles: %d %d %d\n", stdin != NULL, stdout != NULL, stderr != NULL && stderr != stdout);
    putc('o', stdout);
    putchar('k');
    putchar('\n');
    return 0;
}
