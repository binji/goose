// Goose compiler — the interpreter's C types and layouts (docs/design/
// interpreter.md §4, step 2 of §14). Types are structural and interned: two
// spellings of one type are one CType, so comparing types is comparing
// pointers, and a struct or union is its CStruct. Qualifiers are dropped.
// The target is the one the C backend assumes (implementation.md §6.1):
// 64-bit, little-endian, LP64, plain char signed, and packed structs where
// `#pragma pack` says so, which the generated layouts rely on being honored
// exactly.
#pragma once

namespace goose {
namespace interp {

enum CTypeKind {
    TK_VOID,
    TK_BOOL,     // _Bool: one byte, 0 or 1
    TK_INT,      // every integer type, by size and signedness; enums are int
    TK_FLOAT,    // float (4) and double (8)
    TK_PTR,
    TK_ARRAY,    // count < 0: incomplete, `T x[]`
    TK_STRUCT,
    TK_UNION,
    TK_FUNC,
};

struct CStruct;

struct CType {
    CTypeKind kind = TK_VOID;
    int64_t size = 0;
    int align = 1;
    bool uns = false;            // TK_INT: unsigned
    bool ischar = false;         // TK_INT of size 1 spelled char or signed/unsigned char
    CType *elem = nullptr;       // pointer target, array element, function result
    int64_t count = -1;          // TK_ARRAY
    CStruct *rec = nullptr;      // TK_STRUCT, TK_UNION
    vector<CType *> params;      // TK_FUNC
    bool variadic = false;       // TK_FUNC: `, ...`
    bool unspecified = false;    // TK_FUNC: `f()`, no prototype

    bool IsInteger() const { return kind == TK_INT || kind == TK_BOOL; }
    bool IsArith() const { return IsInteger() || kind == TK_FLOAT; }
    bool IsScalar() const { return IsArith() || kind == TK_PTR; }
    bool IsRecord() const { return kind == TK_STRUCT || kind == TK_UNION; }
    bool IsComplete() const {
        return kind != TK_VOID && kind != TK_FUNC && !(kind == TK_ARRAY && count < 0) &&
               !(IsRecord() && size < 0);
    }
};

struct CField {
    string name;
    CType *type = nullptr;
    int64_t offset = 0;
    int line = 0;
};

struct CStruct {
    string tag;                  // "" for an anonymous one
    bool isunion = false;
    bool complete = false;
    int pack = 0;                // the #pragma pack in force at the definition; 0: none
    vector<CField> fields;
    int64_t size = -1;
    int align = 1;
    int file = 0, line = 0;

    const CField *Find(string_view name) const {
        for (auto &f : fields) if (f.name == name) return &f;
        return nullptr;
    }
};

inline int64_t RoundUp(int64_t v, int64_t a) { return (v + a - 1) / a * a; }

// Owns every type. The basic types are members; derived types are interned
// by what they are made of.
struct TypeTable {
    CType tvoid, tbool, tchar, tschar, tuchar, tshort, tushort, tint, tuint, tlong, tulong,
          tllong, tullong, tfloat, tdouble;

    TypeTable() {
        tvoid.kind = TK_VOID;
        tbool.kind = TK_BOOL; tbool.size = 1; tbool.align = 1;
        auto in = [](CType &t, int64_t size, bool uns, bool ischar = false) {
            t.kind = TK_INT; t.size = size; t.align = (int)size; t.uns = uns; t.ischar = ischar;
        };
        in(tchar, 1, false, true); in(tschar, 1, false, true); in(tuchar, 1, true, true);
        in(tshort, 2, false); in(tushort, 2, true);
        in(tint, 4, false); in(tuint, 4, true);
        in(tlong, 8, false); in(tulong, 8, true);
        in(tllong, 8, false); in(tullong, 8, true);
        tfloat.kind = TK_FLOAT; tfloat.size = 4; tfloat.align = 4;
        tdouble.kind = TK_FLOAT; tdouble.size = 8; tdouble.align = 8;
    }

    // The integer type of a size and signedness, as the standard integer
    // types the arithmetic rules name. `char` and `signed char` are both
    // signed 8-bit integers; they differ only in spelling.
    CType *Int(int64_t size, bool uns) {
        switch (size) {
            case 1: return uns ? &tuchar : &tschar;
            case 2: return uns ? &tushort : &tshort;
            case 4: return uns ? &tuint : &tint;
            default: return uns ? &tulong : &tlong;
        }
    }

    CType *Pointer(CType *to) {
        auto &p = pointers[to];
        if (!p) {
            p = New();
            p->kind = TK_PTR; p->size = 8; p->align = 8; p->elem = to;
        }
        return p;
    }

    CType *Array(CType *of, int64_t count) {
        auto &a = arrays[{ of, count }];
        if (!a) {
            a = New();
            a->kind = TK_ARRAY; a->elem = of; a->count = count;
            a->size = count < 0 ? -1 : of->size * count;
            a->align = of->align;
        }
        return a;
    }

    CType *Func(CType *ret, const vector<CType *> &params, bool variadic, bool unspecified) {
        string key = cat((uintptr_t)ret, variadic ? "v" : "", unspecified ? "u" : "");
        for (auto p : params) Append(key, ",", (uintptr_t)p);
        auto &f = funcs[key];
        if (!f) {
            f = New();
            f->kind = TK_FUNC; f->elem = ret; f->params = params;
            f->variadic = variadic; f->unspecified = unspecified;
        }
        return f;
    }

    CType *Record(CStruct *rec) {
        auto &t = records[rec];
        if (!t) {
            t = New();
            t->kind = rec->isunion ? TK_UNION : TK_STRUCT; t->rec = rec;
            t->size = rec->size; t->align = rec->align;
        }
        return t;
    }

    CStruct *NewStruct(string tag, bool isunion) {
        structs.push_back(make_unique<CStruct>());
        auto s = structs.back().get();
        s->tag = std::move(tag);
        s->isunion = isunion;
        return s;
    }

    // Lays out a record under the pack in force: a member is aligned to its
    // natural alignment or the pack, whichever is less, and the record to the
    // greatest of those. The record's type, if made already, follows.
    void Layout(CStruct &s, int pack) {
        s.pack = pack;
        int64_t off = 0;
        int align = 1;
        for (auto &f : s.fields) {
            int a = f.type->align;
            if (pack && a > pack) a = pack;
            if (s.isunion) {
                f.offset = 0;
                off = max(off, f.type->size);
            } else {
                off = RoundUp(off, a);
                f.offset = off;
                off += f.type->size;
            }
            align = max(align, a);
        }
        s.align = align;
        s.size = RoundUp(off, align);
        s.complete = true;
        if (auto it = records.find(&s); it != records.end()) {
            it->second->size = s.size;
            it->second->align = s.align;
        }
    }

    // After a record completes, the arrays of it made earlier get their size.
    void CompleteArraysOf(CType *t) {
        for (auto &[key, a] : arrays)
            if (key.first == t && a->count >= 0) { a->size = t->size * a->count; a->align = t->align; }
    }

    vector<unique_ptr<CStruct>> structs;

private:
    vector<unique_ptr<CType>> owned;
    unordered_map<CType *, CType *> pointers;
    map<pair<CType *, int64_t>, CType *> arrays;
    unordered_map<string, CType *> funcs;
    unordered_map<CStruct *, CType *> records;

    CType *New() {
        owned.push_back(make_unique<CType>());
        return owned.back().get();
    }
};

// A type's spelling for dumps and diagnostics, prefix style so nesting reads
// left to right: *uint8_t, [4]int32_t, fn(int64_t, *uint8_t) -> void.
inline string TypeName(const CType *t) {
    if (!t) return "?";
    switch (t->kind) {
        case TK_VOID: return "void";
        case TK_BOOL: return "_Bool";
        case TK_INT:
            if (t->ischar && !t->uns) return "char";
            return cat(t->uns ? "uint" : "int", t->size * 8, "_t");
        case TK_FLOAT: return t->size == 4 ? "float" : "double";
        case TK_PTR: return cat("*", TypeName(t->elem));
        case TK_ARRAY:
            return t->count < 0 ? cat("[]", TypeName(t->elem))
                                : cat("[", t->count, "]", TypeName(t->elem));
        case TK_STRUCT:
        case TK_UNION: {
            auto s = t->rec;
            return cat(s->isunion ? "union " : "struct ", s->tag.empty() ? "<anon>" : s->tag);
        }
        case TK_FUNC: {
            string s = "fn(";
            for (size_t i = 0; i < t->params.size(); i++)
                Append(s, i ? ", " : "", TypeName(t->params[i]));
            if (t->variadic) Append(s, t->params.empty() ? "..." : ", ...");
            if (t->unspecified) s += "?";
            return cat(s, ") -> ", TypeName(t->elem));
        }
    }
    return "?";
}

}  // namespace interp
}  // namespace goose
