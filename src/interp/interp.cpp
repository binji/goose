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
#include "wasmgen.h"
#include <filesystem>
#include "interp.h"

namespace goose {

// The program's resource directory, as RunJit defines it (jit.h): the
// entry source's directory, absolute, as a C string literal in fixed-width
// octal escapes, with a slash last; as a -D definition for the preprocessor,
// so that the text's line numbers stay its own.
static string ResourceDirDefine(const string &progname) {
    std::error_code ec;
    auto root = filesystem::absolute(filesystem::path(progname), ec).parent_path();
    if (ec) throw CompileError { "cannot locate the entry source directory" };
    auto bytes = root.generic_u8string();
    string define = "GS_JIT_RESOURCE_DIR=\"";
    for (auto ch : bytes) {
        char escaped[5];
        snprintf(escaped, sizeof escaped, "\\%03o", (unsigned)(uint8_t)ch);
        define += escaped;
    }
    if (bytes.empty() || bytes.back() != u8'/') define += '/';
    return define + "\"";
}

// What a thunk decodes a handle through (§7.1): the bytes of a VM segment,
// a pointer good for the duration of the call.
extern "C" void *goose_vm_decode(goose_vm *vm, uint64_t handle, uint64_t len) {
    if (!handle) return nullptr;
    return reinterpret_cast<interp::VM *>(vm)->Access(handle, len);
}

// A run kept between the host's calls (InterpResumable, InterpCompiled):
// everything the VM points into, the VM, and for a compiled run the module
// and what ended it.
struct InterpRun {
    interp::TypeTable types;
    vector<interp::PPToken> toks;
    unique_ptr<interp::CParser> parser;
    interp::Program prog;
    unique_ptr<interp::VM> vm;
    string module;
    // Why compiled code stopped, recorded before the trap that unwinds it.
    enum { NONE, EXIT, FAULT, REPORT } stop = NONE;
    int status = 0;
    string fault;
    int reporter = -1;
    vector<uint64_t> reportargs;
};
static unique_ptr<InterpRun> pending;
static bool resumable = false, compiled = false;

void InterpResumable(bool on) { resumable = on; }
void InterpCompiled(bool on) { compiled = on; }
bool InterpSuspended() { return pending != nullptr; }

int InterpResume() {
    if (!pending) return 0;
    if (!pending->vm->Resume()) return kInterpSuspended;
    int status = pending->vm->status;
    pending.reset();
    return status;
}

// The text preprocessed, parsed and lowered into a run.
static unique_ptr<InterpRun> Lower(const string &csrc, const string &progname, const string &cname) {
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
        pp.Define(ResourceDirDefine(progname));
        run->toks = pp.Run(csrc, cname);
        run->parser = make_unique<CParser>(run->toks, pp.files, run->types);
        auto &unit = run->parser->ParseUnit();
        run->prog = Lowerer(unit, pp.files).Run();
    } catch (CompileError &e) {
        throw CompileError { cat("interpreter: ", e.msg) };
    }
    return run;
}

string EmitWasm(const string &csrc, const string &progname, const string &cname) {
    auto run = Lower(csrc, progname, cname);
    return interp::WasmGen(run->prog).Run();
}

// ---- The host of compiled code (wasmgen.h; docs/design/wasm_backend.md §6, §7)

// A host entry point: what the VM throws is recorded as the reason the
// run stops and the code is trapped, which unwinds the module's frames to
// the host's caller, who asks InterpWasmFinish.
template <class F> static auto HostCall(F f) -> decltype(f()) {
    try {
        return f();
    } catch (interp::Exit &e) {
        pending->stop = InterpRun::EXIT;
        pending->status = e.status;
    } catch (interp::Fault &f) {
        pending->stop = InterpRun::FAULT;
        pending->fault = f.msg;
    } catch (interp::Reroute &r) {
        pending->stop = InterpRun::REPORT;
        pending->reporter = r.fn;
        pending->reportargs = r.args;
    }
#if defined(__GNUC__) || defined(__clang__)
    __builtin_trap();
#else
    abort();
#endif
}

extern "C" {

uint64_t goose_native(uint32_t fi, uint32_t args, uint32_t nargs) {
    return HostCall([&] {
        auto &vm = *pending->vm;
        return vm.CallExternal(vm.prog.funcs[fi], reinterpret_cast<const uint64_t *>((uintptr_t)args), (int)nargs);
    });
}

uint32_t goose_access_slow(uint64_t h, uint32_t n) {
    return HostCall([&] { return (uint32_t)(uintptr_t)pending->vm->AccessSlow(h, n); });
}

uint64_t goose_static_handle(uint32_t i) {
    return HostCall([&] { return pending->vm->StaticHandle(i); });
}

uint64_t goose_func_handle(uint32_t i, uint32_t tableidx) {
    return HostCall([&] { return pending->vm->FuncHandle(i, tableidx); });
}

uint64_t goose_info(uint32_t which) {
    return HostCall([&]() -> uint64_t {
        auto &vm = *pending->vm;
        switch (which) {
            case interp::INFO_SEGTAB: return (uintptr_t)vm.segtab.data();
            case interp::INFO_FRAMEBASE: return (uintptr_t)vm.segs[vm.frameseg].mem.data();
            case interp::INFO_FRAMELIMIT: return vm.framelimit;
            case interp::INFO_FRAMESEG: return interp::Handle(vm.frameseg, 0);
            case interp::INFO_ARGC: return vm.argc;
            case interp::INFO_ARGV: return vm.argvh;
            default: return 0;
        }
    });
}

void goose_stop(int32_t status) {
    pending->stop = InterpRun::EXIT;
    pending->status = status;
}

void goose_fault(int32_t code, uint64_t a, uint64_t b) {
    HostCall([&]() -> int {
        auto &vm = *pending->vm;
        auto describe = [&](uint64_t h) { return interp::SegOf(h) ? vm.Describe(vm.Seg(h)) : string("null"); };
        switch (code) {
            case interp::FAULT_NATIVE_OVERFLOW: throw interp::Fault { "native call stack overflow" };
            case interp::FAULT_BAD_CALL: throw interp::Fault { cat("call through a pointer to ", describe(a)) };
            case interp::FAULT_PTRDIFF:
                throw interp::Fault { cat("difference of pointers into different segments (", describe(a), " and ", describe(b), ")") };
            default: throw interp::Fault { cat("fault ", code) };
        }
    });
}

}  // extern "C"

const string &InterpModule() {
    static const string none;
    return pending ? pending->module : none;
}

const char *InterpWasmReportName() {
    if (!pending || pending->reporter < 0) return nullptr;
    return pending->prog.funcs[(size_t)pending->reporter].name.c_str();
}

uint64_t InterpWasmReportArg() {
    return pending && !pending->reportargs.empty() ? pending->reportargs[0] : 0;
}

int InterpWasmFinish(const char *trap) {
    if (!pending) return 0;
    auto &run = *pending;
    string msg = trap ? trap : "";
    if (run.stop == InterpRun::NONE && !msg.empty()) {
        // A trap the host did not record: the engine's own, in the VM's
        // words. Its stack limit is the native call stack overflow the
        // runtime reports through its own function, where the program
        // has one: that is asked for as a reporter, with the message kept
        // for a program without.
        if (msg.find("divide by zero") != string::npos) msg = "division by zero";
        else if (msg.find("unrepresentable") != string::npos) msg = "integer overflow in division";
        else if (msg.find("call stack") != string::npos) {
            msg = "native call stack overflow";
            if (auto f = run.prog.Find("gs_native_overflow"); f && !f->external && run.fault.empty()) {
                run.stop = InterpRun::REPORT;
                run.reporter = (int)(f - run.prog.funcs.data());
                run.reportargs.clear();
            }
        }
        if (run.stop == InterpRun::NONE) {
            run.stop = InterpRun::FAULT;
            run.fault = msg;
        }
    }
    if (run.stop == InterpRun::REPORT) {
        // The reporter prints the runtime's message and exits; the next
        // finish has that. Asked once; should it not run, the message.
        run.stop = InterpRun::NONE;
        run.fault = msg.empty() ? "the program stopped in its reporter" : msg;
        return kWasmReport;
    }
    int status;
    if (run.stop == InterpRun::EXIT) status = run.status;
    else {
        run.vm->write(2, cat("fault: ", run.fault.empty() ? "the program stopped without a reason" : run.fault, "\n"));
        status = 134;
    }
    pending.reset();
    return status;
}

int RunInterp(const string &csrc, const string &progname, const vector<string> &progargs,
              const vector<const goose_thunk *> &thunks, const string &cname, const string &argv0) {
    using namespace interp;
    auto run = Lower(csrc, progname, cname);
    // The runtime makes stdout unbuffered so that output is never lost to
    // an abort and interleaves with stderr; its setvbuf reaches the VM's
    // adapter, not the process, so the process does the same here.
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (compiled) run->module = WasmGen(run->prog).Run();
    run->vm = make_unique<VM>(run->prog, compiled);
    auto &vm = *run->vm;
    for (auto table : thunks)
        for (auto t = table; t->name; t++) vm.thunks[t->name] = t;
    vector<string> args;
    args.push_back(argv0.empty() ? progname : argv0);
    args.insert(args.end(), progargs.begin(), progargs.end());
    // In pieces, as the page runs it (§9), so that every run exercises the
    // suspension; natively the pieces just follow one another, and for a
    // host that resumes the run itself the first yield hands it back.
    vm.budget = 1 << 20;
    vm.Start(args);
    if (compiled) {
        // Set up and handed to the host, which runs the module (§6).
        pending = std::move(run);
        return kWasmReady;
    }
    if (resumable) {
        pending = std::move(run);
        return InterpResume();
    }
    while (!vm.Resume()) {}
    return vm.status;
}

}  // namespace goose
