// The interpreter end to end (src/interp/): each fixture in test/interp/run/
// is preprocessed, parsed, lowered to bytecode and run, and what it writes
// to the two streams, followed by `exit status N` when that is not zero,
// is compared with `name.expected` (`--bless` rewrites them; `--dump name`
// prints a fixture's bytecode). The fixtures are small C programs over the
// prelude and the C library (docs/design/interpreter.md §14, steps 3 and
// 4): arithmetic at every width, pointers and segments, control flow,
// calls, static data, the faults the VM must report, and each part of the
// library. A fixture gets a fresh empty directory as argv[1] for its files,
// `name.stdin` as its standard input if there is one, and the environment
// variable INTERP_RUN_VAR set to "forty-two".
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../src/includes.h"
#include "../src/utils.h"
#include "../src/interp/pp.h"
#include "../src/interp/ctypes.h"
#include "../src/interp/cast.h"
#include "../src/interp/cparse.h"
#include "../src/interp/bytecode.h"
#include "../src/interp/lower.h"
#include "../src/interp/vm.h"
#include "../src/interp/prelude.h"
#include "../src/interp/clib.h"

#include <filesystem>

using namespace goose;
using namespace goose::interp;
namespace fs = std::filesystem;

static int failures = 0;

static void Fail(const string &what, const string &detail) {
    failures++;
    printf("FAIL %s\n%s\n", what.c_str(), detail.c_str());
}

static string ReadAll(const fs::path &p) {
    string s;
    if (!LoadFile(p.string(), s)) {
        fprintf(stderr, "cannot read %s\n", p.string().c_str());
        exit(2);
    }
    return s;
}

static void WriteAll(const fs::path &p, const string &s) {
    auto f = fopen(p.string().c_str(), "wb");
    assert(f);
    fwrite(s.data(), 1, s.size(), f);
    fclose(f);
}

// <a.h> from the prelude, "a.h" beside the including file.
static PPIncludeFn Includer() {
    return PreludeIncluder([](const string &name, bool system, const string &from, string &text,
                              string &path) {
        if (system) return false;
        auto p = fs::path(from) / name;
        if (!fs::is_regular_file(p)) return false;
        text = ReadAll(p);
        path = p.generic_string();
        return true;
    });
}

static string Relative(string s, const fs::path &dir) {
    auto prefix = dir.generic_string() + "/";
    for (size_t at; (at = s.find(prefix)) != string::npos;) s.erase(at, prefix.size());
    return s;
}

// A fixture compiled: preprocessed, parsed and lowered.
struct Compiled {
    TypeTable types;
    vector<string> files;
    vector<PPToken> toks;
    unique_ptr<CParser> parser;
    Program prog;
    explicit Compiled(const fs::path &file) {
        Preprocessor pp(Includer());
        toks = pp.Run(ReadAll(file), file.generic_string());
        files = pp.files;
        parser = make_unique<CParser>(toks, files, types);
        auto &unit = parser->ParseUnit();
        prog = Lowerer(unit, files).Run();
    }
};

// Runs a fixture and returns its output; the stack limit is small so the
// recursion fixtures reach it quickly.
static string RunFixture(const fs::path &f, const fs::path &dir, bool dump) {
    string out;
    auto tmp = fs::temp_directory_path() / cat("interp_run_", f.stem().string());
    fs::remove_all(tmp);
    fs::create_directories(tmp);
    try {
        Compiled c(f);
        if (dump)
            for (auto &fn : c.prog.funcs)
                if (!fn.external) out += Disasm(c.prog, fn);
        VM vm(c.prog);
        vm.framelimit = 1u << 20;
        vm.write = [&](int, string_view data) { out += data; };
        string input;
        auto in = f;
        in.replace_extension(".stdin");
        if (fs::exists(in)) input = ReadAll(in);
        size_t at = 0;
        vm.read = [&](char *buf, size_t n) {
            n = min(n, input.size() - at);
            memcpy(buf, input.data() + at, n);
            at += n;
            return n;
        };
        int status = vm.Run({ "fixture", tmp.generic_string() });
        if (status) Append(out, "exit status ", status, "\n");
    } catch (CompileError &e) {
        Append(out, "error: ", e.msg, "\n");
    }
    fs::remove_all(tmp);
    return Relative(Relative(out, dir), tmp);
}

// The runtime under GS_INTERP (docs/design/interpreter.md §10, step 5): the
// text a JIT run assembles, with a stand-in for codegen's type block,
// preprocesses through the prelude, parses and lowers, and every external
// function it calls is one the library binds. Nothing runs it yet.
static void Runtime(const fs::path &root) {
    auto rt = root / "src" / "runtime";
    string text = "#define GS_INTERP 1\n";
    for (auto name : { "runtime.h", "runtime_impl.h", "runtime_threads.h" })
        text += ReadAll(rt / name);
    // What codegen's head always has of the types the extern support is
    // written against (CodeGen::EmitCoreTypes), and sl_u8 as a program that
    // uses it gets it.
    text +=
        "#pragma pack(push, 1)\n"
        "typedef struct { uint8_t *base; int64_t len; } gs_rhdr;\n"
        "typedef struct { gs_rhdr *hdr; gs_stack *stk; } gs_rref;\n"
        "typedef struct { gs_rhdr *hdr; gs_stack *stk; gs_rhdr *fl; gs_stack *flstk; } gs_pref;\n"
        "typedef struct { uint8_t *data; int64_t len; } sl_u8;\n"
        "#pragma pack(pop)\n";
    for (auto name : { "runtime_ext.h", "runtime_os.h" }) text += ReadAll(rt / name);
    try {
        Preprocessor pp(PreludeIncluder());
        auto toks = pp.Run(text, "runtime");
        TypeTable types;
        CParser parser(toks, pp.files, types);
        auto &unit = parser.ParseUnit();
        auto prog = Lowerer(unit, pp.files).Run();
        VM vm(prog);
        // The externals some function calls or takes the address of; the
        // thread entry points runtime.h declares stay unused without
        // GS_NEED_THREADS.
        vector<bool> used(prog.funcs.size());
        for (auto &f : prog.funcs)
            for (auto &in : f.code)
                if (in.op == OP_CALL || in.op == OP_FUNC) used[(size_t)in.imm] = true;
        string missing;
        for (size_t i = 0; i < prog.funcs.size(); i++)
            if (prog.funcs[i].external && used[i] && !vm.library.count(prog.funcs[i].name))
                Append(missing, " ", prog.funcs[i].name);
        if (!missing.empty()) Fail("runtime", cat("calls functions the library lacks:", missing));
        else printf("ok runtime (%zu functions)\n", prog.funcs.size());
    } catch (CompileError &e) {
        Fail("runtime", e.msg);
    }
}

int main(int argc, char **argv) {
    bool bless = false;
    string only;
    for (int i = 1; i < argc; i++) {
        string a = argv[i];
        if (a == "--bless") bless = true;
        else if (a == "--dump" && i + 1 < argc) only = argv[++i];
        else {
            fprintf(stderr, "usage: interp_run [--bless] [--dump fixture]\n");
            return 2;
        }
    }
#ifdef _WIN32
    _putenv_s("INTERP_RUN_VAR", "forty-two");
#else
    setenv("INTERP_RUN_VAR", "forty-two", 1);
#endif
    auto here = fs::path(__FILE__).parent_path();
    auto dir = here / "interp" / "run";
    if (!only.empty()) {
        auto f = dir / only;
        if (f.extension() != ".c") f.replace_extension(".c");
        fputs(RunFixture(f, dir, true).c_str(), stdout);
        return 0;
    }
    vector<fs::path> files;
    for (auto &e : fs::directory_iterator(dir))
        if (e.path().extension() == ".c") files.push_back(e.path());
    sort(files.begin(), files.end());
    for (auto &f : files) {
        auto name = f.filename().string();
        auto got = RunFixture(f, dir, false);
        auto expected = f;
        expected.replace_extension(".expected");
        if (bless) {
            WriteAll(expected, got);
            printf("blessed %s\n", name.c_str());
            continue;
        }
        if (!fs::exists(expected)) {
            Fail(name, "no expected file; run with --bless to write it");
            continue;
        }
        auto want = ReadAll(expected);
        if (want != got) Fail(name, cat("expected:\n", want, "got:\n", got));
        else printf("ok %s\n", name.c_str());
    }
    Runtime(here.parent_path());
    if (failures) printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
