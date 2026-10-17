/* "file" relative to the including file, nested includes, an include guard,
   <file> from the system directory, and the token after an expansion that
   ends an included file. */
#include "inc/a.h"
#include "inc/guard.h"
#include "inc/guard.h"
#include <stddef.h>
int a = A_VALUE, b = B_VALUE, guards = GUARD_COUNT;
size_t n = NULL;
#define WHERE __FILE__ __LINE__
const char *here = WHERE;
