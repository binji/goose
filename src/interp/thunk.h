// Goose compiler — the interpreter's thunk ABI (docs/design/interpreter.md
// §7.1): what scripts/gen_thunks.py writes into goose_thunks_<name>.c for a
// builtin package, and what the VM calls. A thunk takes the arguments as
// the VM laid them out, decodes the pointers through goose_vm_decode and
// stores the result; `yields` says the VM hands control back to its caller
// after the call (a frame's end in the browser, §7.2). On its own so that
// the VM and the tests can include it without the generated package table.
#pragma once

namespace goose {

struct goose_vm;
using goose_thunk_fn = void (*)(goose_vm *vm, const uint8_t *args, uint8_t *ret);
struct goose_thunk {
    const char *name;
    goose_thunk_fn fn;
    int yields;
};

}  // namespace goose
