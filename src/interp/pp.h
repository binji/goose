// Goose compiler — the interpreter's C preprocessor (docs/design/interpreter.md
// §4, step 1 of §14). It turns the C text a JIT run would hand TinyCC into
// preprocessing tokens for the interpreter's parser: macros with Prosser's
// hide sets, conditionals with C's constant arithmetic, `#include` through a
// callback the driver supplies, `#pragma` lines kept as tokens, and `#error`.
// It predefines only __STDC__, __STDC_VERSION__, __GOOSE_INTERP__ and a
// __has_builtin that answers 0, so the runtime's platform branches
// (__GNUC__, _MSC_VER, __TINYC__, __x86_64__, __SIZEOF_INT128__, ...) are
// never entered and the portable C99 paths come out. Variadic macros are not
// supported: neither the runtime nor codegen writes one.
#pragma once

namespace goose {
namespace interp {

enum PPKind {
    PP_IDENT,
    PP_NUMBER,    // a pp-number: digits, letters, dots, e+/e-/p+/p- (1, 0x1Full, 1.5e+3, .5)
    PP_CHAR,      // 'x', spelling kept with its quotes and escapes
    PP_STRING,    // "x", likewise
    PP_PUNCT,
    PP_OTHER,     // any other character, one per token
    PP_PRAGMA,    // a #pragma line: text is what followed the directive name
    PP_END,
};

struct PPToken {
    PPKind kind = PP_END;
    string text;
    int file = 0;         // index into Preprocessor::files
    int line = 0;
    bool space = false;   // whitespace (or a comment) precedes it on its line
    bool bol = false;     // the first token of a logical line
    vector<int> hide;     // the macros it may not expand (Prosser's hide set), by id
};

// Resolves an #include: `name` as written, `system` for the <> form, `from`
// the directory of the including file (with a trailing slash, or empty). On
// success fills the file's text and the path to report it under.
using PPIncludeFn = function<bool(const string &name, bool system, const string &from,
                                  string &text, string &path)>;

struct Preprocessor {
    explicit Preprocessor(PPIncludeFn include) : include(std::move(include)) {
        files.push_back("<predefined>");
        Predefine("__STDC__", "1");
        Predefine("__STDC_VERSION__", "199901L");
        Predefine("__STDC_HOSTED__", "1");
        Predefine("__GOOSE_INTERP__", "1");
        // A function-like macro, so `#if defined(__has_builtin)` holds and
        // `#if __has_builtin(x)` is 0.
        DoDefine(LexAll("__has_builtin(x) 0", 0));
    }

    // A -D style definition: "NAME", "NAME=VALUE" or "NAME(a, b)=VALUE".
    void Define(const string &def) {
        auto eq = def.find('=');
        auto text = eq == string::npos ? def + " 1" : def.substr(0, eq) + " " + def.substr(eq + 1);
        DoDefine(LexAll(text, 0));
    }

    // Preprocesses `text`, reported as `filename`; what the parser consumes,
    // ending in a PP_END token. Throws CompileError on the first error.
    vector<PPToken> Run(const string &text, const string &filename) {
        PushSource(text, filename);
        vector<PPToken> out;
        bool pendbol = false;
        MainStream ms { *this };
        for (;;) {
            auto t = Get(ms);
            if (t.kind == PP_END) break;
            if (t.bol) pendbol = true;
            if (!TryExpand(ms, t, pendbol)) {
                t.bol = pendbol;
                pendbol = false;
                out.push_back(std::move(t));
            }
        }
        PPToken end;
        end.kind = PP_END;
        end.file = (int)files.size() - 1;
        out.push_back(end);
        return out;
    }

    vector<string> files;

    string Where(const PPToken &t) const { return cat(files[t.file], ":", t.line); }

    // ---- the pieces below are public for the tests; the parser needs none of them.

    // The spelling of a token sequence, with single spaces where the source had any.
    static string Spell(const vector<PPToken> &ts, size_t from = 0) {
        string s;
        for (auto i = from; i < ts.size(); i++) {
            if (ts[i].kind == PP_END) break;
            if (i > from && ts[i].space) s += ' ';
            s += ts[i].text;
        }
        return s;
    }

    [[noreturn]] void Error(const PPToken &at, string_view msg) const {
        throw CompileError { cat(Where(at), ": error: ", msg) };
    }

private:
    struct Macro {
        int id = 0;
        bool funclike = false;
        vector<string> params;
        vector<PPToken> body;
    };

    struct Cond {
        bool taken;      // some branch of this #if has been true
        bool live;       // the current branch is being emitted
        bool inelse;
        PPToken at;
    };

    // One file being read: its text after line splicing, and where we are.
    struct Source {
        string text;
        vector<int> physline;   // physical line of each logical line, 1-based
        size_t pos = 0;
        int lline = 1;
        int file = 0;
        string dir;
        size_t condbase = 0;    // the conditional stack's depth as this file began
    };

    // Tokens come from a stream: the files being read, or a fixed list (a
    // macro argument being pre-expanded, an #if line). Expansion pushes the
    // tokens it produces back to the front for rescanning.
    struct Stream {
        deque<PPToken> pending;
        virtual PPToken Pull() = 0;
        virtual ~Stream() = default;
    };
    struct MainStream : Stream {
        Preprocessor &pp;
        explicit MainStream(Preprocessor &pp) : pp(pp) {}
        PPToken Pull() override { return pp.LexNext(*this); }
    };
    struct ListStream : Stream {
        explicit ListStream(const vector<PPToken> &ts) { pending.assign(ts.begin(), ts.end()); }
        PPToken Pull() override { PPToken e; e.kind = PP_END; return e; }
    };

    PPIncludeFn include;
    unordered_map<string, Macro> macros;
    int nextid = 1;
    vector<Source> sources;
    vector<Cond> conds;
    int skipeval = 0;   // inside an operand #if does not evaluate: no errors there

    void Predefine(const char *name, const char *value) {
        DoDefine(LexAll(cat(name, " ", value), 0));
    }

    // ---- Lexing

    static bool IdentStart(char c) { return isalpha((uint8_t)c) || c == '_'; }
    static bool IdentCont(char c)  { return isalnum((uint8_t)c) || c == '_'; }

    void PushSource(const string &text, const string &filename) {
        Source s;
        s.file = (int)files.size();
        files.push_back(filename);
        auto slash = filename.find_last_of("/\\");
        s.dir = slash == string::npos ? string() : filename.substr(0, slash + 1);
        s.condbase = conds.size();
        // Phase 2: splice backslash-newlines, normalize \r\n, and remember
        // each logical line's physical line.
        s.text.reserve(text.size() + 1);
        s.physline.push_back(0);
        s.physline.push_back(1);
        int phys = 1;
        for (size_t i = 0; i < text.size(); i++) {
            auto c = text[i];
            if (c == '\\' && (text[i + 1] == '\n' || (text[i + 1] == '\r' && text[i + 2] == '\n'))) {
                i += text[i + 1] == '\r' ? 2 : 1;
                phys++;
                continue;
            }
            if (c == '\r' && text[i + 1] == '\n') continue;
            if (c == '\n') {
                phys++;
                s.physline.push_back(phys);
            }
            s.text += c;
        }
        if (s.text.empty() || s.text.back() != '\n') s.text += '\n';
        sources.push_back(std::move(s));
    }

    // Lexes one logical line of the current source into `line`, directives
    // included; false at the end of the file. In a skipped group `lenient`
    // lets an unmatched quote through as a character.
    bool LexLine(Source &s, vector<PPToken> &line, bool lenient) {
        line.clear();
        const auto &t = s.text;
        if (s.pos >= t.size()) return false;
        bool space = false;
        bool first = true;
        for (;;) {
            auto c = t[s.pos];
            if (c == '\n') {
                s.pos++;
                s.lline++;
                return true;
            }
            if (c == ' ' || c == '\t' || c == '\f' || c == '\v') {
                s.pos++;
                space = true;
                continue;
            }
            if (c == '/' && t[s.pos + 1] == '/') {
                while (t[s.pos] != '\n') s.pos++;
                continue;
            }
            if (c == '/' && t[s.pos + 1] == '*') {
                auto start = s.pos;
                s.pos += 2;
                for (;;) {
                    if (s.pos + 1 >= t.size()) {
                        PPToken at = Mark(s, start, first, space);
                        Error(at, "unterminated comment");
                    }
                    if (t[s.pos] == '*' && t[s.pos + 1] == '/') { s.pos += 2; break; }
                    if (t[s.pos] == '\n') s.lline++;
                    s.pos++;
                }
                space = true;
                continue;
            }
            PPToken tok = Mark(s, s.pos, first, space);
            first = false;
            space = false;
            auto start = s.pos;
            if (IdentStart(c)) {
                while (IdentCont(t[s.pos])) s.pos++;
                tok.kind = PP_IDENT;
            } else if (isdigit((uint8_t)c) || (c == '.' && isdigit((uint8_t)t[s.pos + 1]))) {
                s.pos++;
                for (;;) {
                    auto d = t[s.pos];
                    if ((d == '+' || d == '-') && strchr("eEpP", t[s.pos - 1])) s.pos++;
                    else if (IdentCont(d) || d == '.') s.pos++;
                    else break;
                }
                tok.kind = PP_NUMBER;
            } else if (c == '"' || c == '\'') {
                s.pos++;
                for (;;) {
                    auto d = t[s.pos];
                    if (d == '\n') {
                        if (lenient) { s.pos = start + 1; break; }
                        Error(tok, c == '"' ? "unterminated string literal"
                                            : "unterminated character constant");
                    }
                    s.pos++;
                    if (d == '\\') {
                        // Splices are gone, so a backslash before the newline
                        // is a stray one: the newline then ends the literal.
                        if (t[s.pos] != '\n') s.pos++;
                    } else if (d == c) {
                        break;
                    }
                }
                tok.kind = c == '"' ? PP_STRING : PP_CHAR;
            } else {
                static const char *const puncts[] = {
                    "...", "<<=", ">>=", "->", "++", "--", "<<", ">>", "<=", ">=", "==", "!=",
                    "&&", "||", "*=", "/=", "%=", "+=", "-=", "&=", "^=", "|=", "##",
                    "(", ")", "[", "]", "{", "}", ".", "&", "*", "+", "-", "~", "!", "/", "%",
                    "<", ">", "^", "|", "?", ":", ";", "=", ",", "#", nullptr
                };
                tok.kind = PP_OTHER;
                for (auto p = puncts; *p; p++) {
                    auto n = strlen(*p);
                    if (t.compare(s.pos, n, *p) == 0) {
                        s.pos += n;
                        tok.kind = PP_PUNCT;
                        break;
                    }
                }
                if (tok.kind == PP_OTHER) s.pos++;
            }
            tok.text.assign(t, start, s.pos - start);
            line.push_back(std::move(tok));
        }
    }

    static PPToken Mark(const Source &s, size_t, bool bol, bool space) {
        PPToken t;
        t.file = s.file;
        t.line = s.physline[(size_t)s.lline < s.physline.size() ? s.lline : s.physline.size() - 1];
        t.bol = bol;
        t.space = space;
        return t;
    }

    // Lexes a string of its own (a -D definition, a pasted token) as one line.
    vector<PPToken> LexAll(const string &text, int file) {
        Source s;
        s.file = file;
        s.text = text + "\n";
        s.physline = { 0, 1 };
        vector<PPToken> line;
        LexLine(s, line, false);
        return line;
    }

    // The first token of the next line that is not a directive, the rest of
    // the line queued on `s`, directives having been carried out on the way;
    // PP_END when every source is exhausted.
    PPToken LexNext(Stream &s) {
        vector<PPToken> line;
        for (;;) {
            if (sources.empty()) { PPToken e; e.kind = PP_END; return e; }
            auto &src = sources.back();
            bool live = conds.empty() || conds.back().live;
            if (!LexLine(src, line, !live)) {
                if (conds.size() > src.condbase) Error(conds.back().at, "unterminated #if");
                sources.pop_back();
                continue;
            }
            if (line.empty()) continue;
            if (line[0].kind == PP_PUNCT && line[0].text == "#") {
                Directive(line, live, s);
                // A #pragma line is a token of its own, ahead of the next line.
                if (!s.pending.empty()) return Get(s);
                continue;
            }
            if (!live) continue;
            s.pending.insert(s.pending.end(), line.begin() + 1, line.end());
            return line[0];
        }
    }

    PPToken Get(Stream &s) {
        if (s.pending.empty()) return s.Pull();
        auto t = std::move(s.pending.front());
        s.pending.pop_front();
        return t;
    }

    void Unget(Stream &s, PPToken t) { s.pending.push_front(std::move(t)); }

    // ---- Directives

    void Directive(vector<PPToken> &line, bool live, Stream &s) {
        if (line.size() == 1) return;   // the null directive
        auto &d = line[1];
        if (d.kind != PP_IDENT) {
            if (live) Error(d, cat("expected a directive name, found '", d.text, "'"));
            return;
        }
        auto &name = d.text;
        auto restspelling = [&] { return Spell(line, 2); };
        if (name == "if" || name == "ifdef" || name == "ifndef") {
            Cond c { false, false, false, d };
            if (live) {
                if (name == "if") c.taken = EvalCondition(line);
                else {
                    if (line.size() < 3 || line[2].kind != PP_IDENT)
                        Error(d, cat("#", name, " expects a macro name"));
                    c.taken = macros.count(line[2].text) == (name == "ifdef");
                }
                c.live = c.taken;
            }
            conds.push_back(c);
            return;
        }
        if (name == "elif" || name == "else" || name == "endif") {
            if (conds.empty() || conds.size() <= sources.back().condbase)
                Error(d, cat("#", name, " without #if"));
            auto &c = conds.back();
            if (name == "endif") {
                conds.pop_back();
                return;
            }
            if (c.inelse) Error(d, cat("#", name, " after #else"));
            bool outerlive = conds.size() == 1 || conds[conds.size() - 2].live;
            if (name == "else") {
                c.inelse = true;
                c.live = outerlive && !c.taken;
                c.taken = true;
            } else {
                c.live = outerlive && !c.taken && EvalCondition(line);
                if (c.live) c.taken = true;
            }
            return;
        }
        if (!live) return;
        if (name == "define") {
            line.erase(line.begin(), line.begin() + 2);
            DoDefine(line);
        } else if (name == "undef") {
            if (line.size() != 3 || line[2].kind != PP_IDENT) Error(d, "#undef expects a macro name");
            macros.erase(line[2].text);
        } else if (name == "include") {
            DoInclude(line);
        } else if (name == "error") {
            Error(d, cat("#error ", restspelling()));
        } else if (name == "pragma") {
            PPToken p = d;
            p.kind = PP_PRAGMA;
            p.bol = true;
            p.text = restspelling();
            s.pending.push_back(std::move(p));
        } else if (name == "line" || name == "warning" || name == "ident") {
            // Nothing the interpreter needs; GCC extensions it tolerates.
        } else {
            Error(d, cat("unknown directive #", name));
        }
    }

    void DoDefine(vector<PPToken> line) {
        if (line.empty() || line[0].kind != PP_IDENT) {
            PPToken at = line.empty() ? PPToken() : line[0];
            Error(at, "#define expects a macro name");
        }
        Macro m;
        auto &name = line[0].text;
        if (name == "defined") Error(line[0], "'defined' cannot be a macro name");
        size_t i = 1;
        if (i < line.size() && line[i].kind == PP_PUNCT && line[i].text == "(" && !line[i].space) {
            m.funclike = true;
            i++;
            while (i < line.size() && !(line[i].kind == PP_PUNCT && line[i].text == ")")) {
                if (line[i].kind == PP_PUNCT && line[i].text == "...")
                    Error(line[i], "variadic macros are not supported");
                if (line[i].kind != PP_IDENT) Error(line[i], "expected a parameter name");
                if (find(m.params.begin(), m.params.end(), line[i].text) != m.params.end())
                    Error(line[i], cat("duplicate parameter '", line[i].text, "'"));
                m.params.push_back(line[i].text);
                i++;
                if (i < line.size() && line[i].kind == PP_PUNCT && line[i].text == ",") {
                    i++;
                    if (i < line.size() && line[i].kind == PP_PUNCT && line[i].text == ")")
                        Error(line[i], "expected a parameter name");
                }
            }
            if (i >= line.size()) Error(line[0], "unterminated parameter list");
            i++;
        }
        m.body.assign(line.begin() + i, line.end());
        if (!m.body.empty()) {
            m.body[0].space = false;
            auto &f = m.body.front();
            auto &b = m.body.back();
            if (f.kind == PP_PUNCT && f.text == "##") Error(f, "'##' cannot start a macro body");
            if (b.kind == PP_PUNCT && b.text == "##") Error(b, "'##' cannot end a macro body");
            if (m.funclike)
                for (size_t k = 0; k < m.body.size(); k++)
                    if (m.body[k].kind == PP_PUNCT && m.body[k].text == "#" &&
                        (k + 1 == m.body.size() || ParamIndex(m, m.body[k + 1]) < 0))
                        Error(m.body[k], "'#' is not followed by a macro parameter");
        }
        for (auto &t : m.body) t.bol = false;
        auto it = macros.find(name);
        if (it != macros.end()) {
            if (!SameMacro(it->second, m)) Error(line[0], cat("'", name, "' redefined differently"));
            return;
        }
        m.id = nextid++;
        macros.emplace(name, std::move(m));
    }

    static int ParamIndex(const Macro &m, const PPToken &t) {
        if (t.kind != PP_IDENT) return -1;
        for (size_t i = 0; i < m.params.size(); i++) if (m.params[i] == t.text) return (int)i;
        return -1;
    }

    static bool SameMacro(const Macro &a, const Macro &b) {
        if (a.funclike != b.funclike || a.params != b.params || a.body.size() != b.body.size())
            return false;
        for (size_t i = 0; i < a.body.size(); i++)
            if (a.body[i].text != b.body[i].text || (i > 0 && a.body[i].space != b.body[i].space))
                return false;
        return true;
    }

    void DoInclude(vector<PPToken> &line) {
        if (line.size() < 3) Error(line[1], "#include expects a file name");
        string name;
        bool system = false;
        auto &t = line[2];
        if (t.kind == PP_STRING) {
            name = t.text.substr(1, t.text.size() - 2);
            if (line.size() > 3) Error(line[3], "extra tokens after #include");
        } else if (t.kind == PP_PUNCT && t.text == "<") {
            system = true;
            size_t i = 3;
            for (; i < line.size() && !(line[i].kind == PP_PUNCT && line[i].text == ">"); i++) {
                if (i > 3 && line[i].space) name += ' ';
                name += line[i].text;
            }
            if (i >= line.size()) Error(t, "missing '>' in #include");
            if (i + 1 < line.size()) Error(line[i + 1], "extra tokens after #include");
        } else {
            Error(t, "#include expects \"file\" or <file>");
        }
        if (sources.size() >= 64) Error(t, "#include nested too deeply");
        string text, path;
        if (!include || !include(name, system, sources.back().dir, text, path))
            Error(t, cat("cannot find include file ", system ? "<" : "\"", name, system ? ">" : "\""));
        PushSource(text, path);
    }

    // ---- Conditions

    struct Val {
        uint64_t v = 0;
        bool u = false;   // unsigned
    };

    // `defined` first, then macro expansion, then every identifier left is 0.
    bool EvalCondition(const vector<PPToken> &line) {
        if (line.size() < 3) Error(line[1], cat("#", line[1].text, " with no expression"));
        vector<PPToken> in;
        for (size_t i = 2; i < line.size(); i++) {
            auto &t = line[i];
            if (t.kind == PP_IDENT && t.text == "defined") {
                size_t j = i + 1;
                bool paren = j < line.size() && line[j].kind == PP_PUNCT && line[j].text == "(";
                if (paren) j++;
                if (j >= line.size() || line[j].kind != PP_IDENT)
                    Error(t, "'defined' expects a macro name");
                PPToken n = t;
                n.kind = PP_NUMBER;
                n.text = macros.count(line[j].text) ? "1" : "0";
                in.push_back(n);
                if (paren) {
                    j++;
                    if (j >= line.size() || line[j].kind != PP_PUNCT || line[j].text != ")")
                        Error(t, "missing ')' after 'defined('");
                }
                i = j;
                continue;
            }
            in.push_back(t);
        }
        auto ts = ExpandList(in);
        for (auto &t : ts)
            if (t.kind == PP_IDENT) {
                if (t.text == "defined") Error(t, "'defined' produced by macro expansion");
                t.kind = PP_NUMBER;
                t.text = "0";
            }
        PPToken end;
        end.kind = PP_END;
        end.file = line[1].file;
        end.line = line[1].line;
        ts.push_back(end);
        size_t pos = 0;
        auto v = Ternary(ts, pos);
        if (ts[pos].kind != PP_END) Error(ts[pos], cat("unexpected '", ts[pos].text, "' in #if"));
        return v.v != 0;
    }

    static bool Is(const PPToken &t, const char *p) { return t.kind == PP_PUNCT && t.text == p; }

    Val Ternary(const vector<PPToken> &ts, size_t &pos) {
        auto c = Binary(ts, pos, 0);
        if (!Is(ts[pos], "?")) return c;
        pos++;
        skipeval += c.v == 0;
        auto a = Ternary(ts, pos);
        skipeval -= c.v == 0;
        if (!Is(ts[pos], ":")) Error(ts[pos], "expected ':' in #if");
        pos++;
        skipeval += c.v != 0;
        auto b = Ternary(ts, pos);
        skipeval -= c.v != 0;
        Val r = c.v ? a : b;
        r.u = a.u || b.u;
        return r;
    }

    static int Prec(const PPToken &t) {
        if (t.kind != PP_PUNCT) return -1;
        static const pair<const char *, int> table[] = {
            { "||", 1 }, { "&&", 2 }, { "|", 3 }, { "^", 4 }, { "&", 5 },
            { "==", 6 }, { "!=", 6 }, { "<", 7 }, { ">", 7 }, { "<=", 7 }, { ">=", 7 },
            { "<<", 8 }, { ">>", 8 }, { "+", 9 }, { "-", 9 }, { "*", 10 }, { "/", 10 }, { "%", 10 },
        };
        for (auto &[s, p] : table) if (t.text == s) return p;
        return -1;
    }

    Val Binary(const vector<PPToken> &ts, size_t &pos, int minprec) {
        auto l = Unary(ts, pos);
        for (;;) {
            auto &op = ts[pos];
            int p = Prec(op);
            if (p < 0 || p < minprec) return l;
            pos++;
            bool skip = (op.text == "&&" && l.v == 0) || (op.text == "||" && l.v != 0);
            skipeval += skip;
            auto r = Binary(ts, pos, p + 1);
            skipeval -= skip;
            l = Apply(op, l, r);
        }
    }

    Val Apply(const PPToken &op, Val a, Val b) const {
        auto &o = op.text;
        bool u = a.u || b.u;
        auto s = [](uint64_t v) { return (int64_t)v; };
        Val r;
        r.u = u;
        if (o == "||") return { (uint64_t)(a.v != 0 || b.v != 0), false };
        if (o == "&&") return { (uint64_t)(a.v != 0 && b.v != 0), false };
        if (o == "|") r.v = a.v | b.v;
        else if (o == "^") r.v = a.v ^ b.v;
        else if (o == "&") r.v = a.v & b.v;
        else if (o == "==") return { (uint64_t)(a.v == b.v), false };
        else if (o == "!=") return { (uint64_t)(a.v != b.v), false };
        else if (o == "<") return { (uint64_t)(u ? a.v < b.v : s(a.v) < s(b.v)), false };
        else if (o == ">") return { (uint64_t)(u ? a.v > b.v : s(a.v) > s(b.v)), false };
        else if (o == "<=") return { (uint64_t)(u ? a.v <= b.v : s(a.v) <= s(b.v)), false };
        else if (o == ">=") return { (uint64_t)(u ? a.v >= b.v : s(a.v) >= s(b.v)), false };
        else if (o == "<<") { r.u = a.u; r.v = b.v >= 64 ? 0 : a.v << b.v; }
        else if (o == ">>") {
            r.u = a.u;
            if (b.v >= 64) r.v = a.u || s(a.v) >= 0 ? 0 : ~(uint64_t)0;
            else r.v = a.u ? a.v >> b.v : (uint64_t)(s(a.v) >> b.v);
        }
        else if (o == "+") r.v = a.v + b.v;
        else if (o == "-") r.v = a.v - b.v;
        else if (o == "*") r.v = a.v * b.v;
        else if (o == "/" || o == "%") {
            if (b.v == 0) {
                if (skipeval) return r;
                Error(op, "division by zero in #if");
            }
            if (u) r.v = o == "/" ? a.v / b.v : a.v % b.v;
            else if (s(b.v) == -1) r.v = o == "/" ? (uint64_t)(-s(a.v)) : 0;
            else r.v = o == "/" ? (uint64_t)(s(a.v) / s(b.v)) : (uint64_t)(s(a.v) % s(b.v));
        }
        return r;
    }

    Val Unary(const vector<PPToken> &ts, size_t &pos) {
        auto &t = ts[pos];
        if (Is(t, "(")) {
            pos++;
            auto v = Ternary(ts, pos);
            if (!Is(ts[pos], ")")) Error(ts[pos], "expected ')' in #if");
            pos++;
            return v;
        }
        if (Is(t, "-")) { pos++; auto v = Unary(ts, pos); v.v = (uint64_t)0 - v.v; return v; }
        if (Is(t, "+")) { pos++; return Unary(ts, pos); }
        if (Is(t, "~")) { pos++; auto v = Unary(ts, pos); v.v = ~v.v; return v; }
        if (Is(t, "!")) { pos++; auto v = Unary(ts, pos); return { (uint64_t)(v.v == 0), false }; }
        if (t.kind == PP_NUMBER) { pos++; return Number(t); }
        if (t.kind == PP_CHAR) { pos++; return { (uint64_t)(int64_t)CharValue(t), false }; }
        Error(t, t.kind == PP_END ? string("expression expected in #if")
                                  : cat("unexpected '", t.text, "' in #if"));
    }

    Val Number(const PPToken &t) const {
        auto &s = t.text;
        size_t i = 0;
        int base = 10;
        if (s.size() > 1 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { base = 16; i = 2; }
        else if (s.size() > 1 && s[0] == '0') { base = 8; i = 1; }
        uint64_t v = 0;
        bool any = false;
        for (; i < s.size(); i++) {
            auto c = s[i];
            int d = isdigit((uint8_t)c) ? c - '0' : isalpha((uint8_t)c) ? tolower(c) - 'a' + 10 : 99;
            if (d >= base) break;
            if (v > (~(uint64_t)0 - (uint64_t)d) / (uint64_t)base) Error(t, "integer constant too large");
            v = v * (uint64_t)base + (uint64_t)d;
            any = true;
        }
        if (!any && base != 8) Error(t, cat("invalid integer constant '", s, "'"));
        bool u = false;
        int ls = 0;
        for (; i < s.size(); i++) {
            auto c = tolower(s[i]);
            if (c == 'u' && !u) u = true;
            else if (c == 'l' && ls < 2) ls++;
            else Error(t, cat("invalid integer constant '", s, "'"));
        }
        // A decimal constant that does not fit a signed 64-bit integer is
        // unsigned, as an octal or hexadecimal one that sets the top bit is.
        if (v >> 63) u = true;
        return { v, u };
    }

    int64_t CharValue(const PPToken &t) const {
        auto &s = t.text;
        if (s.size() < 3) Error(t, "empty character constant");
        size_t i = 1;
        if (s[i] != '\\') {
            if (s.size() != 3) Error(t, "multi-character constants are not supported");
            return (uint8_t)s[i];
        }
        i++;
        auto c = s[i++];
        int64_t v;
        switch (c) {
            case 'n': v = '\n'; break;
            case 't': v = '\t'; break;
            case 'r': v = '\r'; break;
            case 'a': v = '\a'; break;
            case 'b': v = '\b'; break;
            case 'f': v = '\f'; break;
            case 'v': v = '\v'; break;
            case '0': case '1': case '2': case '3': case '4': case '5': case '6': case '7':
                v = c - '0';
                for (int k = 0; k < 2 && s[i] >= '0' && s[i] <= '7'; k++) v = v * 8 + (s[i++] - '0');
                break;
            case 'x':
                v = 0;
                while (isxdigit((uint8_t)s[i])) {
                    auto d = s[i++];
                    v = v * 16 + (isdigit((uint8_t)d) ? d - '0' : tolower(d) - 'a' + 10);
                }
                break;
            default: v = (uint8_t)c; break;
        }
        if (i + 1 != s.size()) Error(t, "multi-character constants are not supported");
        return v;
    }

    // ---- Expansion (Prosser's algorithm)

    static bool InHide(const PPToken &t, int id) {
        return find(t.hide.begin(), t.hide.end(), id) != t.hide.end();
    }

    static vector<int> Union(const vector<int> &a, int id) {
        auto r = a;
        if (find(r.begin(), r.end(), id) == r.end()) r.push_back(id);
        return r;
    }

    static vector<int> Intersect(const vector<int> &a, const vector<int> &b) {
        vector<int> r;
        for (auto x : a) if (find(b.begin(), b.end(), x) != b.end()) r.push_back(x);
        return r;
    }

    // Expands a fixed token list on its own (an argument, an #if line).
    vector<PPToken> ExpandList(const vector<PPToken> &in) {
        ListStream ls(in);
        vector<PPToken> out;
        for (;;) {
            auto t = Get(ls);
            if (t.kind == PP_END) break;
            bool nobol = false;
            if (!TryExpand(ls, t, nobol)) out.push_back(std::move(t));
        }
        return out;
    }

    // If `t` names a macro that expands here, pushes the expansion to the
    // stream's front and returns true. `pendbol` is set when the expansion
    // consumed a line's first token and produced nothing, so the next token
    // starts the line instead.
    bool TryExpand(Stream &s, const PPToken &t, bool &pendbol) {
        if (t.kind != PP_IDENT) return false;
        if (t.text == "__LINE__" || t.text == "__FILE__") {
            PPToken r = t;
            r.hide.clear();
            if (t.text == "__LINE__") { r.kind = PP_NUMBER; r.text = to_string(t.line); }
            else { r.kind = PP_STRING; r.text = Quote(files[t.file]); }
            Unget(s, std::move(r));
            return true;
        }
        auto it = macros.find(t.text);
        if (it == macros.end()) return false;
        auto &m = it->second;
        if (InHide(t, m.id)) return false;
        vector<PPToken> result;
        if (!m.funclike) {
            result = Subst(m, {}, Union(t.hide, m.id), t);
        } else {
            auto n = Get(s);
            if (!Is(n, "(")) {
                if (n.kind != PP_END) Unget(s, std::move(n));
                return false;
            }
            PPToken rparen;
            auto args = CollectArgs(s, t, m, rparen);
            result = Subst(m, args, Union(Intersect(t.hide, rparen.hide), m.id), t);
        }
        if (result.empty()) {
            if (t.bol) pendbol = true;
        } else {
            result[0].space = t.space;
            if (pendbol || t.bol) { result[0].bol = true; pendbol = false; }
        }
        for (auto i = result.size(); i-- > 0;) Unget(s, std::move(result[i]));
        return true;
    }

    vector<vector<PPToken>> CollectArgs(Stream &s, const PPToken &name, const Macro &m,
                                        PPToken &rparen) {
        vector<vector<PPToken>> args(1);
        int depth = 0;
        for (;;) {
            auto t = Get(s);
            if (t.kind == PP_END) Error(name, cat("unterminated argument list of macro '", name.text, "'"));
            if (t.kind == PP_PRAGMA) Error(t, "#pragma inside macro arguments");
            if (Is(t, "(")) depth++;
            if (Is(t, ")")) {
                if (depth == 0) { rparen = t; break; }
                depth--;
            }
            if (Is(t, ",") && depth == 0) {
                args.emplace_back();
                continue;
            }
            t.bol = false;
            args.back().push_back(std::move(t));
        }
        // f() is one empty argument, which a one-parameter macro takes and a
        // zero-parameter macro does not need.
        if (m.params.empty() && args.size() == 1 && args[0].empty()) args.clear();
        if (args.size() != m.params.size())
            Error(name, cat("macro '", name.text, "' expects ", m.params.size(), " argument",
                            m.params.size() == 1 ? "" : "s", ", given ", args.size()));
        for (auto &a : args) if (!a.empty()) a[0].space = false;
        return args;
    }

    vector<PPToken> Subst(const Macro &m, const vector<vector<PPToken>> &args,
                          const vector<int> &hs, const PPToken &inv) {
        vector<PPToken> out;
        auto &body = m.body;
        auto place = [&](PPToken t) {
            t.file = inv.file;
            t.line = inv.line;
            t.bol = false;
            return t;
        };
        auto append = [&](const vector<PPToken> &ts, bool space) {
            for (size_t k = 0; k < ts.size(); k++) {
                auto t = place(ts[k]);
                if (k == 0) t.space = space;
                out.push_back(std::move(t));
            }
        };
        for (size_t i = 0; i < body.size(); i++) {
            auto &t = body[i];
            bool next_paste = i + 1 < body.size() && Is(body[i + 1], "##");
            if (m.funclike && Is(t, "#")) {
                // Checked at definition: a parameter follows.
                int p = ParamIndex(m, body[i + 1]);
                PPToken str = place(t);
                str.kind = PP_STRING;
                str.text = Stringize(args[(size_t)p]);
                str.space = t.space;
                out.push_back(std::move(str));
                i++;
                continue;
            }
            if (Is(t, "##")) {
                auto &n = body[i + 1];   // checked at definition: not the end
                int p = ParamIndex(m, n);
                if (p >= 0) Glue(out, args[(size_t)p], inv, n);
                else Glue(out, { n }, inv, n);
                i++;
                continue;
            }
            int p = ParamIndex(m, t);
            if (p >= 0) {
                auto &actual = args[(size_t)p];
                if (next_paste) {
                    if (actual.empty()) {
                        // A placemarker: the token after ## is pasted onto
                        // nothing, so it appears as it is, unexpanded if a
                        // parameter.
                        auto &n = body[i + 2];
                        int q = ParamIndex(m, n);
                        if (q >= 0) append(args[(size_t)q], t.space);
                        else out.push_back(place(n));
                        i += 2;
                    } else {
                        append(actual, t.space);
                    }
                } else {
                    append(ExpandList(actual), t.space);
                }
                continue;
            }
            out.push_back(place(t));
        }
        for (auto &t : out)
            for (auto id : hs) if (!InHide(t, id)) t.hide.push_back(id);
        return out;
    }

    // Pastes the first of `ts` onto the last of `out`, and appends the rest.
    void Glue(vector<PPToken> &out, const vector<PPToken> &ts, const PPToken &inv, const PPToken &op) {
        if (ts.empty()) return;
        if (out.empty()) {
            for (auto t : ts) { t.file = inv.file; t.line = inv.line; t.bol = false; out.push_back(t); }
            return;
        }
        auto &l = out.back();
        auto text = l.text + ts[0].text;
        auto lexed = LexAll(text, inv.file);
        if (lexed.size() != 1)
            Error(op, cat("pasting '", l.text, "' and '", ts[0].text, "' does not give a valid token"));
        auto hide = Intersect(l.hide, ts[0].hide);
        l.kind = lexed[0].kind;
        l.text = std::move(text);
        l.hide = std::move(hide);
        for (size_t k = 1; k < ts.size(); k++) {
            auto t = ts[k];
            t.file = inv.file;
            t.line = inv.line;
            t.bol = false;
            out.push_back(std::move(t));
        }
    }

    static string Stringize(const vector<PPToken> &ts) {
        string s;
        for (size_t i = 0; i < ts.size(); i++) {
            if (i > 0 && ts[i].space) s += ' ';
            if (ts[i].kind == PP_STRING || ts[i].kind == PP_CHAR) {
                for (auto c : ts[i].text) {
                    if (c == '"' || c == '\\') s += '\\';
                    s += c;
                }
            } else {
                s += ts[i].text;
            }
        }
        return cat("\"", s, "\"");
    }

    static string Quote(const string &s) {
        string q = "\"";
        for (auto c : s) {
            if (c == '"' || c == '\\') q += '\\';
            q += c;
        }
        return q + "\"";
    }
};

// The preprocessed text, as `cc -E` would show it without line markers: one
// line per logical source line that has tokens, a space where the source had
// whitespace. For tests and for looking at what the interpreter sees.
inline string PPText(const vector<PPToken> &ts) {
    string out;
    for (auto &t : ts) {
        if (t.kind == PP_END) break;
        if (t.bol) {
            if (!out.empty()) out += '\n';
        } else if (t.space && !out.empty() && out.back() != '\n') {
            out += ' ';
        }
        if (t.kind == PP_PRAGMA) out += "#pragma ";
        out += t.text;
    }
    if (!out.empty()) out += '\n';
    return out;
}

}  // namespace interp
}  // namespace goose
