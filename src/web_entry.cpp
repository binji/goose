// Goose compiler — the Wasm module's entry points (docs/design/interpreter.md
// §8, §9). The module is a WASI reactor: nothing runs at instantiation, and
// the page calls in. It writes a command line into the buffer goose_argbuf
// hands it and calls goose_run, which splits the line into argv and runs
// Main as main does; where the program the command runs yields (a frame's
// end, the step budget), goose_run returns kInterpSuspended with the run
// kept, and the page calls goose_resume from its animation callback until
// something else comes back: the exit status.
#include "includes.h"
#include "utils.h"
#include "interp/interp.h"
#include <unistd.h>

namespace goose {
int Main(int argc, char **argv);
}

extern "C" {

// Room for the command line: `size` bytes, kept until the next call.
char *goose_argbuf(size_t size) {
    static std::string buf;
    buf.assign(size + 1, '\0');
    return buf.data();
}

// Runs the command in the buffer (the arguments after the program's name,
// as a shell would split them: spaces separate, quotes group). Returns the
// exit status, or kInterpSuspended with the program parked.
int goose_run(const char *cmdline) {
    std::vector<std::string> args = { "goose" };
    std::string cur;
    bool inword = false;
    char quote = 0;
    for (const char *p = cmdline; *p; p++) {
        if (quote) {
            if (*p == quote) quote = 0;
            else cur += *p;
        } else if (*p == '"' || *p == '\'') {
            quote = *p;
            inword = true;
        } else if (*p == ' ' || *p == '\t' || *p == '\n') {
            if (inword) args.push_back(cur);
            cur.clear();
            inword = false;
        } else {
            cur += *p;
            inword = true;
        }
    }
    if (inword) args.push_back(cur);
    // The working directory the program's relative paths resolve against,
    // which WASI has no notion of: wasi-libc keeps one in the module.
    if (auto cwd = getenv("GOOSE_CWD")) (void)chdir(cwd);
    std::vector<char *> argv;
    for (auto &a : args) argv.push_back(a.data());
    argv.push_back(nullptr);
    goose::InterpResumable(true);
    int code = goose::Main((int)argv.size() - 1, argv.data());
    fflush(stdout);
    fflush(stderr);
    return code;
}

int goose_resume(void) {
    int code = goose::InterpResume();
    fflush(stdout);
    fflush(stderr);
    return code;
}

// A compiled run (docs/design/wasm_backend.md §6): goose_run returned
// kWasmReady with the module here; the page instantiates it on this
// module's exports and runs its _start, then asks goose_wasm_finish, with
// the engine's message where the run ended in a trap, for the status, or
// for the reporter to call first.
const char *goose_module_ptr(void) { return goose::InterpModule().data(); }
size_t goose_module_len(void) { return goose::InterpModule().size(); }

int goose_wasm_finish(const char *trap) {
    int code = goose::InterpWasmFinish(trap);
    fflush(stdout);
    fflush(stderr);
    return code;
}

const char *goose_wasm_report_name(void) { return goose::InterpWasmReportName(); }
uint64_t goose_wasm_report_arg(void) { return goose::InterpWasmReportArg(); }

}  // extern "C"
