// Goose compiler — the interpreter's translation unit (docs/design/interpreter.md).
// The interpreter runs the C a JIT run would hand TinyCC, in place of it, and
// is built as a unit of its own so the compiler's single unit does not grow
// with it. Steps 1 to 6 of the plan (§14): the preprocessor, the parser,
// lowering, the VM and its C library, and RunInterp, the entry point the
// driver calls where it would call RunJit.
#include "../includes.h"
#include "../utils.h"
#include "thunk.h"
#include "pp.h"
#include "ctypes.h"
#include "cast.h"
#include "cparse.h"
#include "bytecode.h"
#include "lower.h"
#include "vm.h"
#include "prelude.h"
#include "clib.h"
#include <filesystem>
#include "interp.h"

namespace goose {

// The program's resource directory, as RunJit defines it (jit.h): the
// entry source's directory, absolute, as a C string literal in fixed-width
// octal escapes, with a slash last.
static string ResourceDirDefine(const string &progname) {
    std::error_code ec;
    auto root = filesystem::absolute(filesystem::path(progname), ec).parent_path();
    if (ec) throw CompileError { "cannot locate the entry source directory" };
    auto bytes = root.generic_u8string();
    string define = "#define GS_JIT_RESOURCE_DIR \"";
    for (auto ch : bytes) {
        char escaped[5];
        snprintf(escaped, sizeof escaped, "\\%03o", (unsigned)(uint8_t)ch);
        define += escaped;
    }
    if (bytes.empty() || bytes.back() != u8'/') define += '/';
    return define + "\"\n";
}

// What a thunk decodes a handle through (§7.1): the bytes of a VM segment,
// a pointer good for the duration of the call.
extern "C" void *goose_vm_decode(goose_vm *vm, uint64_t handle, uint64_t len) {
    if (!handle) return nullptr;
    return reinterpret_cast<interp::VM *>(vm)->Access(handle, len);
}

// A run kept between the host's calls (InterpResumable): everything the VM
// points into, and the VM.
struct InterpRun {
    interp::TypeTable types;
    vector<interp::PPToken> toks;
    unique_ptr<interp::CParser> parser;
    interp::Program prog;
    unique_ptr<interp::VM> vm;
};
static unique_ptr<InterpRun> pending;
static bool resumable = false;

void InterpResumable(bool on) { resumable = on; }
bool InterpSuspended() { return pending != nullptr; }

int InterpResume() {
    if (!pending) return 0;
    if (!pending->vm->Resume()) return kInterpSuspended;
    int status = pending->vm->status;
    pending.reset();
    return status;
}

int RunInterp(const string &csrc, const string &progname, const vector<string> &progargs,
              const vector<const goose_thunk *> &thunks) {
    using namespace interp;
    // <name> is the prelude's; "name" is an --include header, named as the
    // command named it, so relative to where the command runs.
    Preprocessor pp(PreludeIncluder([](const string &name, bool system, const string &from,
                                       string &text, string &path) {
        if (system) return false;
        for (auto &dir : { from, string() }) {
            auto p = dir.empty() ? name : cat(dir, "/", name);
            if (LoadFile(p, text)) {
                path = p;
                return true;
            }
        }
        return false;
    }));
    // The parser owns the declarations and the types, which the program's
    // functions point at for as long as the VM runs (an external call lays
    // its arguments out by its prototype; a fault report names its file).
    auto run = make_unique<InterpRun>();
    try {
        run->toks = pp.Run(ResourceDirDefine(progname) + csrc, "<generated C>");
        run->parser = make_unique<CParser>(run->toks, pp.files, run->types);
        auto &unit = run->parser->ParseUnit();
        run->prog = Lowerer(unit, pp.files).Run();
    } catch (CompileError &e) {
        throw CompileError { cat("interpreter: ", e.msg) };
    }
    // The runtime makes stdout unbuffered so that output is never lost to
    // an abort and interleaves with stderr; its setvbuf reaches the VM's
    // adapter, not the process, so the process does the same here.
    setvbuf(stdout, nullptr, _IONBF, 0);
    run->vm = make_unique<VM>(run->prog);
    auto &vm = *run->vm;
    for (auto table : thunks)
        for (auto t = table; t->name; t++) vm.thunks[t->name] = t;
    vector<string> args;
    args.push_back(progname);
    args.insert(args.end(), progargs.begin(), progargs.end());
    // In pieces, as the page runs it (§9), so that every run exercises the
    // suspension; natively the pieces just follow one another, and for a
    // host that resumes the run itself the first yield hands it back.
    vm.budget = 1 << 20;
    vm.Start(args);
    if (resumable) {
        pending = std::move(run);
        return InterpResume();
    }
    while (!vm.Resume()) {}
    return vm.status;
}

}  // namespace goose
