// The interpreter's C parser for declarations (src/interp/cparse.h), two
// ways. The fixtures in test/interp/decl/ are parsed and their declaration
// dumps compared with `name.expected`, which holds the error message where
// the fixture must fail (`--bless` rewrites them). Then
// test/interp/layout/types.h, which this file also includes, is parsed, and
// every sizeof and offsetof its X-macro lists name is compared with what
// the C++ compiler building this test computed: the packed and natural
// layouts must agree exactly (docs/design/interpreter.md §4).
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../src/includes.h"
#include "../src/utils.h"
#include "../src/interp/pp.h"
#include "../src/interp/ctypes.h"
#include "../src/interp/cast.h"
#include "../src/interp/cparse.h"

#include <filesystem>
#include <stddef.h>
#include "interp/layout/types.h"

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

// Parses a file: the preprocessor over it, then the declarations.
struct Parsed {
    TypeTable types;
    vector<PPToken> toks;
    unique_ptr<CParser> parser;
    Parsed(const fs::path &file, const fs::path &sysdir) {
        Preprocessor pp(Includer(sysdir));
        toks = pp.Run(ReadAll(file), file.generic_string());
        // The parser keeps a reference to the file table, so it is copied out
        // of the preprocessor, which does not outlive this constructor.
        files = pp.files;
        parser = make_unique<CParser>(toks, files, types);
        parser->ParseUnit();
    }
    vector<string> files;
};

static void Fixtures(const fs::path &dir, bool bless) {
    vector<fs::path> files;
    for (auto &e : fs::directory_iterator(dir))
        if (e.path().extension() == ".c") files.push_back(e.path());
    sort(files.begin(), files.end());
    for (auto &f : files) {
        auto name = f.filename().string();
        string got;
        try {
            Parsed p(f, dir / "sys");
            got = DumpUnit(p.parser->Unit(), p.types);
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

// The offset of a dotted member path within a record type.
static bool OffsetOf(const CType *t, const string &path, int64_t &off) {
    off = 0;
    size_t i = 0;
    while (i < path.size()) {
        auto dot = path.find('.', i);
        auto name = path.substr(i, dot == string::npos ? string::npos : dot - i);
        if (!t->IsRecord()) return false;
        auto f = t->rec->Find(name);
        if (!f) return false;
        off += f->offset;
        t = f->type;
        if (dot == string::npos) break;
        i = dot + 1;
    }
    return true;
}

static void Layout(const fs::path &dir) {
    unique_ptr<Parsed> parsed;
    try {
        parsed = make_unique<Parsed>(dir / "types.h", dir / "sys");
    } catch (CompileError &e) {
        Fail("layout/types.h", e.msg);
        return;
    }
    auto &p = *parsed;
    int checks = 0;
    auto record = [&](const char *name) -> const CType * {
        auto &g = p.parser->Unit().global;
        if (auto s = g.Find(name); s && s->kind == CSym::S_TYPEDEF) return s->type;
        if (auto st = g.FindTag(name)) return p.types.Record(st);
        Fail(cat("layout ", name), "not declared by the parser");
        return nullptr;
    };
    auto size = [&](const char *name, size_t host) {
        auto t = record(name);
        if (!t) return;
        checks++;
        if ((size_t)t->size != host)
            Fail(cat("sizeof(", name, ")"), cat("host ", host, ", interpreter ", t->size));
    };
    auto field = [&](const char *name, const char *path, size_t host, size_t hostsize) {
        auto t = record(name);
        if (!t) return;
        checks++;
        int64_t off;
        if (!OffsetOf(t, path, off)) { Fail(cat("offsetof(", name, ", ", path, ")"), "no such member"); return; }
        if ((size_t)off != host)
            Fail(cat("offsetof(", name, ", ", path, ")"), cat("host ", host, ", interpreter ", off));
        if ((size_t)t->size != hostsize)
            Fail(cat("sizeof(", name, ")"), cat("host ", hostsize, ", interpreter ", t->size));
    };
    #define S(T) size(#T, sizeof(T));
    LAYOUT_SIZES(S)
    #undef S
    #define X(T, f) field(#T, #f, offsetof(T, f), sizeof(T));
    LAYOUT_FIELDS(X)
    #undef X
    printf("ok layout/types.h (%d checks)\n", checks);
}

int main(int argc, char **argv) {
    bool bless = argc > 1 && string(argv[1]) == "--bless";
    auto here = fs::path(__FILE__).parent_path();
    Fixtures(here / "interp" / "decl", bless);
    Layout(here / "interp" / "layout");
    if (failures) printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
