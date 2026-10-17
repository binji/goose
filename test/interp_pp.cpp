// The interpreter's C preprocessor (src/interp/pp.h) over the fixtures in
// test/interp/pp/: each `name.c` is preprocessed and its text compared with
// `name.expected`, which holds the error message instead where the fixture
// must fail. `--bless` rewrites the expected files. Then src/runtime/runtime.h
// is preprocessed with the interpreter's predefined macros and checked to
// hold none of the platform branches (docs/design/interpreter.md §3).
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../src/includes.h"
#include "../src/utils.h"
#include "../src/interp/pp.h"

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

// "a.h" is looked up beside the including file, <a.h> under sys/.
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

// Paths in diagnostics made relative to the fixture directory, so the
// expected files do not depend on where the tree is.
static string Relative(string s, const fs::path &dir) {
    auto prefix = dir.generic_string() + "/";
    for (size_t at; (at = s.find(prefix)) != string::npos;) s.erase(at, prefix.size());
    return s;
}

static void Fixtures(const fs::path &dir, bool bless) {
    vector<fs::path> files;
    for (auto &e : fs::directory_iterator(dir))
        if (e.path().extension() == ".c") files.push_back(e.path());
    sort(files.begin(), files.end());
    for (auto &f : files) {
        auto name = f.filename().string();
        string got;
        try {
            Preprocessor pp(Includer(dir / "sys"));
            pp.Define("FROM_COMMAND_LINE=42");
            pp.Define("FLAG");
            got = PPText(pp.Run(ReadAll(f), f.generic_string()));
        } catch (CompileError &e) {
            got = cat("error: ", e.msg, "\n");
        }
        got = Relative(got, dir);
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
}

// runtime.h under the interpreter's predefined macros alone takes the
// portable branch everywhere: the text that comes out has no compiler
// extension in it beyond the attributes the parser drops.
static void Runtime(const fs::path &root) {
    auto path = root / "src" / "runtime" / "runtime.h";
    string text;
    try {
        // The standard headers are the prelude's business (step 4); here
        // they are empty, which is all preprocessing needs.
        Preprocessor pp([](const string &, bool system, const string &, string &text,
                           string &path) {
            if (!system) return false;
            text.clear();
            path = "<sys>";
            return true;
        });
        text = PPText(pp.Run(ReadAll(path), path.generic_string()));
    } catch (CompileError &e) {
        Fail("runtime.h", e.msg);
        return;
    }
    static const char *const forbidden[] = {
        "__asm__", "__builtin_", "__int128", "_mm_", "__declspec", "__forceinline",
        "GS_SIMD_TARGET1", "#pragma clang", "#pragma GCC", "#pragma fp_contract",
        "always_inline", "windows.h", "intrin.h", "gs_mulhi_u64(magic, x)",
    };
    for (auto f : forbidden)
        if (text.find(f) != string::npos) Fail("runtime.h", cat("contains ", f));
    // What the portable branches give: memcpy for the unaligned loads and
    // the copy macros, no 128-bit multiply, an inline that is just inline.
    static const char *const expected[] = {
        "static uint32_t gs_ld32(const void *p) { uint32_t v; memcpy(&v, p, 4); return v; }",
        "(void)magic;",   /* the division helpers without a 128-bit multiply */
        "__attribute__((noreturn))",
    };
    for (auto e : expected)
        if (text.find(e) == string::npos) Fail("runtime.h", cat("lacks ", e));
    if (failures == 0) printf("ok runtime.h (%d bytes)\n", (int)text.size());
}

int main(int argc, char **argv) {
    bool bless = argc > 1 && string(argv[1]) == "--bless";
    auto here = fs::path(__FILE__).parent_path();
    Fixtures(here / "interp" / "pp", bless);
    Runtime(here.parent_path());
    if (failures) printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
