// Goose compiler — the interpreter's translation unit (docs/design/interpreter.md).
// The interpreter runs the C a JIT run would hand TinyCC, in place of it, and
// is built as a unit of its own so the compiler's single unit does not grow
// with it. Step 1 of the plan (§14): the preprocessor.
#include "../includes.h"
#include "../utils.h"
#include "pp.h"
#include "ctypes.h"
#include "cast.h"
#include "cparse.h"
