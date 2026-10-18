// Goose compiler — the interpreter's C parser (docs/design/interpreter.md §4,
// steps 2 and 3 of §14). It consumes the preprocessor's tokens and builds a
// translation unit with every type laid out (ctypes.h) and every expression
// typed (cast.h), folding integer constant expressions as it goes, which
// array sizes, enum values and case labels need. Declarations are parsed
// first, with function bodies kept as token ranges, and the bodies after, so
// a body sees every file-scope declaration. The dialect is C99 as codegen
// and the runtime write it, plus what the standard headers' declarations
// need: no bitfields, no variable-length arrays, no anonymous members, and
// __attribute__ dropped.
#pragma once

namespace goose {
namespace interp {

struct CSym {
    enum Kind { S_TYPEDEF, S_VAR, S_FUNC, S_ENUMCONST } kind;
    CType *type = nullptr;
    CDecl *decl = nullptr;
    int64_t value = 0;   // S_ENUMCONST
};

struct Scope {
    unordered_map<string, CSym> syms;
    unordered_map<string, CStruct *> tags;
    Scope *parent = nullptr;

    const CSym *Find(const string &name) const {
        for (auto s = this; s; s = s->parent)
            if (auto it = s->syms.find(name); it != s->syms.end()) return &it->second;
        return nullptr;
    }
    CStruct *FindTag(const string &name) const {
        for (auto s = this; s; s = s->parent)
            if (auto it = s->tags.find(name); it != s->tags.end()) return it->second;
        return nullptr;
    }
};

// A translation unit's declarations, in order, with its file scope.
struct CUnit {
    vector<CDecl *> decls;        // variables and functions, each once
    vector<CDecl *> typedefs;     // for dumps; the scope resolves them
    vector<pair<string, int64_t>> enumconsts;
    Scope global;
};

struct CParser {
    CParser(const vector<PPToken> &toks, const vector<string> &files, TypeTable &types)
        : toks(toks), files(files), types(types) {
        scope = &unit.global;
    }

    CUnit &Unit() { return unit; }

    // The whole unit: declarations, function definitions, and the #pragma
    // pack lines between them, then the function bodies.
    CUnit &ParseUnit() {
        while (!AtEnd()) {
            if (Cur().kind == PP_PRAGMA) { Pragma(Cur()); pos++; continue; }
            if (Is(";")) { pos++; continue; }
            ExternalDeclaration();
        }
        ParseBodies();
        return unit;
    }

    // ---- Diagnostics

    string Where(const PPToken &t) const { return cat(files[t.file], ":", t.line); }

    [[noreturn]] void ErrorAt(int file, int line, string_view msg) const {
        throw CompileError { cat(files[file], ":", line, ": error: ", msg) };
    }
    [[noreturn]] void Error(const PPToken &at, string_view msg) const { ErrorAt(at.file, at.line, msg); }
    [[noreturn]] void Error(const Expr *at, string_view msg) const { ErrorAt(at->file, at->line, msg); }
    [[noreturn]] void Error(const Init *at, string_view msg) const { ErrorAt(at->file, at->line, msg); }

    // ---- Nodes

    Expr *NewExpr(ExprKind k, const PPToken &at) {
        exprs.push_back(make_unique<Expr>());
        auto e = exprs.back().get();
        e->kind = k;
        e->file = at.file;
        e->line = at.line;
        return e;
    }
    Expr *NewExpr(ExprKind k, const Expr *at) {
        exprs.push_back(make_unique<Expr>());
        auto e = exprs.back().get();
        e->kind = k;
        e->file = at->file;
        e->line = at->line;
        return e;
    }
    Init *NewInit(const PPToken &at) {
        inits.push_back(make_unique<Init>());
        auto i = inits.back().get();
        i->file = at.file;
        i->line = at.line;
        return i;
    }
    CDecl *NewDecl(const PPToken &at) {
        decls.push_back(make_unique<CDecl>());
        auto d = decls.back().get();
        d->file = at.file;
        d->line = at.line;
        return d;
    }
    Stmt *NewStmt(StmtKind k, const PPToken &at) {
        stmts.push_back(make_unique<Stmt>());
        auto s = stmts.back().get();
        s->kind = k;
        s->file = at.file;
        s->line = at.line;
        return s;
    }

    // ---- Tokens

    const vector<PPToken> &toks;
    const vector<string> &files;
    TypeTable &types;
    size_t pos = 0;
    Scope *scope;
    int pack = 0;
    vector<int> packstack;

    const PPToken &Cur() const { return toks[pos]; }
    const PPToken &Peek(size_t k = 1) const {
        return toks[min(pos + k, toks.size() - 1)];
    }
    bool AtEnd() const { return Cur().kind == PP_END; }
    bool Is(const char *p) const { return Cur().kind == PP_PUNCT && Cur().text == p; }
    bool IsKw(const char *k) const { return Cur().kind == PP_IDENT && Cur().text == k; }
    static bool Is(const PPToken &t, const char *p) { return t.kind == PP_PUNCT && t.text == p; }
    bool Accept(const char *p) {
        if (!Is(p)) return false;
        pos++;
        return true;
    }
    const PPToken &Expect(const char *p) {
        if (!Is(p)) Error(Cur(), cat("expected '", p, "', found '", Spelling(Cur()), "'"));
        return toks[pos++];
    }
    static string Spelling(const PPToken &t) {
        if (t.kind == PP_END) return "end of file";
        if (t.kind == PP_PRAGMA) return "#pragma";
        return t.text;
    }
    const PPToken &ExpectIdent() {
        if (Cur().kind != PP_IDENT || IsKeyword(Cur().text))
            Error(Cur(), cat("expected an identifier, found '", Spelling(Cur()), "'"));
        return toks[pos++];
    }

    // Skips a balanced (...), [...] or {...} starting at the current opener.
    void SkipBalanced() {
        auto &open = Cur();
        const char *close = Is("(") ? ")" : Is("[") ? "]" : "}";
        int depth = 0;
        for (;;) {
            if (AtEnd()) Error(open, "unbalanced brackets");
            if (Is("(") || Is("[") || Is("{")) depth++;
            else if (Is(")") || Is("]") || Is("}")) {
                depth--;
                if (depth == 0) {
                    if (!Is(close)) Error(Cur(), "mismatched brackets");
                    pos++;
                    return;
                }
            }
            pos++;
        }
    }

    static bool IsKeyword(const string &s) {
        static const unordered_set<string> kws = {
            "auto", "break", "case", "char", "const", "continue", "default", "do", "double",
            "else", "enum", "extern", "float", "for", "goto", "if", "inline", "int", "long",
            "register", "restrict", "return", "short", "signed", "sizeof", "static", "struct",
            "switch", "typedef", "union", "unsigned", "void", "volatile", "while", "_Bool",
            "_Noreturn", "_Thread_local", "_Alignof", "_Alignas", "_Static_assert",
        };
        return kws.count(s) > 0;
    }

    // Decorations the parser drops: GCC attributes, MSVC declspecs, and the
    // spellings of inline and restrict compilers add.
    void SkipDecorations() {
        for (;;) {
            if (Cur().kind != PP_IDENT) return;
            auto &s = Cur().text;
            if (s == "__attribute__" || s == "__attribute" || s == "__declspec") {
                pos++;
                if (!Is("(")) Error(Cur(), cat("expected '(' after ", s));
                SkipBalanced();
            } else if (s == "__extension__" || s == "__inline" || s == "__inline__" ||
                       s == "__restrict" || s == "__restrict__" || s == "_Noreturn" ||
                       s == "__volatile__") {
                pos++;
            } else {
                return;
            }
        }
    }

    // ---- #pragma

    void Pragma(const PPToken &t) {
        // pack(push, n), pack(push), pack(pop), pack(n), pack()
        auto &s = t.text;
        if (s.compare(0, 4, "pack") != 0) return;   // once, GCC ..., fp_contract: nothing to do
        vector<string> words;
        string cur;
        for (auto c : s.substr(4)) {
            if (isalnum((uint8_t)c) || c == '_') cur += c;
            else {
                if (!cur.empty()) words.push_back(cur);
                cur.clear();
            }
        }
        if (!cur.empty()) words.push_back(cur);
        auto value = [&](const string &w) {
            int v = atoi(w.c_str());
            if (v != 1 && v != 2 && v != 4 && v != 8 && v != 16)
                Error(t, cat("#pragma pack: invalid alignment ", w));
            return v;
        };
        if (words.empty()) pack = 0;
        else if (words[0] == "push") {
            packstack.push_back(pack);
            if (words.size() > 1) pack = value(words[1]);
        } else if (words[0] == "pop") {
            if (packstack.empty()) Error(t, "#pragma pack(pop) without a push");
            pack = packstack.back();
            packstack.pop_back();
        } else {
            pack = value(words[0]);
        }
    }

    // ---- Declarations

    struct DeclSpec {
        CType *type = nullptr;
        bool istypedef = false, isstatic = false, isextern = false, istls = false;
        PPToken at;
    };

    // Whether the current token can begin a type: a type keyword, a
    // qualifier, a record or enum, or a typedef name not shadowed.
    bool IsTypeStart() const { return IsTypeStart(Cur()); }
    bool IsTypeStart(const PPToken &t) const {
        if (t.kind != PP_IDENT) return false;
        static const unordered_set<string> kws = {
            "void", "char", "short", "int", "long", "float", "double", "signed", "unsigned",
            "_Bool", "struct", "union", "enum", "const", "volatile", "restrict", "__restrict",
            "typedef", "static", "extern", "inline", "register", "auto", "_Thread_local",
            "__thread", "_Noreturn", "__attribute__", "__extension__", "__inline", "__inline__",
            "__declspec",
        };
        if (kws.count(t.text)) return true;
        auto s = scope->Find(t.text);
        return s && s->kind == CSym::S_TYPEDEF;
    }

    DeclSpec ParseDeclSpec() {
        DeclSpec ds;
        ds.at = Cur();
        int nvoid = 0, nchar = 0, nshort = 0, nint = 0, nlong = 0, nfloat = 0, ndouble = 0,
            nsigned = 0, nunsigned = 0, nbool = 0;
        CType *named = nullptr;
        for (;;) {
            SkipDecorations();
            if (Cur().kind != PP_IDENT) break;
            auto &s = Cur().text;
            if (s == "typedef") ds.istypedef = true;
            else if (s == "static") ds.isstatic = true;
            else if (s == "extern") ds.isextern = true;
            else if (s == "_Thread_local" || s == "__thread") ds.istls = true;
            else if (s == "inline" || s == "register" || s == "auto" || s == "const" ||
                     s == "volatile" || s == "restrict") {}
            else if (s == "void") nvoid++;
            else if (s == "char") nchar++;
            else if (s == "short") nshort++;
            else if (s == "int") nint++;
            else if (s == "long") nlong++;
            else if (s == "float") nfloat++;
            else if (s == "double") ndouble++;
            else if (s == "signed") nsigned++;
            else if (s == "unsigned") nunsigned++;
            else if (s == "_Bool") nbool++;
            else if (s == "struct" || s == "union") {
                if (named) Error(Cur(), "two types in one declaration");
                named = ParseRecord();
                continue;
            } else if (s == "enum") {
                if (named) Error(Cur(), "two types in one declaration");
                named = ParseEnum();
                continue;
            } else {
                // A typedef name, unless a type was already given, in which
                // case it is the declarator's name.
                if (named || nvoid || nchar || nshort || nint || nlong || nfloat || ndouble ||
                    nsigned || nunsigned || nbool) break;
                auto sym = scope->Find(s);
                if (!sym || sym->kind != CSym::S_TYPEDEF) break;
                named = sym->type;
            }
            pos++;
        }
        int basics = nvoid + nchar + nshort + nint + nlong + nfloat + ndouble + nsigned +
                     nunsigned + nbool;
        if (named) {
            if (basics) Error(ds.at, "two types in one declaration");
            ds.type = named;
            return ds;
        }
        if (!basics) Error(Cur(), cat("expected a type, found '", Spelling(Cur()), "'"));
        if (nsigned && nunsigned) Error(ds.at, "both signed and unsigned");
        if (nlong > 2 || nshort > 1 || nchar > 1 || nint > 1 || nvoid > 1 || nbool > 1 ||
            nfloat > 1 || ndouble > 1)
            Error(ds.at, "invalid type specifiers");
        bool uns = nunsigned > 0;
        if (nvoid) {
            if (basics > 1) Error(ds.at, "invalid type specifiers");
            ds.type = &types.tvoid;
        } else if (nbool) {
            if (basics > 1) Error(ds.at, "invalid type specifiers");
            ds.type = &types.tbool;
        } else if (nfloat) {
            if (basics > 1) Error(ds.at, "invalid type specifiers");
            ds.type = &types.tfloat;
        } else if (ndouble) {
            if (nchar || nshort || nint || nsigned || nunsigned || nlong > 1)
                Error(ds.at, "invalid type specifiers");
            ds.type = &types.tdouble;   // long double is double here (nothing uses it)
        } else if (nchar) {
            if (nshort || nint || nlong) Error(ds.at, "invalid type specifiers");
            ds.type = uns ? &types.tuchar : nsigned ? &types.tschar : &types.tchar;
        } else if (nshort) {
            if (nlong) Error(ds.at, "invalid type specifiers");
            ds.type = uns ? &types.tushort : &types.tshort;
        } else if (nlong == 2) {
            ds.type = uns ? &types.tullong : &types.tllong;
        } else if (nlong == 1) {
            ds.type = uns ? &types.tulong : &types.tlong;
        } else {
            ds.type = uns ? &types.tuint : &types.tint;
        }
        return ds;
    }

    CType *ParseRecord() {
        auto &kw = toks[pos++];
        bool isunion = kw.text == "union";
        SkipDecorations();
        string tag;
        if (Cur().kind == PP_IDENT && !IsKeyword(Cur().text)) tag = toks[pos++].text;
        SkipDecorations();
        CStruct *s = nullptr;
        if (!Is("{")) {
            if (tag.empty()) Error(kw, "expected a tag or a body");
            s = scope->FindTag(tag);
            if (!s) {
                s = types.NewStruct(tag, isunion);
                s->file = kw.file;
                s->line = kw.line;
                scope->tags[tag] = s;
            }
            if (s->isunion != isunion) Error(kw, cat("'", tag, "' is a ", s->isunion ? "union" : "struct"));
            return types.Record(s);
        }
        if (!tag.empty()) {
            auto it = scope->tags.find(tag);
            if (it != scope->tags.end()) {
                s = it->second;
                if (s->complete) Error(kw, cat("redefinition of '", tag, "'"));
                if (s->isunion != isunion) Error(kw, cat("'", tag, "' is a ", s->isunion ? "union" : "struct"));
            }
        }
        if (!s) {
            s = types.NewStruct(tag, isunion);
            if (!tag.empty()) scope->tags[tag] = s;
        }
        s->file = kw.file;
        s->line = kw.line;
        pos++;   // {
        while (!Is("}")) {
            if (AtEnd()) Error(kw, "unterminated record");
            if (Cur().kind == PP_PRAGMA) { Pragma(Cur()); pos++; continue; }
            if (Is(";")) { pos++; continue; }
            auto ds = ParseDeclSpec();
            if (ds.istypedef || ds.isstatic || ds.isextern)
                Error(ds.at, "storage class in a member declaration");
            if (Is(";")) Error(Cur(), "anonymous members are not supported");
            for (;;) {
                string name;
                auto &at = Cur();
                auto t = Declarator(ds.type, &name, false, nullptr);
                if (Is(":")) Error(Cur(), "bitfields are not supported");
                if (!t->IsComplete())
                    Error(at, cat("member '", name, "' has incomplete type ", TypeName(t)));
                if (t->kind == TK_FUNC) Error(at, cat("member '", name, "' is a function"));
                if (s->Find(name)) Error(at, cat("duplicate member '", name, "'"));
                s->fields.push_back({ name, t, 0, at.line });
                if (!Accept(",")) break;
            }
            Expect(";");
        }
        pos++;   // }
        SkipDecorations();
        types.Layout(*s, pack);
        auto t = types.Record(s);
        types.CompleteArraysOf(t);
        return t;
    }

    // Enumerations are int; their tags are not recorded, since nothing but
    // `enum tag` references needs them, and those are int too.
    CType *ParseEnum() {
        auto &kw = toks[pos++];
        SkipDecorations();
        if (Cur().kind == PP_IDENT && !IsKeyword(Cur().text)) pos++;
        if (!Accept("{")) return &types.tint;
        int64_t next = 0;
        while (!Is("}")) {
            auto &name = ExpectIdent();
            if (Accept("=")) {
                auto e = ConditionalExpr();
                if (!e->IsConst()) Error(name, cat("enumerator '", name.text, "' is not a constant"));
                next = e->Int();
            }
            if (scope->syms.count(name.text)) Error(name, cat("redefinition of '", name.text, "'"));
            CSym sym { CSym::S_ENUMCONST, &types.tint, nullptr, next };
            scope->syms[name.text] = sym;
            unit.enumconsts.emplace_back(name.text, next);
            next++;
            if (!Accept(",")) break;
        }
        Expect("}");
        (void)kw;
        return &types.tint;
    }

    // A declarator applied to `base`: pointers, then the grouping or name,
    // then the array and function suffixes. A grouped declarator is parsed
    // twice: once skipped to reach its suffixes, then for real on the type
    // those suffixes make. `abstract` allows no name; `paramnames` collects
    // a function declarator's parameter names.
    CType *Declarator(CType *base, string *name, bool abstract, vector<string> *paramnames) {
        for (;;) {
            SkipDecorations();
            if (Is("*")) {
                pos++;
                base = types.Pointer(base);
                while (IsKw("const") || IsKw("volatile") || IsKw("restrict") || IsKw("__restrict"))
                    pos++;
                continue;
            }
            break;
        }
        if (Is("(") && !IsTypeStart(Peek()) && !Is(Peek(), ")")) {
            // The suffixes after the group apply to what the group's inner
            // declarator derives from; the parameters named are those of the
            // function declarator next to the name, which is inside.
            auto inner = pos;
            SkipBalanced();
            auto t = Suffixes(base, nullptr);
            auto after = pos;
            pos = inner + 1;
            auto r = Declarator(t, name, abstract, paramnames);
            Expect(")");
            pos = after;
            return r;
        }
        if (Cur().kind == PP_IDENT && !IsKeyword(Cur().text)) {
            if (!name) Error(Cur(), "unexpected name in an abstract declarator");
            *name = toks[pos++].text;
        } else if (!abstract) {
            Error(Cur(), cat("expected a name, found '", Spelling(Cur()), "'"));
        }
        return Suffixes(base, paramnames);
    }

    CType *Suffixes(CType *t, vector<string> *paramnames) {
        if (Is("[")) {
            auto &open = toks[pos++];
            int64_t n = -1;
            if (!Is("]")) {
                auto e = ConditionalExpr();
                if (!e->IsConst()) Error(open, "array size is not a constant");
                n = e->Int();
                if (n < 0) Error(open, "negative array size");
            }
            Expect("]");
            auto elem = Suffixes(t, nullptr);
            if (!elem->IsComplete()) Error(open, cat("array of incomplete type ", TypeName(elem)));
            if (elem->kind == TK_FUNC) Error(open, "array of functions");
            return types.Array(elem, n);
        }
        if (Is("(")) {
            auto &open = toks[pos++];
            if (t->kind == TK_FUNC) Error(open, "function returning a function");
            if (t->kind == TK_ARRAY) Error(open, "function returning an array");
            vector<CType *> params;
            bool variadic = false, unspecified = false;
            if (Is(")")) {
                unspecified = true;
            } else if (IsKw("void") && Is(Peek(), ")")) {
                pos++;
            } else {
                for (;;) {
                    if (Accept("...")) { variadic = true; break; }
                    auto ds = ParseDeclSpec();
                    if (ds.istypedef || ds.isstatic || ds.isextern || ds.istls)
                        Error(ds.at, "storage class on a parameter");
                    string pname;
                    auto pt = Declarator(ds.type, &pname, true, nullptr);
                    if (pt->kind == TK_ARRAY) pt = types.Pointer(pt->elem);
                    else if (pt->kind == TK_FUNC) pt = types.Pointer(pt);
                    else if (pt->kind == TK_VOID) Error(ds.at, "parameter of type void");
                    params.push_back(pt);
                    if (paramnames) paramnames->push_back(pname);
                    if (!Accept(",")) break;
                }
            }
            Expect(")");
            SkipDecorations();
            // Only the outermost function declarator names the parameters;
            // a function returning a pointer to a function has its own list
            // parsed above without collecting names, so this is a plain
            // check that nothing follows.
            if (Is("(") || Is("[")) Error(Cur(), "unexpected declarator suffix after a parameter list");
            return types.Func(t, params, variadic, unspecified);
        }
        return t;
    }

    // One declaration or function definition at file scope.
    void ExternalDeclaration() {
        auto ds = ParseDeclSpec();
        if (Accept(";")) return;   // a record or enum declared on its own
        for (;;) {
            string name;
            vector<string> paramnames;
            auto &at = Cur();
            auto t = Declarator(ds.type, &name, false, &paramnames);
            SkipDecorations();
            if (ds.istypedef) {
                if (Is("=")) Error(Cur(), "a typedef cannot be initialized");
                DefineTypedef(at, name, t);
            } else if (t->kind == TK_FUNC) {
                auto d = DeclareFunction(at, name, t, ds);
                if (Is("{")) {
                    if (d->defined) Error(at, cat("redefinition of '", name, "'"));
                    d->defined = true;
                    d->paramnames = paramnames;
                    d->bodybegin = pos;
                    SkipBalanced();
                    d->bodyend = pos;
                    return;
                }
            } else {
                DeclareVariable(at, name, t, ds);
            }
            if (!Accept(",")) break;
        }
        Expect(";");
    }

    void DefineTypedef(const PPToken &at, const string &name, CType *t) {
        if (auto s = scope->syms.find(name); s != scope->syms.end()) {
            if (s->second.kind == CSym::S_TYPEDEF && s->second.type == t) return;
            Error(at, cat("redefinition of '", name, "'"));
        }
        auto d = NewDecl(at);
        d->kind = D_TYPEDEF;
        d->name = name;
        d->type = t;
        scope->syms[name] = CSym { CSym::S_TYPEDEF, t, d, 0 };
        unit.typedefs.push_back(d);
    }

    static bool Compatible(CType *a, CType *b) {
        if (a == b) return true;
        if (a->kind == TK_FUNC && b->kind == TK_FUNC) {
            if (a->elem != b->elem) return false;
            if (a->unspecified || b->unspecified) return true;
            return a->params == b->params && a->variadic == b->variadic;
        }
        if (a->kind == TK_ARRAY && b->kind == TK_ARRAY)
            return a->elem == b->elem && (a->count < 0 || b->count < 0);
        return false;
    }

    CDecl *DeclareFunction(const PPToken &at, const string &name, CType *t, const DeclSpec &ds) {
        if (auto s = scope->syms.find(name); s != scope->syms.end()) {
            if (s->second.kind != CSym::S_FUNC || !Compatible(s->second.type, t))
                Error(at, cat("conflicting declarations of '", name, "'"));
            auto d = s->second.decl;
            if (!t->unspecified) { d->type = t; s->second.type = t; }
            if (ds.isstatic) d->isstatic = true;
            return d;
        }
        auto d = NewDecl(at);
        d->kind = D_FUNC;
        d->name = name;
        d->type = t;
        d->isstatic = ds.isstatic;
        d->isextern = !ds.isstatic;
        scope->syms[name] = CSym { CSym::S_FUNC, t, d, 0 };
        unit.decls.push_back(d);
        return d;
    }

    void DeclareVariable(const PPToken &at, const string &name, CType *t, const DeclSpec &ds) {
        if (t->kind == TK_VOID) Error(at, cat("variable '", name, "' has type void"));
        CDecl *d = nullptr;
        if (auto s = scope->syms.find(name); s != scope->syms.end()) {
            if (s->second.kind != CSym::S_VAR || !Compatible(s->second.type, t))
                Error(at, cat("conflicting declarations of '", name, "'"));
            d = s->second.decl;
            if (t->IsComplete()) { d->type = t; s->second.type = t; }
            if (!ds.isextern) d->isextern = false;   // `extern int x; int x;` defines x
        } else {
            d = NewDecl(at);
            d->kind = D_VAR;
            d->name = name;
            d->type = t;
            d->isstatic = ds.isstatic;
            d->isextern = ds.isextern;
            d->istls = ds.istls;
            scope->syms[name] = CSym { CSym::S_VAR, t, d, 0 };
            unit.decls.push_back(d);
        }
        if (Accept("=")) {
            if (d->init) Error(at, cat("'", name, "' initialized twice"));
            d->init = ParseInit();
            d->defined = true;
            d->isextern = false;
            d->type = TypeInit(d->init, d->type);
            scope->syms[name].type = d->type;
        }
        if (!d->type->IsComplete() && !d->isextern)
            Error(at, cat("variable '", name, "' has incomplete type ", TypeName(d->type)));
    }

    // ---- Statements

    // Every defined function's body. Static locals and file-scope compound
    // literals append to the unit's declarations while bodies parse, hence
    // the index loop.
    void ParseBodies() {
        for (size_t i = 0; i < unit.decls.size(); i++) {
            auto d = unit.decls[i];
            if (d->kind == D_FUNC && d->defined && !d->body) ParseBody(d);
        }
    }

    void ParseBody(CDecl *fn) {
        auto &at = toks[fn->bodybegin];
        auto savedpos = pos;
        auto savedscope = scope;
        curfn = fn;
        labels.clear();
        gotos.clear();
        switches.clear();
        PushScope();
        auto ft = fn->type;
        for (size_t i = 0; i < ft->params.size(); i++) {
            if (i >= fn->paramnames.size() || fn->paramnames[i].empty())
                Error(at, cat("parameter ", i + 1, " of '", fn->name, "' has no name"));
            auto &name = fn->paramnames[i];
            auto p = NewDecl(at);
            p->kind = D_VAR;
            p->name = name;
            p->type = ft->params[i];
            p->islocal = true;
            fn->params.push_back(p);
            fn->locals.push_back(p);
            Bind(at, name, CSym { CSym::S_VAR, p->type, p, 0 });
        }
        pos = fn->bodybegin;
        fn->body = Block(false);
        if (pos != fn->bodyend) Error(at, "function body ends unexpectedly");
        for (auto &[label, where] : gotos)
            if (!labels.count(label)) ErrorAt(where.first, where.second, cat("label '", label, "' is not defined"));
        PopScope();
        scope = savedscope;
        pos = savedpos;
        curfn = nullptr;
    }

    void PushScope() {
        scopes.push_back(make_unique<Scope>());
        scopes.back()->parent = scope;
        scope = scopes.back().get();
    }
    void PopScope() { scope = scope->parent; }

    // A name in the current scope; redeclaring one there is an error,
    // shadowing an outer one is not.
    void Bind(const PPToken &at, const string &name, const CSym &sym) {
        if (scope->syms.count(name)) Error(at, cat("redefinition of '", name, "'"));
        scope->syms[name] = sym;
    }

    // `{ ... }`, in a scope of its own unless the caller opened one (a
    // function's outermost block shares the parameters' scope).
    Stmt *Block(bool ownscope) {
        auto &open = Expect("{");
        auto st = NewStmt(S_BLOCK, open);
        if (ownscope) PushScope();
        while (!Is("}")) {
            if (AtEnd()) Error(open, "unterminated block");
            st->items.push_back(Statement());
        }
        pos++;
        if (ownscope) PopScope();
        return st;
    }

    Stmt *Statement() {
        auto &t = Cur();
        if (t.kind == PP_PRAGMA) { Pragma(t); pos++; return NewStmt(S_EMPTY, t); }
        if (Is(";")) { pos++; return NewStmt(S_EMPTY, t); }
        if (Is("{")) return Block(true);
        if (t.kind == PP_IDENT) {
            auto &k = t.text;
            if (k == "if") {
                pos++;
                auto st = NewStmt(S_IF, t);
                st->expr = Condition();
                st->a = Statement();
                if (IsKw("else")) { pos++; st->b = Statement(); }
                return st;
            }
            if (k == "while") {
                pos++;
                auto st = NewStmt(S_WHILE, t);
                st->expr = Condition();
                st->a = Statement();
                return st;
            }
            if (k == "do") {
                pos++;
                auto st = NewStmt(S_DO, t);
                st->a = Statement();
                if (!IsKw("while")) Error(Cur(), cat("expected 'while', found '", Spelling(Cur()), "'"));
                pos++;
                st->expr = Condition();
                Expect(";");
                return st;
            }
            if (k == "for") {
                pos++;
                auto st = NewStmt(S_FOR, t);
                Expect("(");
                PushScope();
                if (Is(";")) st->init = NewStmt(S_EMPTY, toks[pos++]);
                else if (IsTypeStart()) st->init = Declaration();
                else {
                    st->init = NewStmt(S_EXPR, Cur());
                    st->init->expr = Expression();
                    Expect(";");
                }
                if (!Is(";")) st->expr = Scalar(Expression(), "loop condition");
                Expect(";");
                if (!Is(")")) st->step = Expression();
                Expect(")");
                st->a = Statement();
                PopScope();
                return st;
            }
            if (k == "switch") {
                pos++;
                auto st = NewStmt(S_SWITCH, t);
                Expect("(");
                auto e = Decay(Expression());
                if (!e->type->IsInteger()) Error(t, cat("switch on a non-integer (", TypeName(e->type), ")"));
                st->expr = Promote(e);
                Expect(")");
                switches.push_back(st);
                st->a = Statement();
                switches.pop_back();
                return st;
            }
            if (k == "case" || k == "default") {
                pos++;
                if (switches.empty()) Error(t, cat("'", k, "' outside a switch"));
                auto sw = switches.back();
                auto st = NewStmt(k == "case" ? S_CASE : S_DEFAULT, t);
                if (k == "case") {
                    auto v = ConditionalExpr();
                    if (!v->IsConst()) Error(t, "case label is not a constant");
                    st->value = (int64_t)Truncate(v->ival, sw->expr->type);
                }
                for (auto c : sw->cases)
                    if (c->kind == st->kind && (st->kind == S_DEFAULT || c->value == st->value))
                        Error(t, k == "case" ? cat("duplicate case ", st->value) : "duplicate default");
                sw->cases.push_back(st);
                Expect(":");
                st->a = Statement();
                return st;
            }
            if (k == "break" || k == "continue") {
                pos++;
                Expect(";");
                return NewStmt(k == "break" ? S_BREAK : S_CONTINUE, t);
            }
            if (k == "goto") {
                pos++;
                auto st = NewStmt(S_GOTO, t);
                st->label = ExpectIdent().text;
                gotos.emplace_back(st->label, make_pair(t.file, t.line));
                Expect(";");
                return st;
            }
            if (k == "return") {
                pos++;
                auto st = NewStmt(S_RETURN, t);
                auto rt = curfn->type->elem;
                if (!Is(";")) {
                    auto e = Expression();
                    if (rt->kind == TK_VOID) Error(t, "return with a value in a void function");
                    st->expr = ConvertTo(e, rt, t, "return");
                } else if (rt->kind != TK_VOID) {
                    Error(t, "return without a value");
                }
                Expect(";");
                return st;
            }
            if (Is(Peek(), ":") && !IsTypeStart(t)) {
                pos += 2;
                auto st = NewStmt(S_LABEL, t);
                st->label = k;
                if (!labels.insert(k).second) Error(t, cat("duplicate label '", k, "'"));
                st->a = Is("}") ? NewStmt(S_EMPTY, Cur()) : Statement();   // `label: }` as compilers allow
                return st;
            }
            if (IsTypeStart(t)) return Declaration();
        }
        auto st = NewStmt(S_EXPR, t);
        st->expr = Expression();
        Expect(";");
        return st;
    }

    // `( expr )` of a scalar type.
    Expr *Condition() {
        auto &open = Expect("(");
        auto e = Scalar(Expression(), "condition");
        Expect(")");
        (void)open;
        return e;
    }

    Expr *Scalar(Expr *e, const char *what) {
        e = Decay(e);
        if (!e->type->IsScalar()) Error(e, cat(what, " has type ", TypeName(e->type)));
        return e;
    }

    // A declaration in a block: locals of the function, in order of their
    // initialization; typedefs and record definitions in the block's scope;
    // static locals as objects of the unit; prototypes and externs as the
    // file-scope declarations they are.
    Stmt *Declaration() {
        auto st = NewStmt(S_DECL, Cur());
        auto ds = ParseDeclSpec();
        if (Accept(";")) return st;
        for (;;) {
            string name;
            auto &at = Cur();
            auto t = Declarator(ds.type, &name, false, nullptr);
            SkipDecorations();
            if (ds.istypedef) {
                if (Is("=")) Error(Cur(), "a typedef cannot be initialized");
                DefineTypedef(at, name, t);
            } else if (t->kind == TK_FUNC || ds.isextern) {
                if (Is("{")) Error(Cur(), "function definition in a block");
                if (Is("=")) Error(Cur(), "an extern declaration cannot be initialized");
                auto saved = scope;
                scope = &unit.global;
                if (t->kind == TK_FUNC) DeclareFunction(at, name, t, ds);
                else DeclareVariable(at, name, t, ds);
                scope = saved;
            } else {
                if (t->kind == TK_VOID) Error(at, cat("variable '", name, "' has type void"));
                auto d = NewDecl(at);
                d->kind = D_VAR;
                d->name = name;
                d->type = t;
                d->isstatic = ds.isstatic;
                d->istls = ds.istls;
                d->islocal = !ds.isstatic;
                if (Accept("=")) {
                    d->init = ParseInit();
                    d->defined = true;
                    d->type = TypeInit(d->init, d->type);
                }
                if (!d->type->IsComplete()) Error(at, cat("variable '", name, "' has incomplete type ", TypeName(d->type)));
                Bind(at, name, CSym { CSym::S_VAR, d->type, d, 0 });
                if (ds.isstatic) unit.decls.push_back(d);
                else {
                    curfn->locals.push_back(d);
                    st->decls.push_back(d);
                }
            }
            if (!Accept(",")) break;
        }
        Expect(";");
        return st;
    }

    // ---- Initializers

    Init *ParseInit() {
        auto in = NewInit(Cur());
        if (!Accept("{")) {
            in->expr = AssignmentExpr();
            return in;
        }
        while (!Is("}")) {
            auto item = NewInit(Cur());
            while (Is(".") || Is("[")) {
                Designator d;
                if (Accept(".")) d.field = ExpectIdent().text;
                else {
                    pos++;
                    d.index = ConditionalExpr();
                    if (!d.index->IsConst()) Error(Cur(), "array designator is not a constant");
                    Expect("]");
                }
                item->desig.push_back(d);
            }
            if (!item->desig.empty()) Expect("=");
            if (Is("{")) {
                auto sub = ParseInit();
                item->items = sub->items;
                item->expr = nullptr;
            } else {
                item->expr = AssignmentExpr();
            }
            in->items.push_back(item);
            if (!Accept(",")) break;
        }
        Expect("}");
        return in;
    }

    // Types an initializer against the type it initializes, as C's rules
    // read it (§6.7.8): a scalar's expression is converted to it; a brace
    // list is matched to an aggregate member by member, with designators
    // setting the position and braces elided where a member's own list is
    // not written. The result is fully braced and positional: an array's or
    // struct's items are one per member in order (null where none was
    // written, which lowering zero-fills), a union's the one member chosen
    // (`ufield`). Returns the type, completed where it was an open array.
    CType *TypeInit(Init *in, CType *t) {
        if (!in->IsList()) return InitScalar(in, t);
        if (t->IsScalar()) {
            // `int x = { 1 }`: a list of one around a scalar.
            if (in->items.size() != 1 || !in->items[0]->desig.empty())
                Error(in, cat("a scalar's initializer must be one expression, not a list"));
            auto item = in->items[0];
            TypeInit(item, t);
            in->expr = item->expr;
            in->items.clear();
            return t;
        }
        if (!t->IsRecord() && t->kind != TK_ARRAY) Error(in, cat("cannot initialize ", TypeName(t)));
        if (t->IsRecord() && !t->rec->complete) Error(in, cat("initializer for incomplete type ", TypeName(t)));
        auto flat = in->items;
        vector<Init *> placed;
        int64_t cursor = 0, limit = -1;
        if (t->kind == TK_ARRAY) {
            limit = t->count;
            // A string in braces, `char s[] = { "ab" }`.
            if (flat.size() == 1 && flat[0]->expr && flat[0]->expr->kind == E_STR &&
                t->elem->size == 1 && flat[0]->desig.empty()) {
                in->items.clear();
                in->expr = flat[0]->expr;
                return StringInit(in, t);
            }
        } else {
            limit = (int64_t)t->rec->fields.size();
            if (t->kind == TK_UNION) limit = 1;
        }
        size_t i = 0;
        while (i < flat.size()) {
            auto item = flat[i];
            if (!item->desig.empty()) {
                if (item->desig.size() > 1) Error(item, "nested designators are not supported");
                auto &d = item->desig[0];
                if (t->kind == TK_ARRAY) {
                    if (!d.index) Error(item, "field designator in an array initializer");
                    cursor = d.index->Int();
                    if (cursor < 0 || (t->count >= 0 && cursor >= t->count))
                        Error(item, cat("array designator [", cursor, "] out of range"));
                } else {
                    if (d.index) Error(item, "array designator in a record initializer");
                    auto &fs = t->rec->fields;
                    size_t k = 0;
                    while (k < fs.size() && fs[k].name != d.field) k++;
                    if (k == fs.size()) Error(item, cat("no member '", d.field, "' in ", TypeName(t)));
                    cursor = (int64_t)k;
                    if (t->kind == TK_UNION) { in->ufield = (int)k; cursor = 0; }
                }
                item->desig.clear();
            }
            if (limit >= 0 && cursor >= limit) Error(item, cat("too many initializers for ", TypeName(t)));
            CType *mt = t->kind == TK_ARRAY ? t->elem
                      : t->kind == TK_UNION ? t->rec->fields[(size_t)in->ufield].type
                      : t->rec->fields[(size_t)cursor].type;
            auto sub = Consume(flat, i, mt, item);
            if ((size_t)cursor >= placed.size()) placed.resize((size_t)cursor + 1, nullptr);
            placed[(size_t)cursor] = sub;
            cursor++;
        }
        if (t->kind == TK_ARRAY && t->count < 0) t = types.Array(t->elem, (int64_t)placed.size());
        placed.resize((size_t)(t->kind == TK_ARRAY ? t->count : limit), nullptr);
        in->items = placed;
        return t;
    }

    // The initializer of one member of type `mt`, taken from the flat items
    // at `i`: its own brace list, an expression of its type, a string for a
    // char array, or, braces elided, as many following items as it has
    // members of its own.
    Init *Consume(const vector<Init *> &flat, size_t &i, CType *mt, const Init *at) {
        auto item = flat[i];
        if (item->IsList()) {
            i++;
            TypeInit(item, mt);
            return item;
        }
        if (mt->IsScalar()) {
            i++;
            InitScalar(item, mt);
            return item;
        }
        auto et = item->expr->type;
        if (et == mt || (mt->kind == TK_ARRAY && mt->elem->size == 1 && item->expr->kind == E_STR)) {
            i++;
            if (mt->kind == TK_ARRAY) StringInit(item, mt);
            return item;
        }
        // Elided braces: fill the aggregate's members from here on, up to a
        // designator, which belongs to the enclosing list.
        auto sub = NewInit(at->file == 0 && at->line == 0 ? *item : *at);
        int64_t n = mt->kind == TK_ARRAY ? mt->count : mt->kind == TK_UNION ? 1 : (int64_t)mt->rec->fields.size();
        if (n < 0) Error(item, "braces elided around an array of unknown size");
        for (int64_t k = 0; k < n && i < flat.size() && flat[i]->desig.empty(); k++) {
            CType *kt = mt->kind == TK_ARRAY ? mt->elem : mt->rec->fields[(size_t)k].type;
            sub->items.push_back(Consume(flat, i, kt, at));
        }
        sub->items.resize((size_t)n, nullptr);
        return sub;
    }

    Init *NewInit(const Init &like) {
        inits.push_back(make_unique<Init>());
        auto i = inits.back().get();
        i->file = like.file;
        i->line = like.line;
        return i;
    }

    // An initializer that is one expression; returns the type, completed
    // where a string sized an open char array.
    CType *InitScalar(Init *in, CType *t) {
        auto e = in->expr;
        if (t->IsRecord()) {
            if (Decay(e)->type != t) Error(e, cat("initializer of type ", TypeName(e->type), " for ", TypeName(t)));
            return t;
        }
        if (t->kind == TK_ARRAY) {
            if (e->kind == E_STR && t->elem->size == 1) return StringInit(in, t);
            Error(e, cat("an array's initializer must be a brace list or a string, not ", TypeName(e->type)));
        }
        in->expr = ConvertTo(e, t, in, "initialization");
        return t;
    }

    // A char array from a string: the array may leave the NUL out when the
    // string fills it exactly (`uint8_t s[6] = " nodes"`).
    CType *StringInit(Init *in, CType *t) {
        auto n = (int64_t)in->expr->sval.size();
        if (t->count < 0) return types.Array(t->elem, n);
        if (n - 1 > t->count) Error(in, cat("string of ", n - 1, " bytes does not fit [", t->count, "]"));
        return t;
    }

    // ---- Expressions

    Expr *Expression() {
        auto e = AssignmentExpr();
        while (Is(",")) {
            auto &op = toks[pos++];
            auto r = NewExpr(E_COMMA, op);
            r->a = e;
            r->b = AssignmentExpr();
            r->type = r->b->type;
            e = r;
        }
        return e;
    }

    static bool IsAssignOp(const PPToken &t) {
        static const unordered_set<string> ops = { "=", "+=", "-=", "*=", "/=", "%=", "&=",
                                                   "|=", "^=", "<<=", ">>=" };
        return t.kind == PP_PUNCT && ops.count(t.text) > 0;
    }

    Expr *AssignmentExpr() {
        auto l = ConditionalExpr();
        if (!IsAssignOp(Cur())) return l;
        auto &op = toks[pos++];
        if (!l->lvalue) Error(op, "assignment to something that is not an lvalue");
        if (l->type->kind == TK_ARRAY) Error(op, "assignment to an array");
        if (!l->type->IsComplete()) Error(op, "assignment to an incomplete type");
        auto r = AssignmentExpr();
        auto e = NewExpr(E_ASSIGN, op);
        e->op = op.text;
        e->a = l;
        if (op.text == "=") {
            r = ConvertTo(r, l->type, op, "assignment");
        } else {
            // `a op= b` is done in the type `a op b` would be, which the
            // operation type records for lowering: the common type for
            // arithmetic, the promoted left type for shifts, the pointer for
            // pointer arithmetic. Only b is converted here; a is loaded and
            // converted by lowering, which has the lvalue.
            r = Decay(r);
            if (r->type->kind == TK_PTR) Error(op, cat("pointer on the right of ", op.text));
            auto o = op.text.substr(0, op.text.size() - 1);
            Expr *lv = l;
            auto t = CheckArith(op, lv, r, o);
            if (!t) { e->optype = l->type; r = Promote(r); }
            else if (o == "<<" || o == ">>") e->optype = Promoted(l->type);
            else { e->optype = t; r = Cast(r, t); }
        }
        e->b = r;
        e->type = l->type;
        return e;
    }

    Expr *ConditionalExpr() {
        auto c = BinaryExpr(1);
        if (!Is("?")) return c;
        auto &op = toks[pos++];
        c = Decay(c);
        if (!c->type->IsScalar()) Error(op, "condition is not a scalar");
        auto a = Decay(Expression());
        Expect(":");
        auto b = Decay(ConditionalExpr());
        auto e = NewExpr(E_COND, op);
        e->a = c;
        if (a->type->IsArith() && b->type->IsArith()) {
            auto t = UsualArith(a->type, b->type);
            a = Cast(a, t);
            b = Cast(b, t);
            e->type = t;
        } else if (a->type->kind == TK_PTR || b->type->kind == TK_PTR) {
            auto at = a->type, bt = b->type;
            CType *p;
            if (at->kind == TK_PTR && bt->kind == TK_PTR) {
                // Two pointers: the same type, or one is void *, which wins.
                if (at == bt) p = at;
                else if (at->elem->kind == TK_VOID) p = at;
                else if (bt->elem->kind == TK_VOID) p = bt;
                else Error(op, cat("pointer types differ in ?: (", TypeName(at), ", ", TypeName(bt), ")"));
            } else {
                p = at->kind == TK_PTR ? at : bt;
                auto &other = at->kind == TK_PTR ? bt : at;
                if (!other->IsInteger()) Error(op, "pointer and non-pointer in ?:");
            }
            a = Cast(a, p);
            b = Cast(b, p);
            e->type = p;
        } else if (a->type == b->type) {
            e->type = a->type;
        } else {
            Error(op, cat("types differ in ?: (", TypeName(a->type), ", ", TypeName(b->type), ")"));
        }
        e->b = a;
        e->c = b;
        if (c->IsConst() && a->IsConst() && b->IsConst()) return c->ival ? a : b;
        return e;
    }

    static int Prec(const PPToken &t) {
        if (t.kind != PP_PUNCT) return 0;
        static const unordered_map<string, int> table = {
            { "||", 1 }, { "&&", 2 }, { "|", 3 }, { "^", 4 }, { "&", 5 },
            { "==", 6 }, { "!=", 6 }, { "<", 7 }, { ">", 7 }, { "<=", 7 }, { ">=", 7 },
            { "<<", 8 }, { ">>", 8 }, { "+", 9 }, { "-", 9 }, { "*", 10 }, { "/", 10 }, { "%", 10 },
        };
        auto it = table.find(t.text);
        return it == table.end() ? 0 : it->second;
    }

    Expr *BinaryExpr(int minprec) {
        auto l = CastExpr();
        for (;;) {
            int p = Prec(Cur());
            if (p == 0 || p < minprec) return l;
            auto &op = toks[pos++];
            auto r = BinaryExpr(p + 1);
            l = Binary(op, l, r);
        }
    }

    Expr *CastExpr() {
        if (Is("(") && IsTypeStart(Peek())) {
            auto &open = toks[pos++];
            auto t = ParseTypeName();
            Expect(")");
            if (Is("{")) return CompoundLiteral(open, t);
            auto e = Decay(CastExpr());
            if (t->kind == TK_VOID) {
                auto c = NewExpr(E_CAST, open);
                c->a = e;
                c->type = t;
                return c;
            }
            if (!t->IsScalar()) Error(open, cat("cast to a non-scalar type ", TypeName(t)));
            if (!e->type->IsScalar()) Error(open, cat("cast of a non-scalar (", TypeName(e->type), ")"));
            return Cast(e, t);
        }
        return UnaryExpr();
    }

    // A type name in a cast, sizeof or compound literal: specifiers and an
    // abstract declarator.
    CType *ParseTypeName() {
        auto ds = ParseDeclSpec();
        if (ds.istypedef || ds.isstatic || ds.isextern) Error(ds.at, "storage class in a type name");
        return Declarator(ds.type, nullptr, true, nullptr);
    }

    // A compound literal is an object: a local of the function it is in,
    // initialized each time it is evaluated, or a static one at file scope.
    Expr *CompoundLiteral(const PPToken &open, CType *t) {
        auto e = NewExpr(E_COMPOUND, open);
        e->init = ParseInit();
        t = TypeInit(e->init, t);
        if (!t->IsComplete()) Error(open, cat("compound literal of incomplete type ", TypeName(t)));
        e->type = t;
        e->lvalue = true;
        auto d = NewDecl(open);
        d->kind = D_VAR;
        d->name = cat("(", TypeName(t), "){...}");
        d->type = t;
        d->init = e->init;
        d->defined = true;
        if (curfn) {
            d->islocal = true;
            curfn->locals.push_back(d);
        } else {
            d->isstatic = true;
            unit.decls.push_back(d);
        }
        e->decl = d;
        return e;
    }

    Expr *UnaryExpr() {
        auto &t = Cur();
        if (Is("++") || Is("--")) {
            pos++;
            auto a = UnaryExpr();
            return IncDec(t, a, false);
        }
        if (Is("&")) {
            pos++;
            auto a = CastExpr();
            if (a->kind == E_DECAY) a = a->a;   // &array, &function: the thing itself
            if (!a->lvalue && !(a->kind == E_VAR && a->decl->kind == D_FUNC))
                Error(t, "address of something that is not an lvalue");
            auto e = NewExpr(E_ADDR, t);
            e->a = a;
            e->type = types.Pointer(a->type);
            return e;
        }
        if (Is("*")) {
            pos++;
            auto a = Decay(CastExpr());
            if (a->type->kind != TK_PTR) Error(t, cat("dereference of a non-pointer (", TypeName(a->type), ")"));
            return Deref(t, a);
        }
        if (Is("-") || Is("+") || Is("~") || Is("!")) {
            pos++;
            auto a = Decay(CastExpr());
            auto e = NewExpr(E_UNARY, t);
            e->op = t.text;
            if (t.text == "!") {
                if (!a->type->IsScalar()) Error(t, "operand of ! is not a scalar");
                e->a = a;
                e->type = &types.tint;
                if (a->IsConst()) return IntConst(t, a->ival == 0, &types.tint);
                return e;
            }
            if (!a->type->IsArith() || (t.text == "~" && !a->type->IsInteger()))
                Error(t, cat("operand of unary ", t.text, " has type ", TypeName(a->type)));
            a = Promote(a);
            e->a = a;
            e->type = a->type;
            if (a->IsConst()) {
                uint64_t v = t.text == "-" ? (uint64_t)0 - a->ival : t.text == "~" ? ~a->ival : a->ival;
                return IntConst(t, v, a->type);
            }
            if (a->kind == E_FLOAT && t.text != "~") {
                a->fval = t.text == "-" ? -a->fval : a->fval;
                return a;
            }
            return e;
        }
        if (IsKw("sizeof") || IsKw("_Alignof")) {
            bool align = t.text == "_Alignof";
            pos++;
            CType *ty;
            if (Is("(") && IsTypeStart(Peek())) {
                pos++;
                ty = ParseTypeName();
                Expect(")");
            } else {
                if (align) Error(t, "_Alignof needs a type");
                ty = UnaryExpr()->type;
            }
            if (!ty->IsComplete()) Error(t, cat("sizeof an incomplete type (", TypeName(ty), ")"));
            return IntConst(t, (uint64_t)(align ? ty->align : ty->size), &types.tulong);
        }
        return PostfixExpr();
    }

    Expr *IncDec(const PPToken &op, Expr *a, bool post) {
        if (!a->lvalue) Error(op, cat("operand of ", op.text, " is not an lvalue"));
        if (!a->type->IsScalar()) Error(op, cat("operand of ", op.text, " is not a scalar"));
        if (a->type->kind == TK_PTR && !a->type->elem->IsComplete())
            Error(op, "arithmetic on a pointer to an incomplete type");
        auto e = NewExpr(E_INCDEC, op);
        e->op = op.text;
        e->a = a;
        e->post = post;
        e->type = a->type;
        return e;
    }

    Expr *Deref(const PPToken &at, Expr *p) {
        auto e = NewExpr(E_DEREF, at);
        e->a = p;
        e->type = p->type->elem;
        if (e->type->kind == TK_VOID) Error(at, "dereference of a void pointer");
        e->lvalue = e->type->kind != TK_FUNC;
        return e;
    }

    Expr *PostfixExpr() {
        auto e = PrimaryExpr();
        for (;;) {
            auto &t = Cur();
            if (Is("[")) {
                pos++;
                auto i = Expression();
                Expect("]");
                auto sum = Binary(t, e, i, "+");
                if (sum->type->kind != TK_PTR) Error(t, "subscript of a non-pointer");
                e = Deref(t, sum);
            } else if (Is("(")) {
                pos++;
                e = Call(t, e);
            } else if (Is(".") || Is("->")) {
                pos++;
                auto &name = ExpectIdent();
                Expr *base = e;
                if (t.text == "->") {
                    base = Decay(e);
                    if (base->type->kind != TK_PTR) Error(t, "-> on a non-pointer");
                    base = Deref(t, base);
                }
                if (!base->type->IsRecord()) Error(t, cat("member access on a non-record (", TypeName(base->type), ")"));
                if (!base->type->rec->complete) Error(t, cat("member access on an incomplete record (", TypeName(base->type), ")"));
                auto f = base->type->rec->Find(name.text);
                if (!f) Error(name, cat("no member '", name.text, "' in ", TypeName(base->type)));
                auto m = NewExpr(E_FIELD, t);
                m->a = base;
                m->field = f;
                m->type = f->type;
                m->lvalue = base->lvalue;
                e = m;
            } else if (Is("++") || Is("--")) {
                pos++;
                e = IncDec(t, e, true);
            } else {
                return e;
            }
        }
    }

    Expr *Call(const PPToken &at, Expr *callee) {
        callee = Decay(callee);
        if (callee->type->kind != TK_PTR || callee->type->elem->kind != TK_FUNC)
            Error(at, cat("call of a non-function (", TypeName(callee->type), ")"));
        auto ft = callee->type->elem;
        auto e = NewExpr(E_CALL, at);
        e->a = callee;
        e->type = ft->elem;
        while (!Is(")")) {
            auto arg = Decay(AssignmentExpr());
            size_t i = e->args.size();
            if (!ft->unspecified && i < ft->params.size()) {
                arg = ConvertTo(arg, ft->params[i], at, cat("argument ", i + 1));
            } else if (ft->unspecified || ft->variadic) {
                // Default argument promotions.
                if (arg->type->kind == TK_FLOAT && arg->type->size == 4) arg = Cast(arg, &types.tdouble);
                else if (arg->type->IsInteger()) arg = Promote(arg);
            } else {
                Error(at, cat("too many arguments (", ft->params.size(), " expected)"));
            }
            e->args.push_back(arg);
            if (!Accept(",")) break;
        }
        Expect(")");
        if (!ft->unspecified && e->args.size() < ft->params.size())
            Error(at, cat("too few arguments (", ft->params.size(), " expected, ", e->args.size(), " given)"));
        return e;
    }

    Expr *PrimaryExpr() {
        auto &t = toks[pos];
        switch (t.kind) {
            case PP_NUMBER: pos++; return NumberConst(t);
            case PP_CHAR: pos++; return IntConst(t, (uint64_t)CharConst(t), &types.tint);
            case PP_STRING: {
                auto e = NewExpr(E_STR, t);
                while (Cur().kind == PP_STRING) e->sval += Unescape(toks[pos++]);
                e->sval += '\0';
                e->type = types.Array(&types.tchar, (int64_t)e->sval.size());
                e->lvalue = true;
                return e;
            }
            case PP_PUNCT:
                if (Is("(")) {
                    pos++;
                    auto e = Expression();
                    Expect(")");
                    return e;
                }
                break;
            case PP_IDENT: {
                if (IsKeyword(t.text)) break;
                pos++;
                auto s = scope->Find(t.text);
                if (!s) Error(t, cat("undeclared identifier '", t.text, "'"));
                if (s->kind == CSym::S_ENUMCONST) return IntConst(t, (uint64_t)s->value, &types.tint);
                if (s->kind == CSym::S_TYPEDEF) Error(t, cat("'", t.text, "' is a type"));
                auto e = NewExpr(E_VAR, t);
                e->decl = s->decl;
                e->type = s->type;
                e->lvalue = s->kind == CSym::S_VAR;
                return e;
            }
            default: break;
        }
        Error(t, cat("expected an expression, found '", Spelling(t), "'"));
    }

    // ---- Constants

    Expr *IntConst(const PPToken &at, uint64_t v, CType *t) {
        auto e = NewExpr(E_INT, at);
        e->type = t;
        e->ival = Truncate(v, t);
        return e;
    }
    Expr *IntConst(const Expr *at, uint64_t v, CType *t) {
        auto e = NewExpr(E_INT, at);
        e->type = t;
        e->ival = Truncate(v, t);
        return e;
    }

    // A value as its type stores it: truncated, and sign-extended if signed.
    static uint64_t Truncate(uint64_t v, const CType *t) {
        if (t->kind == TK_BOOL) return v != 0;
        if (t->size >= 8) return v;
        int bits = (int)t->size * 8;
        v &= ((uint64_t)1 << bits) - 1;
        if (!t->uns && (v >> (bits - 1))) v |= ~(uint64_t)0 << bits;
        return v;
    }

    Expr *NumberConst(const PPToken &t) {
        auto &s = t.text;
        bool isfloat = false;
        bool hex = s.size() > 1 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X');
        for (auto c : s) {
            if (c == '.') isfloat = true;
            if (!hex && (c == 'e' || c == 'E')) isfloat = true;
            if (hex && (c == 'p' || c == 'P')) isfloat = true;
        }
        if (isfloat) {
            auto e = NewExpr(E_FLOAT, t);
            char *end;
            e->fval = strtod(s.c_str(), &end);
            string suffix(end);
            if (suffix == "f" || suffix == "F") e->type = &types.tfloat;
            else if (suffix.empty() || suffix == "l" || suffix == "L") e->type = &types.tdouble;
            else Error(t, cat("invalid floating constant '", s, "'"));
            if (e->type == &types.tfloat) e->fval = (float)e->fval;
            return e;
        }
        size_t i = 0;
        int base = 10;
        if (hex) { base = 16; i = 2; }
        else if (s.size() > 1 && s[0] == '0') { base = 8; i = 1; }
        uint64_t v = 0;
        for (; i < s.size(); i++) {
            auto c = s[i];
            int d = isdigit((uint8_t)c) ? c - '0' : isalpha((uint8_t)c) ? tolower(c) - 'a' + 10 : 99;
            if (d >= base) break;
            if (v > (~(uint64_t)0 - (uint64_t)d) / (uint64_t)base) Error(t, "integer constant too large");
            v = v * (uint64_t)base + (uint64_t)d;
        }
        bool u = false;
        int ls = 0;
        for (; i < s.size(); i++) {
            auto c = tolower(s[i]);
            if (c == 'u' && !u) u = true;
            else if (c == 'l' && ls < 2 && (ls == 0 || tolower(s[i - 1]) == 'l')) ls++;
            else Error(t, cat("invalid integer constant '", s, "'"));
        }
        // The first of int, unsigned, long, unsigned long that holds it and
        // the suffix allows; a decimal constant skips unsigned int.
        bool fits32 = v <= 0x7fffffffULL, fitsu32 = v <= 0xffffffffULL,
             fits64 = v <= 0x7fffffffffffffffULL;
        CType *ty;
        if (u) ty = ls == 0 && fitsu32 ? &types.tuint : &types.tulong;
        else if (ls == 0 && fits32) ty = &types.tint;
        else if (ls == 0 && base != 10 && fitsu32) ty = &types.tuint;
        else if (fits64) ty = &types.tlong;
        else ty = &types.tulong;
        return IntConst(t, v, ty);
    }

    int64_t CharConst(const PPToken &t) {
        auto s = Unescape(t);
        if (s.size() != 1) Error(t, "multi-character constants are not supported");
        return (int8_t)s[0];
    }

    // The bytes a string or character literal spells.
    string Unescape(const PPToken &t) const {
        auto &s = t.text;
        string out;
        for (size_t i = 1; i + 1 < s.size(); i++) {
            auto c = s[i];
            if (c != '\\') { out += c; continue; }
            c = s[++i];
            switch (c) {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case 'a': out += '\a'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'v': out += '\v'; break;
                case '\\': case '\'': case '"': case '?': out += c; break;
                case 'x': {
                    int v = 0, n = 0;
                    while (i + 1 < s.size() && isxdigit((uint8_t)s[i + 1])) {
                        auto d = s[++i];
                        v = v * 16 + (isdigit((uint8_t)d) ? d - '0' : tolower(d) - 'a' + 10);
                        n++;
                    }
                    if (!n) Error(t, "\\x with no hex digits");
                    out += (char)v;
                    break;
                }
                default:
                    if (c >= '0' && c <= '7') {
                        int v = c - '0';
                        for (int k = 0; k < 2 && i + 1 < s.size() && s[i + 1] >= '0' && s[i + 1] <= '7'; k++)
                            v = v * 8 + (s[++i] - '0');
                        out += (char)v;
                    } else {
                        Error(t, cat("unknown escape \\", string(1, c)));
                    }
            }
        }
        return out;
    }

    // ---- Typing

    // An array or function used as a value is a pointer to it.
    Expr *Decay(Expr *e) {
        if (e->type->kind == TK_ARRAY || e->type->kind == TK_FUNC) {
            auto d = NewExpr(E_DECAY, e);
            d->a = e;
            d->type = types.Pointer(e->type->kind == TK_ARRAY ? e->type->elem : e->type);
            return d;
        }
        return e;
    }

    // The integer promotions: anything narrower than int becomes int.
    Expr *Promote(Expr *e) {
        auto t = e->type;
        if (t->kind == TK_BOOL || (t->kind == TK_INT && t->size < 4)) return Cast(e, &types.tint);
        return e;
    }

    CType *Promoted(CType *t) {
        if (t->kind == TK_BOOL || (t->kind == TK_INT && t->size < 4)) return &types.tint;
        return t;
    }

    // The usual arithmetic conversions on two arithmetic types.
    CType *UsualArith(CType *a, CType *b) {
        if (a->kind == TK_FLOAT || b->kind == TK_FLOAT) {
            int size = max(a->kind == TK_FLOAT ? a->size : 0, b->kind == TK_FLOAT ? b->size : 0);
            return size == 8 ? &types.tdouble : &types.tfloat;
        }
        a = Promoted(a);
        b = Promoted(b);
        if (a == b) return a;
        if (a->uns == b->uns) return a->size >= b->size ? a : b;
        auto u = a->uns ? a : b, s = a->uns ? b : a;
        if (u->size >= s->size) return u;
        return s;   // the signed type holds every value of the narrower unsigned one
    }

    // A conversion node, folded when the operand is a constant.
    Expr *Cast(Expr *e, CType *t) {
        if (e->type == t) return e;
        if (e->kind == E_INT && t->IsInteger()) return IntConst(e, e->ival, t);
        if (e->kind == E_INT && t->kind == TK_FLOAT) {
            auto f = NewExpr(E_FLOAT, e);
            f->type = t;
            f->fval = e->type->uns ? (double)e->ival : (double)(int64_t)e->ival;
            if (t->size == 4) f->fval = (float)f->fval;
            return f;
        }
        if (e->kind == E_FLOAT && t->kind == TK_FLOAT) {
            auto f = NewExpr(E_FLOAT, e);
            f->type = t;
            f->fval = t->size == 4 ? (double)(float)e->fval : e->fval;
            return f;
        }
        if (e->kind == E_FLOAT && t->IsInteger()) {
            // In range by construction where the runtime writes one; the VM
            // handles the rest at run time.
            auto v = e->fval;
            if (t->kind == TK_BOOL) return IntConst(e, v != 0, t);
            if (v >= -9223372036854775808.0 && v < 9223372036854775808.0)
                return IntConst(e, (uint64_t)(int64_t)v, t);
            if (v >= 0 && v < 18446744073709551616.0) return IntConst(e, (uint64_t)v, t);
        }
        auto c = NewExpr(E_CAST, e);
        c->a = e;
        c->type = t;
        return c;
    }

    // The implicit conversion of a value to the type of what it is assigned
    // to, passed as, returned as or initializes.
    Expr *ConvertTo(Expr *e, CType *t, const PPToken &at, const string &what) {
        return ConvertAt(e, t, at.file, at.line, what);
    }
    Expr *ConvertTo(Expr *e, CType *t, const Init *at, const string &what) {
        return ConvertAt(e, t, at->file, at->line, what);
    }
    Expr *ConvertAt(Expr *e, CType *t, int file, int line, const string &what) {
        e = Decay(e);
        auto from = e->type;
        if (from == t) return e;
        if (t->IsArith() && from->IsArith()) return Cast(e, t);
        if (t->kind == TK_PTR && from->kind == TK_PTR) return Cast(e, t);   // void * both ways, and the rest
        if (t->kind == TK_PTR && from->IsInteger()) return Cast(e, t);      // null constants, and C's leniency
        if (t->IsInteger() && from->kind == TK_PTR) return Cast(e, t);
        if (t->IsRecord() && from == t) return e;
        ErrorAt(file, line, cat("cannot convert ", TypeName(from), " to ", TypeName(t), " in ", what));
    }

    static bool IsIntegerZero(const Expr *e) { return e->kind == E_INT && e->ival == 0; }

    // Checks an arithmetic or pointer binary operation's operands, and
    // returns their common type where there is one (null for pointer
    // arithmetic, whose result type is the pointer's).
    CType *CheckArith(const PPToken &op, Expr *&l, Expr *&r, const string &o) {
        auto lt = l->type, rt = r->type;
        bool arith = o == "+" || o == "-" || o == "*" || o == "/" || o == "%";
        bool bits = o == "&" || o == "|" || o == "^";
        bool shift = o == "<<" || o == ">>";
        if (o == "+" && lt->kind == TK_PTR && rt->IsInteger()) return PointerArith(op, lt);
        if (o == "+" && rt->kind == TK_PTR && lt->IsInteger()) { swap(l, r); return PointerArith(op, rt); }
        if (o == "-" && lt->kind == TK_PTR && rt->IsInteger()) return PointerArith(op, lt);
        if (o == "-" && lt->kind == TK_PTR && rt->kind == TK_PTR) {
            if (lt != rt && lt->elem->kind != TK_VOID && rt->elem->kind != TK_VOID)
                Error(op, cat("subtraction of pointers to different types (", TypeName(lt), ", ", TypeName(rt), ")"));
            PointerArith(op, lt);
            return &types.tlong;
        }
        if (arith && (!lt->IsArith() || !rt->IsArith()))
            Error(op, cat("operands of ", o, " have types ", TypeName(lt), " and ", TypeName(rt)));
        if ((bits || o == "%") && (!lt->IsInteger() || !rt->IsInteger()))
            Error(op, cat("operands of ", o, " must be integers (", TypeName(lt), ", ", TypeName(rt), ")"));
        if (shift) {
            if (!lt->IsInteger() || !rt->IsInteger())
                Error(op, cat("operands of ", o, " must be integers (", TypeName(lt), ", ", TypeName(rt), ")"));
            l = Promote(l);
            r = Promote(r);
            return l->type;
        }
        return UsualArith(lt, rt);
    }

    CType *PointerArith(const PPToken &op, CType *p) {
        if (!p->elem->IsComplete() || p->elem->kind == TK_FUNC)
            Error(op, cat("arithmetic on a pointer to ", TypeName(p->elem)));
        return nullptr;
    }

    Expr *Binary(const PPToken &op, Expr *l, Expr *r) { return Binary(op, l, r, op.text); }

    Expr *Binary(const PPToken &op, Expr *l, Expr *r, const string &o) {
        l = Decay(l);
        r = Decay(r);
        auto e = NewExpr(E_BINARY, op);
        e->op = o;
        if (o == "&&" || o == "||") {
            if (!l->type->IsScalar() || !r->type->IsScalar()) Error(op, cat("operands of ", o, " are not scalars"));
            e->a = l;
            e->b = r;
            e->type = &types.tint;
            if (l->IsConst() && r->IsConst())
                return IntConst(op, o == "&&" ? (l->ival != 0 && r->ival != 0) : (l->ival != 0 || r->ival != 0), e->type);
            return e;
        }
        bool compare = o == "==" || o == "!=" || o == "<" || o == ">" || o == "<=" || o == ">=";
        if (compare) {
            auto lt = l->type, rt = r->type;
            if (lt->IsArith() && rt->IsArith()) {
                auto t = UsualArith(lt, rt);
                l = Cast(l, t);
                r = Cast(r, t);
            } else if (lt->kind == TK_PTR && rt->kind == TK_PTR) {
                if (lt != rt && lt->elem->kind != TK_VOID && rt->elem->kind != TK_VOID)
                    Error(op, cat("comparison of pointers to different types (", TypeName(lt), ", ", TypeName(rt), ")"));
            } else if (lt->kind == TK_PTR && rt->IsInteger()) {
                r = Cast(r, lt);
            } else if (rt->kind == TK_PTR && lt->IsInteger()) {
                l = Cast(l, rt);
            } else {
                Error(op, cat("comparison of ", TypeName(lt), " and ", TypeName(rt)));
            }
            e->a = l;
            e->b = r;
            e->type = &types.tint;
            if (l->IsConst() && r->IsConst()) return IntConst(op, Compare(o, l, r), e->type);
            return e;
        }
        if (o == "-" && l->type->kind == TK_PTR && r->type->kind == TK_PTR) {
            CheckArith(op, l, r, o);
            e->a = l;
            e->b = r;
            e->type = &types.tlong;
            return e;
        }
        auto t = CheckArith(op, l, r, o);
        if (!t) {
            // Pointer arithmetic: the result is the pointer, the integer
            // operand promoted; lowering scales it.
            e->a = l;
            e->b = Promote(r);
            e->type = l->type;
            return e;
        }
        if (o != "<<" && o != ">>") {
            l = Cast(l, t);
            r = Cast(r, t);
        }
        e->a = l;
        e->b = r;
        e->type = t;
        if (l->IsConst() && r->IsConst() && t->IsInteger()) return IntConst(op, Arith(op, o, l, r, t), t);
        if (l->kind == E_FLOAT && r->kind == E_FLOAT && (o == "+" || o == "-" || o == "*" || o == "/")) {
            auto f = NewExpr(E_FLOAT, op);
            f->type = t;
            double a = l->fval, b = r->fval;
            f->fval = o == "+" ? a + b : o == "-" ? a - b : o == "*" ? a * b : a / b;
            if (t->size == 4) f->fval = (float)f->fval;
            return f;
        }
        return e;
    }

    static bool Compare(const string &o, const Expr *l, const Expr *r) {
        bool uns = l->type->kind == TK_PTR || l->type->uns;
        auto a = l->ival, b = r->ival;
        auto sa = (int64_t)a, sb = (int64_t)b;
        if (o == "==") return a == b;
        if (o == "!=") return a != b;
        if (o == "<") return uns ? a < b : sa < sb;
        if (o == ">") return uns ? a > b : sa > sb;
        if (o == "<=") return uns ? a <= b : sa <= sb;
        return uns ? a >= b : sa >= sb;
    }

    uint64_t Arith(const PPToken &op, const string &o, const Expr *l, const Expr *r, const CType *t) {
        auto a = l->ival, b = r->ival;
        bool uns = t->uns;
        int bits = (int)t->size * 8;
        if (o == "+") return a + b;
        if (o == "-") return a - b;
        if (o == "*") return a * b;
        if (o == "/" || o == "%") {
            if (b == 0) Error(op, "division by zero in a constant expression");
            if (uns) return o == "/" ? a / b : a % b;
            if ((int64_t)b == -1) return o == "/" ? (uint64_t)0 - a : 0;
            return o == "/" ? (uint64_t)((int64_t)a / (int64_t)b) : (uint64_t)((int64_t)a % (int64_t)b);
        }
        if (o == "&") return a & b;
        if (o == "|") return a | b;
        if (o == "^") return a ^ b;
        if (o == "<<") {
            if (b >= (uint64_t)bits) Error(op, "shift count out of range in a constant expression");
            return a << b;
        }
        if (o == ">>") {
            if (b >= (uint64_t)bits) Error(op, "shift count out of range in a constant expression");
            return uns ? a >> b : (uint64_t)((int64_t)a >> b);
        }
        Error(op, cat("unknown operator ", o));
    }

private:
    CUnit unit;
    vector<unique_ptr<Expr>> exprs;
    vector<unique_ptr<Init>> inits;
    vector<unique_ptr<CDecl>> decls;
    vector<unique_ptr<Stmt>> stmts;
    vector<unique_ptr<Scope>> scopes;
    CDecl *curfn = nullptr;                   // the function whose body is being parsed
    unordered_set<string> labels;             // its labels so far
    vector<pair<string, pair<int, int>>> gotos;   // its gotos, checked at the end
    vector<Stmt *> switches;                  // the enclosing switch statements
};

// ---- Dumps, for the fixtures and for looking at what the parser made.

inline string DumpInit(const Init *in);

inline string DumpExpr(const Expr *e) {
    if (!e) return "?";
    switch (e->kind) {
        case E_INT:
            return e->type->uns ? to_string(e->ival) : to_string((int64_t)e->ival);
        case E_FLOAT: {
            char buf[64];
            snprintf(buf, sizeof buf, "%.17g", e->fval);
            return buf;
        }
        case E_STR: {
            string s = "\"";
            for (size_t i = 0; i + 1 < e->sval.size(); i++) {
                auto c = e->sval[i];
                if (c == '"' || c == '\\') { s += '\\'; s += c; }
                else if (c == '\n') s += "\\n";
                else if ((uint8_t)c < 32 || (uint8_t)c >= 127) {
                    char buf[8];
                    snprintf(buf, sizeof buf, "\\x%02x", (uint8_t)c);
                    s += buf;
                } else s += c;
            }
            return s + "\"";
        }
        case E_VAR: return e->decl->name;
        case E_FIELD: return cat("(. ", DumpExpr(e->a), " ", e->field->name, ")");
        case E_DEREF: return cat("(* ", DumpExpr(e->a), ")");
        case E_ADDR: return cat("(& ", DumpExpr(e->a), ")");
        case E_DECAY: return cat("(decay ", DumpExpr(e->a), ")");
        case E_UNARY: return cat("(", e->op, " ", DumpExpr(e->a), ")");
        case E_BINARY: return cat("(", e->op, " ", DumpExpr(e->a), " ", DumpExpr(e->b), ")");
        case E_ASSIGN: return cat("(", e->op, " ", DumpExpr(e->a), " ", DumpExpr(e->b), ")");
        case E_INCDEC: return cat("(", e->post ? "post" : "pre", e->op, " ", DumpExpr(e->a), ")");
        case E_COND: return cat("(?: ", DumpExpr(e->a), " ", DumpExpr(e->b), " ", DumpExpr(e->c), ")");
        case E_CAST: return cat("(cast ", TypeName(e->type), " ", DumpExpr(e->a), ")");
        case E_CALL: {
            string s = cat("(call ", DumpExpr(e->a));
            for (auto a : e->args) Append(s, " ", DumpExpr(a));
            return s + ")";
        }
        case E_COMMA: return cat("(, ", DumpExpr(e->a), " ", DumpExpr(e->b), ")");
        case E_COMPOUND: return cat("(compound ", TypeName(e->type), " ", DumpInit(e->init), ")");
    }
    return "?";
}

inline string DumpInit(const Init *in) {
    if (!in) return "_";
    if (!in->IsList()) return DumpExpr(in->expr);
    string s = in->ufield ? cat("{.", in->ufield, "=") : "{";
    for (size_t i = 0; i < in->items.size(); i++) Append(s, i ? ", " : "", DumpInit(in->items[i]));
    return s + "}";
}

inline string DumpUnit(const CUnit &unit, const TypeTable &types) {
    string s;
    for (auto &st : types.structs) {
        Append(s, st->isunion ? "union " : "struct ", st->tag.empty() ? "<anon>" : st->tag);
        if (!st->complete) { s += " (incomplete)\n"; continue; }
        Append(s, " (size ", st->size, ", align ", st->align, st->pack ? cat(", pack ", st->pack) : "", ")\n");
        for (auto &f : st->fields) Append(s, "    ", f.name, ": ", TypeName(f.type), " @", f.offset, "\n");
    }
    for (auto d : unit.typedefs) Append(s, "typedef ", d->name, " = ", TypeName(d->type), "\n");
    for (auto &[name, v] : unit.enumconsts) Append(s, "enum ", name, " = ", v, "\n");
    for (auto d : unit.decls) {
        Append(s, d->kind == D_FUNC ? "fn " : "var ", d->name, ": ", TypeName(d->type));
        if (d->isstatic) s += " static";
        if (d->isextern) s += " extern";
        if (d->istls) s += " tls";
        if (d->kind == D_FUNC && d->defined) {
            s += " defined(";
            for (size_t i = 0; i < d->paramnames.size(); i++) Append(s, i ? ", " : "", d->paramnames[i]);
            Append(s, ") body ", d->bodyend - d->bodybegin, " tokens");
        }
        if (d->init) Append(s, " = ", DumpInit(d->init));
        s += '\n';
    }
    return s;
}

}  // namespace interp
}  // namespace goose
