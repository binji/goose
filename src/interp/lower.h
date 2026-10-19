// Goose compiler — lowering the interpreter's C syntax tree to bytecode
// (docs/design/interpreter.md §4, step 3 of §14). Every function body
// becomes a Function, every file-scope object a StaticObj, and the static
// initializers one function that runs before main. The tree is fully typed,
// so this is a walk that picks operand types off the nodes: a scalar
// expression leaves its value on the stack, a record-typed one leaves the
// address of storage holding it, and an lvalue's address is pushed by Addr.
#pragma once

namespace goose {
namespace interp {

struct Lowerer {
    Lowerer(const CUnit &unit, const vector<string> &files) : unit(unit), files(files) {}

    Program Run() {
        prog.files = files;
        // Indexes first, so bodies can refer to anything.
        for (auto d : unit.decls) {
            if (d->kind == D_FUNC) {
                funcidx[d] = (int)prog.funcs.size();
                prog.funcs.push_back(Declare(d));
            } else if (d->kind == D_VAR) {
                staticidx[d] = (int)prog.statics.size();
                StaticObj o;
                o.name = d->name;
                o.size = d->type->IsComplete() ? d->type->size : 0;
                o.decl = d;
                o.isextern = d->isextern && !d->defined;
                prog.statics.push_back(o);
            }
        }
        // The initializer function, then every body.
        {
            Function f;
            f.name = "<init>";
            prog.init = (int)prog.funcs.size();
            prog.funcs.push_back(f);
            Begin(&prog.funcs.back());
            for (auto d : unit.decls)
                if (d->kind == D_VAR && d->init) {
                    curline = d->line;
                    InitObject(d);
                }
            Emit(OP_RET);
            End();
        }
        for (auto d : unit.decls)
            if (d->kind == D_FUNC && d->defined) Body(d);
        if (auto m = prog.Find("main")) prog.main = (int)(m - prog.funcs.data());
        return std::move(prog);
    }

private:
    const CUnit &unit;
    const vector<string> &files;
    Program prog;
    unordered_map<const CDecl *, int> funcidx, staticidx;
    unordered_map<string, int> strings;

    // The function being lowered.
    Function *fn = nullptr;
    int curline = 0;
    vector<int64_t> labels;                  // label id -> pc, or -1 while unplaced
    int64_t lastplaced = -1;                 // the pc the last label was placed at
    vector<int> breaks, continues;           // the enclosing loop or switch exits
    unordered_map<string, int> namedlabels;
    unordered_map<const Stmt *, int> caselabels;
    int64_t frametop = 0;
    int depth = 0, maxdepth = 0;             // the operand stack's height, followed in emission order

    [[noreturn]] void Error(int line, string_view msg) const {
        throw CompileError { cat(files[fn && fn->decl ? fn->decl->file : 0], ":", line, ": error: ", msg) };
    }

    Function Declare(CDecl *d) {
        Function f;
        f.name = d->name;
        f.decl = d;
        f.external = !d->defined;
        f.variadic = d->type->variadic || d->type->unspecified;
        auto rt = d->type->elem;
        if (rt->IsRecord()) f.sret = true;
        else f.ret = VTOf(rt);
        return f;
    }

    // ---- Emission

    void Emit(Op op, VT t = VT_VOID, int64_t imm = 0, uint8_t t2 = 0) {
        Insn in;
        in.op = op;
        in.t = t;
        in.t2 = t2;
        in.imm = imm;
        in.line = curline;
        fn->code.push_back(in);
        depth += Effect(in);
        maxdepth = max(maxdepth, depth);
    }

    // The operand stack's height, followed in emission order (StackEffect,
    // bytecode.h), bounds the height the code reaches: a jump's target is
    // never deeper than the code before the jump, since statements leave
    // the stack where they found it and the expressions that branch (?:,
    // &&, ||) join at equal heights.
    static int Effect(const Insn &in) { return StackEffect(in); }
    int NewLabel() {
        labels.push_back(-1);
        return (int)labels.size() - 1;
    }
    void Place(int label) { labels[(size_t)label] = lastplaced = (int64_t)fn->code.size(); }

    // A jump; on a comparison just emitted that nothing jumps to, the two
    // fuse into one compare-and-jump, the jump's sense folded into the
    // comparison.
    void Jump(Op op, int label) {
        auto &code = fn->code;
        if ((op == OP_JZ || op == OP_JNZ) && !code.empty() && lastplaced != (int64_t)code.size()) {
            auto &last = code.back();
            if (last.op >= OP_EQ && last.op <= OP_GE) {
                static const Op inverse[] = { OP_NE, OP_EQ, OP_GE, OP_GT, OP_LE, OP_LT };
                auto cmp = op == OP_JNZ ? last.op : inverse[last.op - OP_EQ];
                // Comparing floats, `!(a < b)` is not `a >= b` when one is a NaN.
                if (!IsFloat(last.t) || op == OP_JNZ) {
                    last.op = OP_JCMP;
                    last.t2 = (uint8_t)cmp;
                    last.imm = label;
                    depth -= 1;   // the jump's pop; the comparison's push is undone
                    return;
                }
            }
        }
        Emit(op, VT_VOID, label);
    }

    void Begin(Function *f) {
        fn = f;
        depth = maxdepth = 0;
        lastplaced = -1;
        labels.clear();
        breaks.clear();
        continues.clear();
        namedlabels.clear();
        caselabels.clear();
        frametop = 0;
    }

    // Resolves the jumps and sizes the frame.
    void End() {
        for (auto &in : fn->code)
            if (in.op == OP_JMP || in.op == OP_JZ || in.op == OP_JNZ || in.op == OP_JCMP) {
                auto target = labels[(size_t)in.imm];
                assert(target >= 0);
                in.imm = target;
            }
        fn->framesize = RoundUp(frametop, 8);
        // A slot or two of slack for what a native call pushes.
        fn->maxstack = maxdepth + 2;
        fn = nullptr;
    }

    int64_t Alloc(int64_t size, int align) {
        frametop = RoundUp(frametop, max(align, 1));
        auto off = frametop;
        frametop += max<int64_t>(size, 1);
        fn->locals.push_back(FrameObj { off, size });
        return off;
    }

    // ---- Functions

    void Body(CDecl *d) {
        auto f = &prog.funcs[(size_t)funcidx[d]];
        Begin(f);
        curline = d->line;
        if (f->sret) {
            Param p;
            p.off = Alloc(8, 8);
            p.t = VT_U64;
            f->params.push_back(p);
        }
        for (auto pd : d->params) {
            pd->frameoff = Alloc(pd->type->size, pd->type->align);
            Param p;
            p.off = pd->frameoff;
            p.t = VTOf(pd->type);
            if (pd->type->IsRecord()) p.size = pd->type->size;
            f->params.push_back(p);
        }
        for (auto l : d->locals)
            if (find(d->params.begin(), d->params.end(), l) == d->params.end())
                l->frameoff = Alloc(l->type->size, l->type->align);
        Statement(d->body);
        // Falling off the end: a void return, or an unspecified value.
        curline = d->line;
        if (f->ret != VT_VOID) Emit(OP_PUSH, VT_VOID, 0);
        Emit(OP_RET, f->ret);
        End();
    }

    // ---- Statements

    void Statement(Stmt *s) {
        curline = s->line;
        switch (s->kind) {
            case S_EMPTY: break;
            case S_EXPR: Value(s->expr, false); break;
            case S_DECL:
                for (auto d : s->decls) InitObject(d);
                break;
            case S_BLOCK:
                for (auto item : s->items) Statement(item);
                break;
            case S_IF: {
                int lelse = NewLabel(), lend = s->b ? NewLabel() : lelse;
                Branch(s->expr, OP_JZ, lelse);
                Statement(s->a);
                if (s->b) {
                    Jump(OP_JMP, lend);
                    Place(lelse);
                    Statement(s->b);
                }
                Place(lend);
                break;
            }
            case S_WHILE: {
                int ltop = NewLabel(), lend = NewLabel();
                Place(ltop);
                Branch(s->expr, OP_JZ, lend);
                Loop(s->a, lend, ltop);
                Jump(OP_JMP, ltop);
                Place(lend);
                break;
            }
            case S_DO: {
                int ltop = NewLabel(), lcond = NewLabel(), lend = NewLabel();
                Place(ltop);
                Loop(s->a, lend, lcond);
                Place(lcond);
                curline = s->line;
                Branch(s->expr, OP_JNZ, ltop);
                Place(lend);
                break;
            }
            case S_FOR: {
                int ltop = NewLabel(), lstep = NewLabel(), lend = NewLabel();
                Statement(s->init);
                Place(ltop);
                if (s->expr) Branch(s->expr, OP_JZ, lend);
                Loop(s->a, lend, lstep);
                Place(lstep);
                curline = s->line;
                if (s->step) Value(s->step, false);
                Jump(OP_JMP, ltop);
                Place(lend);
                break;
            }
            case S_SWITCH: {
                // The value in a frame slot, so the stack is clean at every
                // case label; then a chain of comparisons to the labels.
                auto t = VTOf(s->expr->type);
                auto slot = Alloc(8, 8);
                int lend = NewLabel(), ldefault = lend;
                Emit(OP_LOCAL, VT_VOID, slot);
                Value(s->expr);
                Emit(OP_STORE, t);
                for (auto c : s->cases) {
                    int l = NewLabel();
                    caselabels[c] = l;
                    if (c->kind == S_DEFAULT) { ldefault = l; continue; }
                    Emit(OP_LOCAL, VT_VOID, slot);
                    Emit(OP_LOAD, t);
                    Emit(OP_PUSH, VT_VOID, c->value);
                    Emit(OP_EQ, t);
                    Jump(OP_JNZ, l);
                }
                Jump(OP_JMP, ldefault);
                breaks.push_back(lend);
                Statement(s->a);
                breaks.pop_back();
                Place(lend);
                break;
            }
            case S_CASE:
            case S_DEFAULT:
                Place(caselabels.at(s));
                Statement(s->a);
                break;
            case S_BREAK:
                if (breaks.empty()) Error(s->line, "break outside a loop or switch");
                Jump(OP_JMP, breaks.back());
                break;
            case S_CONTINUE:
                if (continues.empty()) Error(s->line, "continue outside a loop");
                Jump(OP_JMP, continues.back());
                break;
            case S_GOTO: Jump(OP_JMP, NamedLabel(s->label)); break;
            case S_LABEL:
                Place(NamedLabel(s->label));
                Statement(s->a);
                break;
            case S_RETURN:
                if (fn->sret) {
                    Emit(OP_LOCAL, VT_VOID, fn->params[0].off);
                    Emit(OP_LOAD, VT_U64);
                    Value(s->expr);
                    Emit(OP_COPY, VT_VOID, s->expr->type->size);
                    Emit(OP_RET);
                } else if (s->expr) {
                    Value(s->expr);
                    Emit(OP_RET, fn->ret);
                } else {
                    Emit(OP_RET);
                }
                break;
        }
    }

    void Loop(Stmt *body, int lbreak, int lcontinue) {
        breaks.push_back(lbreak);
        continues.push_back(lcontinue);
        Statement(body);
        breaks.pop_back();
        continues.pop_back();
    }

    int NamedLabel(const string &name) {
        auto it = namedlabels.find(name);
        if (it != namedlabels.end()) return it->second;
        return namedlabels[name] = NewLabel();
    }

    // A conditional jump on a scalar: a float compares against zero first,
    // since a negative zero's bits are not zero.
    void Branch(Expr *e, Op op, int label) {
        // A condition's integer casts of a 0-or-1 value change nothing, and
        // `!x` is the other jump on x: the `if (!(a <= b))` codegen writes
        // for a loop's exit becomes a compare and one jump.
        while (e->kind == E_CAST && e->type->IsInteger() && IsBoolean(e->a)) e = e->a;
        if (e->kind == E_UNARY && e->op == "!") {
            Branch(e->a, op == OP_JZ ? OP_JNZ : OP_JZ, label);
            return;
        }
        Value(e);
        if (IsFloat(VTOf(e->type))) Emit(OP_TOBOOL, VTOf(e->type));
        Jump(op, label);
    }

    // An expression whose value is 0 or 1: a comparison, a logical operator,
    // or an integer cast of one.
    static bool IsBoolean(const Expr *e) {
        if (e->kind == E_UNARY) return e->op == "!";
        if (e->kind == E_BINARY)
            return e->op == "==" || e->op == "!=" || e->op == "<" || e->op == "<=" || e->op == ">" ||
                   e->op == ">=" || e->op == "&&" || e->op == "||";
        if (e->kind == E_CAST) return e->type->IsInteger() && IsBoolean(e->a);
        return false;
    }

    // ---- Objects and initializers

    void ObjAddr(CDecl *d) {
        if (d->islocal) Emit(OP_LOCAL, VT_VOID, d->frameoff);
        else Emit(OP_GLOBAL, VT_VOID, staticidx.at(d));
    }

    // Initializes an object from its initializer: a brace list zero-fills
    // first, then every written item is stored.
    void InitObject(CDecl *d) {
        if (!d->init) return;
        if (d->islocal && !d->init->IsList() && d->type->IsScalar()) {
            curline = d->init->expr->line;
            Value(d->init->expr);
            Emit(OP_LSTORE, VTOf(d->type), d->frameoff);
            return;
        }
        auto base = [&] { ObjAddr(d); };
        if (d->init->IsList()) {
            base();
            Emit(OP_ZERO, VT_VOID, d->type->size);
        }
        StoreInit(base, 0, d->init, d->type);
    }

    void StoreInit(const function<void()> &base, int64_t off, Init *in, CType *t) {
        if (!in) return;
        if (!in->IsList()) {
            auto e = in->expr;
            curline = e->line;
            base();
            if (off) Emit(OP_OFFSET, VT_VOID, off);
            if (t->kind == TK_ARRAY) {
                // A string into a char array, the NUL left out where it does not fit.
                assert(e->kind == E_STR);
                Emit(OP_GLOBAL, VT_VOID, Intern(e->sval));
                Emit(OP_COPY, VT_VOID, min<int64_t>(t->size, (int64_t)e->sval.size()));
            } else if (t->IsRecord()) {
                Value(e);
                Emit(OP_COPY, VT_VOID, t->size);
            } else {
                Value(e);
                Emit(OP_STORE, VTOf(t));
            }
            return;
        }
        if (t->kind == TK_ARRAY) {
            for (size_t i = 0; i < in->items.size(); i++)
                StoreInit(base, off + (int64_t)i * t->elem->size, in->items[i], t->elem);
        } else if (t->kind == TK_UNION) {
            auto &f = t->rec->fields[(size_t)in->ufield];
            if (!in->items.empty()) StoreInit(base, off, in->items[0], f.type);
        } else {
            auto &fs = t->rec->fields;
            for (size_t i = 0; i < in->items.size() && i < fs.size(); i++)
                StoreInit(base, off + fs[i].offset, in->items[i], fs[i].type);
        }
    }

    // A string literal's static object, one per distinct string.
    int Intern(const string &s) {
        auto it = strings.find(s);
        if (it != strings.end()) return it->second;
        StaticObj o;
        o.name = "<string>";
        o.size = (int64_t)s.size();
        o.bytes = s;
        prog.statics.push_back(o);
        return strings[s] = (int)prog.statics.size() - 1;
    }

    // ---- Expressions

    // Pushes the address of an lvalue, of a function, or of the storage a
    // record-typed expression's value is in.
    void Addr(Expr *e) {
        curline = e->line;
        switch (e->kind) {
            case E_VAR:
                if (e->decl->kind == D_FUNC) Emit(OP_FUNC, VT_VOID, funcidx.at(e->decl));
                else ObjAddr(e->decl);
                return;
            case E_STR: Emit(OP_GLOBAL, VT_VOID, Intern(e->sval)); return;
            case E_FIELD:
                Addr(e->a);
                if (e->field->offset) Emit(OP_OFFSET, VT_VOID, e->field->offset);
                return;
            case E_DEREF: Value(e->a); return;
            case E_COMPOUND:
                InitObject(e->decl);
                ObjAddr(e->decl);
                return;
            case E_CALL:
            case E_COND:
            case E_COMMA:
            case E_ASSIGN:
                if (e->type->IsRecord()) { Value(e); return; }
                break;
            default: break;
        }
        Error(e->line, "internal: no address for this expression");
    }

    // Pushes the value, or nothing if it is not wanted (the expression's
    // effects still happen). A record's value is the address of its storage.
    void Value(Expr *e, bool want = true) {
        curline = e->line;
        switch (e->kind) {
            case E_INT:
                if (want) Emit(OP_PUSH, VT_VOID, (int64_t)e->ival);
                return;
            case E_FLOAT:
                if (want) Emit(OP_PUSH, VT_VOID, (int64_t)bit_cast<uint64_t>(e->fval));
                return;
            case E_STR:
                if (want) Addr(e);
                return;
            case E_VAR:
                if (!want) return;
                if (IsSlotLocal(e)) { Emit(OP_LLOAD, VTOf(e->type), SlotOf(e)); return; }
                Addr(e);
                Load(e->type);
                return;
            case E_FIELD:
                if (!want) { Value(e->a, false); return; }
                Addr(e);
                Load(e->type);
                return;
            case E_DEREF:
                if (!want) { Value(e->a, false); return; }
                Value(e->a);
                Load(e->type);
                return;
            case E_ADDR:
            case E_DECAY:
                if (want) Addr(e->a);
                return;
            case E_UNARY: {
                if (!want) { Value(e->a, false); return; }
                Value(e->a);
                auto t = VTOf(e->a->type);
                if (e->op == "-") Emit(OP_NEG, t);
                else if (e->op == "~") Emit(OP_NOT, t);
                else if (e->op == "!") Emit(OP_LNOT, t);
                return;
            }
            case E_BINARY: Binary(e, want); return;
            case E_ASSIGN: Assign(e, want); return;
            case E_INCDEC: {
                bool ptr = e->type->kind == TK_PTR;
                int64_t delta = ptr ? e->type->elem->size : 1;
                if (e->op == "--") delta = -delta;
                auto t = VTOf(e->type);
                if (IsSlotLocal(e->a)) {
                    // On a local: load, add, store, with the old value kept
                    // under the new one where a postfix form's is wanted.
                    auto off = SlotOf(e->a);
                    Emit(OP_LLOAD, t, off);
                    if (want && e->post) Emit(OP_DUP);
                    if (ptr) {
                        Emit(OP_PUSH, VT_VOID, 1);
                        Emit(OP_PTRADD, VT_VOID, delta);
                    } else {
                        Emit(OP_PUSH, VT_VOID, IsFloat(t) ? (int64_t)bit_cast<uint64_t>((double)delta) : delta);
                        Emit(OP_ADD, t);
                    }
                    Emit(want && !e->post ? OP_LSTOREK : OP_LSTORE, t, off);
                    return;
                }
                // t2: bit 0 post, bit 1 pointer (checked arithmetic).
                Addr(e->a);
                Emit(OP_INCDEC, t, delta, (uint8_t)((e->post ? 1 : 0) | (ptr ? 2 : 0)));
                if (!want) Emit(OP_POP);
                return;
            }
            case E_COND: {
                int lelse = NewLabel(), lend = NewLabel();
                Branch(e->a, OP_JZ, lelse);
                Value(e->b, want);
                Jump(OP_JMP, lend);
                Place(lelse);
                Value(e->c, want);
                Place(lend);
                return;
            }
            case E_CAST: {
                if (e->type->kind == TK_VOID) { Value(e->a, false); return; }
                Value(e->a, want);
                if (want) Convert(e->a->type, e->type);
                return;
            }
            case E_CALL: Call(e, want); return;
            case E_COMMA:
                Value(e->a, false);
                Value(e->b, want);
                return;
            case E_COMPOUND:
                InitObject(e->decl);
                if (!want) return;
                ObjAddr(e->decl);
                Load(e->type);
                return;
        }
    }

    // The value at the address on the stack, if the type has slot values;
    // a record or array stays as its address.
    void Load(CType *t) {
        if (t->IsScalar()) Emit(OP_LOAD, VTOf(t));
    }

    void Convert(CType *from, CType *to) {
        auto f = VTOf(from), t = VTOf(to);
        if (f == t) return;
        // The 64-bit integers and pointers share one slot form.
        if ((f == VT_I64 || f == VT_U64) && (t == VT_I64 || t == VT_U64)) return;
        Emit(OP_CVT, f, 0, t);
    }

    void Truth(Expr *e) {
        Value(e);
        Emit(OP_TOBOOL, VTOf(e->type));
    }

    void Binary(Expr *e, bool want) {
        auto &o = e->op;
        if (o == "&&" || o == "||") {
            int lend = NewLabel();
            Truth(e->a);
            Emit(OP_DUP);
            Jump(o == "&&" ? OP_JZ : OP_JNZ, lend);
            Emit(OP_POP);
            Truth(e->b);
            Place(lend);
            if (!want) Emit(OP_POP);
            return;
        }
        if (!want) {
            Value(e->a, false);
            Value(e->b, false);
            return;
        }
        auto at = e->a->type, bt = e->b->type;
        // `i * 1` and `p + 0`, which codegen writes for byte-sized elements
        // and a first field, are their left operand.
        if (e->b->kind == E_INT && at->IsInteger() && bt->IsInteger() &&
            ((o == "*" && e->b->ival == 1) || ((o == "+" || o == "-") && e->b->ival == 0))) {
            Value(e->a);
            return;
        }
        Value(e->a);
        Value(e->b);
        curline = e->line;
        if (o == "-" && at->kind == TK_PTR && bt->kind == TK_PTR) {
            Emit(OP_PTRDIFF, VT_VOID, at->elem->size);
            return;
        }
        if ((o == "+" || o == "-") && at->kind == TK_PTR) {
            Emit(o == "+" ? OP_PTRADD : OP_PTRSUB, VT_VOID, at->elem->size);
            return;
        }
        static const unordered_map<string, Op> ops = {
            { "+", OP_ADD }, { "-", OP_SUB }, { "*", OP_MUL }, { "/", OP_DIV }, { "%", OP_MOD },
            { "&", OP_AND }, { "|", OP_OR }, { "^", OP_XOR }, { "<<", OP_SHL }, { ">>", OP_SHR },
            { "==", OP_EQ }, { "!=", OP_NE }, { "<", OP_LT }, { "<=", OP_LE }, { ">", OP_GT }, { ">=", OP_GE },
        };
        auto it = ops.find(o);
        if (it == ops.end()) Error(e->line, cat("internal: operator ", o));
        // Comparisons work in the operands' type (the parser made them one);
        // the rest in the result's, which is the operands' too.
        Emit(it->second, VTOf(at));
    }

    // A scalar at a fixed place in the frame, which the fused local
    // instructions reach without an address: a local or parameter, or a
    // field of a local record, however deep.
    static bool IsSlotLocal(const Expr *e) {
        int64_t off;
        return e->type->IsScalar() && FrameSlot(e, off);
    }
    static bool FrameSlot(const Expr *e, int64_t &off) {
        if (e->kind == E_VAR) {
            if (e->decl->kind != D_VAR || !e->decl->islocal) return false;
            off = e->decl->frameoff;
            return true;
        }
        if (e->kind == E_FIELD && FrameSlot(e->a, off)) {
            off += e->field->offset;
            return true;
        }
        return false;
    }
    static int64_t SlotOf(const Expr *e) {
        int64_t off = 0;
        FrameSlot(e, off);
        return off;
    }

    void Assign(Expr *e, bool want) {
        auto lt = e->a->type;
        bool local = IsSlotLocal(e->a);
        if (e->op == "=") {
            if (local) {
                Value(e->b);
                Emit(want ? OP_LSTOREK : OP_LSTORE, VTOf(lt), SlotOf(e->a));
                return;
            }
            Addr(e->a);
            if (lt->IsRecord()) {
                if (want) Emit(OP_DUP);
                Value(e->b);
                Emit(OP_COPY, VT_VOID, lt->size);
            } else {
                Value(e->b);
                Emit(want ? OP_STOREK : OP_STORE, VTOf(lt));
            }
            return;
        }
        // a op= b: load a, do the operation in optype, convert back, store.
        auto o = e->op.substr(0, e->op.size() - 1);
        auto ot = e->optype;
        if (local) {
            Emit(OP_LLOAD, VTOf(lt), SlotOf(e->a));
        } else {
            Addr(e->a);
            Emit(OP_DUP);
            Emit(OP_LOAD, VTOf(lt));
        }
        if (ot->kind == TK_PTR) {
            Value(e->b);
            Emit(o == "+" ? OP_PTRADD : OP_PTRSUB, VT_VOID, ot->elem->size);
        } else {
            Convert(lt, ot);
            Value(e->b);
            static const unordered_map<string, Op> ops = {
                { "+", OP_ADD }, { "-", OP_SUB }, { "*", OP_MUL }, { "/", OP_DIV }, { "%", OP_MOD },
                { "&", OP_AND }, { "|", OP_OR }, { "^", OP_XOR }, { "<<", OP_SHL }, { ">>", OP_SHR },
            };
            curline = e->line;
            Emit(ops.at(o), VTOf(ot));
            Convert(ot, lt);
        }
        if (local) Emit(want ? OP_LSTOREK : OP_LSTORE, VTOf(lt), SlotOf(e->a));
        else Emit(want ? OP_STOREK : OP_STORE, VTOf(lt));
    }

    void Call(Expr *e, bool want) {
        auto callee = e->a;
        auto ft = callee->type->elem;
        const CDecl *direct = nullptr;
        if (callee->kind == E_DECAY && callee->a->kind == E_VAR && callee->a->decl->kind == D_FUNC)
            direct = callee->a->decl;
        else Value(callee);
        int nargs = 0;
        int64_t sret = -1;
        if (e->type->IsRecord()) {
            sret = Alloc(e->type->size, e->type->align);
            Emit(OP_LOCAL, VT_VOID, sret);
            nargs++;
        }
        for (auto a : e->args) {
            Value(a);
            nargs++;
        }
        if (nargs > 255) Error(e->line, "too many arguments");
        curline = e->line;
        // The result type on the stack: none for void or a record (sret).
        auto rt = ft->elem->IsScalar() ? VTOf(ft->elem) : VT_VOID;
        if (direct) Emit(OP_CALL, rt, funcidx.at(direct), (uint8_t)nargs);
        else Emit(OP_CALLI, rt, 0, (uint8_t)nargs);
        if (sret >= 0) {
            if (want) Emit(OP_LOCAL, VT_VOID, sret);
        } else if (ft->elem->kind != TK_VOID && !want) {
            Emit(OP_POP);
        }
    }
};

}  // namespace interp
}  // namespace goose
