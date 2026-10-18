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

}  // extern "C"
