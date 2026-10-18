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
              const vector<const goose_thunk *> &thunks);

// In pieces, for a host that calls in repeatedly (the page, §9, through
// web_entry.cpp): with InterpResumable(true), RunInterp returns
// kInterpSuspended as soon as the program yields, with the run kept, and
// each InterpResume continues it, returning the same until the program
// ends with its status. Natively the driver runs a program whole.
constexpr int kInterpSuspended = -2;
void InterpResumable(bool on);
bool InterpSuspended();
int InterpResume();

}  // namespace goose
