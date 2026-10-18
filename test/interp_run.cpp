// The interpreter end to end (src/interp/): each fixture in test/interp/run/
// is preprocessed, parsed, lowered to bytecode and run, and what it writes
// to the two streams, followed by `exit status N` when that is not zero,
// is compared with `name.expected` (`--bless` rewrites them; `--dump name`
// prints a fixture's bytecode). The fixtures are small C programs over the
// handful of library functions this step binds (docs/design/interpreter.md
// §14, step 3): arithmetic at every width, pointers and segments, control
// flow, calls, static data, and the faults the VM must report.
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

// "a.h" beside the including file, <a.h> under the fixture directory's sys/.
static PPIncludeFn Includer(const fs::path &sysdir) {
    return [sysdir](const string &name, bool system, const string &from, string &text,
                    string &path) {
        auto p = system ? sysdir / name : fs::path(from) / name;
        if (!fs::is_regular_file(p)) return false;
        text = ReadAll(p);
        path = p.generic_string();
        return true;
    };
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
    Compiled(const fs::path &file, const fs::path &sysdir) {
        Preprocessor pp(Includer(sysdir));
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
    try {
        Compiled c(f, dir / "sys");
        if (dump)
            for (auto &fn : c.prog.funcs)
                if (!fn.external) out += Disasm(c.prog, fn);
        VM vm(c.prog);
        vm.framelimit = 1u << 20;
        vm.write = [&](int, string_view data) { out += data; };
        int status = vm.Run({ "fixture" });
        if (status) Append(out, "exit status ", status, "\n");
    } catch (CompileError &e) {
        Append(out, "error: ", e.msg, "\n");
    }
    return Relative(out, dir);
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
    if (failures) printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
