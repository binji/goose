// Goose compiler — the interpreter's entry point (docs/design/interpreter.md
// §10), what main.cpp calls where it would call RunJit. The interpreter is
// a unit of its own (interp.cpp); this is all the rest of the compiler sees
// of it.
#pragma once

namespace goose {

// Runs the C a JIT run would hand TinyCC: preprocesses it with the
// interpreter's prelude, parses and lowers it, and runs its main with
// `progargs` after argv[0], returning what the program returned or exited
// with. The text must have been assembled with GS_INTERP defined. An
// `--include` header is read relative to the directory the command runs
// in, as TinyCC would read it. Throws CompileError where the C does not
// parse, which is a bug in codegen, the runtime or the interpreter rather
// than in the program; a fault during the run is reported on stderr and
// exits with 134, as an abort would.
int RunInterp(const string &csrc, const string &progname, const vector<string> &progargs);

}  // namespace goose
