// Goose compiler — the interpreter's entry point (docs/design/interpreter.md
// §10), what main.cpp calls where it would call RunJit. The interpreter is
// a unit of its own (interp.cpp); this is all the rest of the compiler sees
// of it.
#pragma once

#include "thunk.h"

namespace goose {

// Runs the C a JIT run would hand TinyCC: preprocesses it with the
// interpreter's prelude, parses and lowers it, and runs its main with
// `progargs` after argv[0], returning what the program returned or exited
// with. The text must have been assembled with GS_INTERP defined. An
// `--include` header is read relative to the directory the command runs
// in, as TinyCC would read it. `thunks` are the thunk tables of the native
// packages the program reaches (builtin.h), through which it calls their
// C (docs/design/interpreter.md §7.1). Throws CompileError where the C does
// not parse, which is a bug in codegen, the runtime or the interpreter
// rather than in the program; a fault during the run is reported on stderr
// and exits with 134, as an abort would.
int RunInterp(const string &csrc, const string &progname, const vector<string> &progargs,
              const vector<const goose_thunk *> &thunks, const string &cname = "<generated C>",
              const string &argv0 = "");

// In pieces, for a host that calls in repeatedly (the page, §9, through
// web_entry.cpp): with InterpResumable(true), RunInterp returns
// kInterpSuspended as soon as the program yields, with the run kept, and
// each InterpResume continues it, returning the same until the program
// ends with its status. Natively the driver runs a program whole.
constexpr int kInterpSuspended = -2;
void InterpResumable(bool on);
bool InterpSuspended();
int InterpResume();

// The WebAssembly backend (wasmgen.h, docs/design/wasm_backend.md): the
// same text compiled to a module over the bytecode. EmitWasm writes the
// module's bytes; with InterpCompiled(true), RunInterp emits them instead
// of running the loop, sets the run up and returns kWasmReady, and the
// host (web_entry.cpp) instantiates the module on the exports of §6 and
// runs its _start. What ends the run is a trap after the host recorded
// why; InterpWasmFinish then gives the status, printing a fault's message,
// or kWasmReport, asking the host to call the exported reporter
// InterpWasmReportName names with InterpWasmReportArg and finish again.
// `trap` is the engine's message where the run ended in a trap the host
// did not record, or null.
string EmitWasm(const string &csrc, const string &progname, const string &cname = "<generated C>");
void InterpCompiled(bool on);
constexpr int kWasmReady = -3;
constexpr int kWasmReport = -4;
const string &InterpModule();
int InterpWasmFinish(const char *trap);
const char *InterpWasmReportName();
uint64_t InterpWasmReportArg();

}  // namespace goose
