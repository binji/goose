// Goose compiler — the interpreter's virtual machine (docs/design/
// interpreter.md §4 to §6, step 3 of §14). A switch loop over the bytecode
// with explicit frames, so interpreted recursion costs no host stack and
// the loop can stop and resume between instructions. Memory is segments
// (§5): a pointer is a handle, segment id above, byte offset below, decoded
// only at a load, a store or a call into compiled code, and a stack segment
// grows when an access lands past its size, which is what lets Goose's data
// stacks move without the emitted code noticing. Out-of-range accesses,
// use after free and arithmetic that leaves a segment are faults, reported
// with the interpreted call stack. The C library the program calls is a
// table of adapters over the host's library (§6); this step holds what the
// fixtures need, step 4 the rest.
#pragma once

namespace goose {
namespace interp {

// An interpreter fault: something the program did that native code would
// have crashed on, or silently corrupted memory with.
struct Fault { string msg; };

// exit() from the program.
struct Exit { int status; };

enum SegKind { SEG_STATIC, SEG_FRAME, SEG_HEAP, SEG_STACK, SEG_FUNC };

struct Segment {
    SegKind kind = SEG_STATIC;
    vector<uint8_t> mem;
    uint64_t size = 0;           // bytes accessible now
    uint64_t limit = 0;          // bytes a stack segment may grow to
    bool alive = true;
    string name;
    int func = -1;               // SEG_FUNC: the function
};

inline uint64_t Handle(uint32_t id, uint64_t off) { return ((uint64_t)id << 32) | off; }
inline uint32_t SegOf(uint64_t h) { return (uint32_t)(h >> 32); }
inline uint64_t OffOf(uint64_t h) { return (uint32_t)h; }

struct VM;
using Native = uint64_t (*)(VM &vm, const uint64_t *args, int nargs);

struct Frame {
    const Function *fn;
    size_t pc;
    uint64_t fp;                 // the frame's offset in the frame segment
    size_t sp;                   // the operand stack's height on entry, after the arguments
};

struct VM {
    const Program &prog;

    // Output: what fwrite and friends write to the two streams.
    function<void(int fd, string_view data)> write = [](int fd, string_view data) {
        fwrite(data.data(), 1, data.size(), fd == 2 ? stderr : stdout);
    };
    // The frame segment's limit: the native stack size, which caps recursion.
    uint64_t framelimit = 8u << 20;

    explicit VM(const Program &prog) : prog(prog) {
        segs.emplace_back();   // id 0 is null
        frameseg = NewSeg(SEG_FRAME, "the frame segment", 0);
        segs[frameseg].limit = framelimit;
        statichandles.assign(prog.statics.size(), 0);
        funchandles.assign(prog.funcs.size(), 0);
        natives.assign(prog.funcs.size(), nullptr);
        auto out = NewSeg(SEG_STATIC, "stdout", 1), err = NewSeg(SEG_STATIC, "stderr", 1);
        stdouth = Handle(out, 0);
        stderrh = Handle(err, 0);
        Library();
    }

    // Runs the static initializers, then main with the arguments; returns
    // the exit status. A fault is reported on the error stream and exits
    // with status 134, as abort() would.
    int Run(const vector<string> &args) {
        try {
            segs[frameseg].limit = framelimit;
            if (prog.init >= 0) {
                Enter(prog.funcs[(size_t)prog.init], 0);
                Loop();
            }
            if (prog.main < 0) throw Fault { "no main function" };
            auto &m = prog.funcs[(size_t)prog.main];
            if (!m.params.empty()) {
                // argv: the strings and an array of their handles, NULL last.
                auto argv = NewSeg(SEG_STATIC, "argv", 8 * (args.size() + 1));
                for (size_t i = 0; i < args.size(); i++) {
                    auto s = NewSeg(SEG_STATIC, "an argument", args[i].size() + 1);
                    memcpy(segs[s].mem.data(), args[i].c_str(), args[i].size() + 1);
                    StoreVal(segs[argv].mem.data() + 8 * i, VT_U64, Handle(s, 0));
                }
                Push((uint64_t)args.size());
                Push(Handle(argv, 0));
                if (m.params.size() > 2) Push(0);
            }
            Enter(m, (int)m.params.size());
            Loop();
            return m.ret == VT_VOID ? 0 : (int)Pop();
        } catch (Exit &e) {
            return e.status;
        } catch (Fault &f) {
            Report(f.msg);
            return 134;
        }
    }

    // ---- Segments

    vector<Segment> segs;
    uint32_t frameseg = 0;
    uint64_t frametop = 0;
    uint64_t stdouth = 0, stderrh = 0;

    uint32_t NewSeg(SegKind kind, string name, uint64_t size) {
        if (segs.size() >= 0xffffffffu) throw Fault { "too many segments" };
        Segment s;
        s.kind = kind;
        s.name = std::move(name);
        s.size = size;
        s.limit = size;
        s.mem.resize(size);
        segs.push_back(std::move(s));
        return (uint32_t)segs.size() - 1;
    }

    string Describe(const Segment &s) const {
        switch (s.kind) {
            case SEG_STATIC: return cat("static object '", s.name, "' (", s.size, " bytes)");
            case SEG_FRAME: return "the frame segment";
            case SEG_HEAP: return cat("heap block (", s.size, " bytes)", s.alive ? "" : ", freed");
            case SEG_STACK: return cat("stack segment (", s.size, " of ", s.limit, " bytes)");
            case SEG_FUNC: return cat("function '", s.name, "'");
        }
        return "?";
    }

    Segment &Seg(uint64_t h) {
        auto id = SegOf(h);
        if (id == 0) throw Fault { cat("null pointer access (address ", OffOf(h), ")") };
        if (id >= segs.size()) throw Fault { cat("invalid pointer ", id, ":", OffOf(h)) };
        return segs[id];
    }

    // The bytes of an access: `n` bytes at the handle, which must be within
    // a live segment, or within a stack segment's limit, which grows it.
    uint8_t *Access(uint64_t h, uint64_t n) {
        auto &s = Seg(h);
        auto off = OffOf(h);
        if (!s.alive) throw Fault { cat("use after free: ", n, " bytes at offset ", off, " of a ", Describe(s)) };
        if (s.kind == SEG_FUNC) throw Fault { cat("data access to ", Describe(s)) };
        if (off + n > s.size) {
            if (s.kind == SEG_STACK && off + n <= s.limit) Grow(s, off + n);
            else if (s.kind == SEG_STACK)
                throw Fault { cat("data stack overflow: ", n, " bytes at offset ", off, " of a ", Describe(s)) };
            else
                throw Fault { cat("access of ", n, " bytes at offset ", off, " past the end of ", Describe(s)) };
        }
        return s.mem.data() + off;
    }

    // A stack segment doubles, zero-filled, up to its limit; the block may
    // move, which no handle notices.
    static void Grow(Segment &s, uint64_t needed) {
        auto size = max<uint64_t>(max(s.size * 2, needed), 4096);
        size = min(size, s.limit);
        s.mem.resize(size);
        s.size = size;
    }

    // Pointer arithmetic stays within the segment: [0, size], or a stack's
    // limit. An integer cast to a pointer (segment 0) is plain arithmetic.
    uint64_t PtrAdd(uint64_t p, int64_t delta) {
        auto id = SegOf(p);
        if (id == 0) return p + (uint64_t)delta;
        if (id >= segs.size()) throw Fault { cat("invalid pointer ", id, ":", OffOf(p)) };
        auto &s = segs[id];
        auto off = (int64_t)OffOf(p) + delta;
        auto lim = s.kind == SEG_STACK ? s.limit : s.size;
        if (off < 0 || (uint64_t)off > lim)
            throw Fault { cat("pointer arithmetic leaves ", Describe(s), ": offset ", off) };
        return Handle(id, (uint64_t)off);
    }

    int64_t PtrDiff(uint64_t a, uint64_t b, int64_t size) {
        if (SegOf(a) != SegOf(b))
            throw Fault { cat("difference of pointers into different segments (",
                              SegOf(a) ? Describe(Seg(a)) : "null", " and ",
                              SegOf(b) ? Describe(Seg(b)) : "null", ")") };
        return ((int64_t)OffOf(a) - (int64_t)OffOf(b)) / size;
    }

    // ---- Values in memory, little-endian

    static uint64_t LoadVal(const uint8_t *p, VT t) {
        if (t == VT_F32) {
            uint32_t bits = 0;
            for (int i = 0; i < 4; i++) bits |= (uint32_t)p[i] << (8 * i);
            return bit_cast<uint64_t>((double)bit_cast<float>(bits));
        }
        uint64_t v = 0;
        int n = VTSize(t);
        for (int i = 0; i < n; i++) v |= (uint64_t)p[i] << (8 * i);
        return Normalize(v, t);
    }

    static void StoreVal(uint8_t *p, VT t, uint64_t v) {
        if (t == VT_F32) {
            auto bits = bit_cast<uint32_t>((float)bit_cast<double>(v));
            for (int i = 0; i < 4; i++) p[i] = (uint8_t)(bits >> (8 * i));
            return;
        }
        int n = VTSize(t);
        for (int i = 0; i < n; i++) p[i] = (uint8_t)(v >> (8 * i));
    }

    // ---- The operand stack and frames

    vector<uint64_t> stack;
    vector<Frame> frames;
    vector<uint64_t> statichandles, funchandles;
    vector<Native> natives;

    void Push(uint64_t v) { stack.push_back(v); }
    uint64_t Pop() {
        assert(!stack.empty());
        auto v = stack.back();
        stack.pop_back();
        return v;
    }
    uint64_t &Top() { return stack.back(); }

    uint64_t StaticHandle(size_t i) {
        auto &h = statichandles[i];
        if (!h) {
            auto &o = prog.statics[i];
            if (o.isextern) {
                auto it = libobjects.find(o.name);
                if (it == libobjects.end()) throw Fault { cat("undefined variable '", o.name, "'") };
                auto id = NewSeg(SEG_STATIC, o.name, 8);
                StoreVal(segs[id].mem.data(), VT_U64, it->second);
                h = Handle(id, 0);
            } else {
                auto id = NewSeg(SEG_STATIC, o.name, (uint64_t)o.size);
                if (!o.bytes.empty()) memcpy(segs[id].mem.data(), o.bytes.data(), o.bytes.size());
                h = Handle(id, 0);
            }
        }
        return h;
    }

    uint64_t FuncHandle(size_t i) {
        auto &h = funchandles[i];
        if (!h) {
            auto id = NewSeg(SEG_FUNC, prog.funcs[i].name, 0);
            segs[id].func = (int)i;
            h = Handle(id, 0);
        }
        return h;
    }

    // Calls a function with the top `nargs` slots as its arguments: a native
    // one runs to completion here; an interpreted one gets a frame, its
    // arguments copied into the parameter slots, and runs when the loop
    // continues. Returns whether a frame was pushed.
    bool Enter(const Function &f, int nargs) {
        if (f.external) {
            auto &fp = natives[(size_t)(&f - prog.funcs.data())];
            if (!fp) {
                auto it = library.find(f.name);
                if (it == library.end()) throw Fault { cat("undefined function '", f.name, "'") };
                fp = it->second;
            }
            auto args = stack.data() + stack.size() - (size_t)nargs;
            auto r = fp(*this, args, nargs);
            stack.resize(stack.size() - (size_t)nargs);
            if (f.ret != VT_VOID) Push(Normalize(r, f.ret));
            return false;
        }
        if ((size_t)nargs != f.params.size())
            throw Fault { cat("'", f.name, "' called with ", nargs, " arguments, declared with ", f.params.size()) };
        auto &fs = segs[frameseg];
        auto fp = RoundUp((int64_t)frametop, 8);
        auto top = (uint64_t)fp + (uint64_t)f.framesize;
        if (top > fs.limit) throw Fault { "native call stack overflow" };
        if (top > fs.mem.size()) fs.mem.resize(max(top, (uint64_t)fs.mem.size() * 2));
        fs.size = top;
        auto frame = fs.mem.data() + fp;
        auto args = stack.data() + stack.size() - (size_t)nargs;
        for (size_t i = 0; i < f.params.size(); i++) {
            auto &p = f.params[i];
            if (p.size) memmove(frame + p.off, Access(args[i], (uint64_t)p.size), (size_t)p.size);
            else StoreVal(frame + p.off, p.t, args[i]);
        }
        stack.resize(stack.size() - (size_t)nargs);
        frames.push_back(Frame { &f, 0, (uint64_t)fp, stack.size() });
        frametop = top;
        return true;
    }

    void Ret(VT t) {
        uint64_t v = t == VT_VOID ? 0 : Pop();
        auto &fr = frames.back();
        if (stack.size() != fr.sp) throw Fault { cat("internal: operand stack imbalance in '", fr.fn->name, "'") };
        frametop = fr.fp;
        segs[frameseg].size = frametop;
        frames.pop_back();
        if (!frames.empty()) frames.back().pc++;
        if (t != VT_VOID) Push(v);
    }

    // ---- The loop

    void Loop() {
        while (!frames.empty()) Step();
    }

    void Step() {
        auto &fr = frames.back();
        auto &in = fr.fn->code[fr.pc];
        switch (in.op) {
            case OP_NOP: break;
            case OP_PUSH: Push((uint64_t)in.imm); break;
            case OP_POP: Pop(); break;
            case OP_DUP: Push(Top()); break;
            case OP_LOCAL: Push(Handle(frameseg, fr.fp + (uint64_t)in.imm)); break;
            case OP_GLOBAL: Push(StaticHandle((size_t)in.imm)); break;
            case OP_FUNC: Push(FuncHandle((size_t)in.imm)); break;
            case OP_LOAD: Top() = LoadVal(Access(Top(), (uint64_t)VTSize(in.t)), in.t); break;
            case OP_STORE:
            case OP_STOREK: {
                auto v = Pop();
                auto a = Pop();
                StoreVal(Access(a, (uint64_t)VTSize(in.t)), in.t, v);
                if (in.op == OP_STOREK) Push(v);
                break;
            }
            case OP_COPY: {
                auto src = Pop(), dst = Pop();
                auto n = (uint64_t)in.imm;
                if (n) {
                    // Both accesses before either copy, since the second may
                    // grow a stack segment and move the first.
                    Access(dst, n);
                    auto s = Access(src, n);
                    auto d = Access(dst, n);
                    memmove(d, s, n);
                }
                break;
            }
            case OP_ZERO: {
                auto a = Pop();
                if (in.imm) memset(Access(a, (uint64_t)in.imm), 0, (size_t)in.imm);
                break;
            }
            case OP_OFFSET: Top() = PtrAdd(Top(), in.imm); break;
            case OP_PTRADD:
            case OP_PTRSUB: {
                auto i = (int64_t)Pop();
                auto delta = i * in.imm;
                Top() = PtrAdd(Top(), in.op == OP_PTRADD ? delta : -delta);
                break;
            }
            case OP_PTRDIFF: {
                auto b = Pop();
                Top() = (uint64_t)PtrDiff(Top(), b, in.imm);
                break;
            }
            case OP_INCDEC: {
                auto a = Pop();
                auto p = Access(a, (uint64_t)VTSize(in.t));
                auto old = LoadVal(p, in.t);
                uint64_t nv;
                if (in.t2 & 2) nv = PtrAdd(old, in.imm);
                else if (IsFloat(in.t)) nv = bit_cast<uint64_t>(bit_cast<double>(old) + (double)in.imm);
                else nv = Normalize(old + (uint64_t)in.imm, in.t);
                StoreVal(Access(a, (uint64_t)VTSize(in.t)), in.t, nv);
                Push((in.t2 & 1) ? old : nv);
                break;
            }
            case OP_NEG:
                if (IsFloat(in.t)) Top() = FloatBits(-bit_cast<double>(Top()), in.t);
                else Top() = Normalize(0 - Top(), in.t);
                break;
            case OP_NOT: Top() = Normalize(~Top(), in.t); break;
            case OP_LNOT: Top() = !Truth(Top(), in.t); break;
            case OP_TOBOOL: Top() = Truth(Top(), in.t); break;
            case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV: case OP_MOD:
            case OP_AND: case OP_OR: case OP_XOR: case OP_SHL: case OP_SHR: {
                auto b = Pop();
                Top() = Arith(in.op, in.t, Top(), b);
                break;
            }
            case OP_EQ: case OP_NE: case OP_LT: case OP_LE: case OP_GT: case OP_GE: {
                auto b = Pop();
                Top() = Compare(in.op, in.t, Top(), b);
                break;
            }
            case OP_CVT: Top() = Convert(Top(), in.t, (VT)in.t2); break;
            case OP_JMP: fr.pc = (size_t)in.imm; return;
            case OP_JZ:
                if (Pop() == 0) { fr.pc = (size_t)in.imm; return; }
                break;
            case OP_JNZ:
                if (Pop() != 0) { fr.pc = (size_t)in.imm; return; }
                break;
            case OP_CALL:
                if (Enter(prog.funcs[(size_t)in.imm], in.t2)) return;
                break;
            case OP_CALLI: {
                auto h = stack[stack.size() - 1 - in.t2];
                auto &s = Seg(h);
                if (s.kind != SEG_FUNC || OffOf(h) != 0) throw Fault { cat("call through a pointer to ", Describe(s)) };
                // The callee's handle sits below the arguments: move them down over it.
                stack.erase(stack.end() - 1 - in.t2);
                if (Enter(prog.funcs[(size_t)s.func], in.t2)) return;
                break;
            }
            case OP_RET: Ret(in.t); return;
            case OP_UNREACHABLE: throw Fault { "unreachable code reached" };
        }
        fr.pc++;
    }

    // ---- Arithmetic

    static uint64_t FloatBits(double d, VT t) {
        if (t == VT_F32) d = (double)(float)d;
        return bit_cast<uint64_t>(d);
    }

    static bool Truth(uint64_t v, VT t) {
        if (IsFloat(t)) return bit_cast<double>(v) != 0;
        return v != 0;
    }

    uint64_t Arith(Op op, VT t, uint64_t a, uint64_t b) {
        if (IsFloat(t)) {
            double x = bit_cast<double>(a), y = bit_cast<double>(b), r;
            switch (op) {
                case OP_ADD: r = x + y; break;
                case OP_SUB: r = x - y; break;
                case OP_MUL: r = x * y; break;
                case OP_DIV: r = x / y; break;
                default: throw Fault { cat("internal: ", OpName(op), " on a float") };
            }
            return FloatBits(r, t);
        }
        int bits = VTSize(t) * 8;
        bool uns = IsUnsigned(t);
        switch (op) {
            case OP_ADD: return Normalize(a + b, t);
            case OP_SUB: return Normalize(a - b, t);
            case OP_MUL: return Normalize(a * b, t);
            case OP_DIV:
            case OP_MOD: {
                if (b == 0) throw Fault { "division by zero" };
                if (uns) return Normalize(op == OP_DIV ? a / b : a % b, t);
                auto x = (int64_t)a, y = (int64_t)b;
                if (y == -1 && x == (bits == 64 ? INT64_MIN : -((int64_t)1 << (bits - 1))))
                    throw Fault { "integer overflow in division" };
                return Normalize((uint64_t)(op == OP_DIV ? x / y : x % y), t);
            }
            case OP_AND: return a & b;
            case OP_OR: return a | b;
            case OP_XOR: return a ^ b;
            case OP_SHL: return Normalize(a << (b & (uint64_t)(bits - 1)), t);
            case OP_SHR: {
                auto n = b & (uint64_t)(bits - 1);
                return uns ? a >> n : (uint64_t)((int64_t)a >> n);
            }
            default: throw Fault { cat("internal: ", OpName(op), " is not arithmetic") };
        }
    }

    static uint64_t Compare(Op op, VT t, uint64_t a, uint64_t b) {
        if (IsFloat(t)) {
            double x = bit_cast<double>(a), y = bit_cast<double>(b);
            switch (op) {
                case OP_EQ: return x == y;
                case OP_NE: return x != y;
                case OP_LT: return x < y;
                case OP_LE: return x <= y;
                case OP_GT: return x > y;
                default: return x >= y;
            }
        }
        if (IsUnsigned(t)) {
            switch (op) {
                case OP_EQ: return a == b;
                case OP_NE: return a != b;
                case OP_LT: return a < b;
                case OP_LE: return a <= b;
                case OP_GT: return a > b;
                default: return a >= b;
            }
        }
        auto x = (int64_t)a, y = (int64_t)b;
        switch (op) {
            case OP_EQ: return x == y;
            case OP_NE: return x != y;
            case OP_LT: return x < y;
            case OP_LE: return x <= y;
            case OP_GT: return x > y;
            default: return x >= y;
        }
    }

    static uint64_t Convert(uint64_t v, VT from, VT to) {
        if (to == VT_BOOL) return Truth(v, from);
        if (IsFloat(from)) {
            auto d = bit_cast<double>(v);
            if (IsFloat(to)) return FloatBits(d, to);
            // Out of range is undefined in C; the host's x86 gives the
            // "integer indefinite", which this mirrors for signed targets.
            if (d != d) return Normalize(0x8000000000000000ull, to);
            if (IsUnsigned(to)) {
                if (d <= -1) return Normalize((uint64_t)(int64_t)d, to);
                if (d >= 18446744073709551616.0) return Normalize(0, to);
                return Normalize((uint64_t)d, to);
            }
            if (d >= 9223372036854775808.0 || d < -9223372036854775808.0)
                return Normalize(0x8000000000000000ull, to);
            return Normalize((uint64_t)(int64_t)d, to);
        }
        if (IsFloat(to)) {
            double d = IsUnsigned(from) ? (double)v : (double)(int64_t)v;
            return FloatBits(d, to);
        }
        return Normalize(v, to);
    }

    // ---- Diagnostics

    string Location(const Frame &fr) const {
        auto &f = *fr.fn;
        int line = fr.pc < f.code.size() ? f.code[fr.pc].line : 0;
        auto file = f.decl ? prog.files[(size_t)f.decl->file] : string("<init>");
        return cat(f.name, " (", file, ":", line, ")");
    }

    void Report(const string &msg) {
        string s = cat("fault: ", msg, "\n");
        int shown = 0;
        for (size_t i = frames.size(); i-- > 0 && shown < 8; shown++)
            Append(s, "  in ", Location(frames[i]), "\n");
        if (frames.size() > 8) Append(s, "  ... ", frames.size() - 8, " more\n");
        write(2, s);
    }

    // ---- The C library

    unordered_map<string, Native> library;
    unordered_map<string, uint64_t> libobjects;   // extern variables: their values

    // The stream a FILE * handle names.
    int Fd(uint64_t h) {
        if (h == stdouth) return 1;
        if (h == stderrh) return 2;
        throw Fault { "not a stream" };
    }

    // The bytes of a NUL-terminated string, within its segment.
    string_view CString(uint64_t h) {
        auto &s = Seg(h);
        auto off = OffOf(h);
        if (!s.alive) throw Fault { cat("use after free: string at offset ", off, " of a ", Describe(s)) };
        if (off > s.size) throw Fault { cat("string at offset ", off, " past the end of ", Describe(s)) };
        auto p = s.mem.data() + off;
        auto end = (const uint8_t *)memchr(p, 0, (size_t)(s.size - off));
        if (!end) throw Fault { cat("unterminated string at offset ", off, " of ", Describe(s)) };
        return string_view((const char *)p, (size_t)(end - p));
    }

    void Library() {
        libobjects["stdout"] = stdouth;
        libobjects["stderr"] = stderrh;
        library["fwrite"] = [](VM &vm, const uint64_t *a, int) -> uint64_t {
            auto n = a[1] * a[2];
            auto fd = vm.Fd(a[3]);
            if (n) vm.write(fd, string_view((const char *)vm.Access(a[0], n), (size_t)n));
            return a[2];
        };
        library["fputs"] = [](VM &vm, const uint64_t *a, int) -> uint64_t {
            auto s = vm.CString(a[0]);
            vm.write(vm.Fd(a[1]), s);
            return 1;
        };
        library["fputc"] = [](VM &vm, const uint64_t *a, int) -> uint64_t {
            char c = (char)a[0];
            vm.write(vm.Fd(a[1]), string_view(&c, 1));
            return (uint8_t)c;
        };
        library["putchar"] = [](VM &vm, const uint64_t *a, int) -> uint64_t {
            char c = (char)a[0];
            vm.write(1, string_view(&c, 1));
            return (uint8_t)c;
        };
        library["puts"] = [](VM &vm, const uint64_t *a, int) -> uint64_t {
            vm.write(1, cat(vm.CString(a[0]), "\n"));
            return 1;
        };
        library["fflush"] = [](VM &, const uint64_t *, int) -> uint64_t { return 0; };
        library["memcpy"] = [](VM &vm, const uint64_t *a, int) -> uint64_t {
            if (a[2]) {
                vm.Access(a[0], a[2]);
                auto s = vm.Access(a[1], a[2]);
                memmove(vm.Access(a[0], a[2]), s, (size_t)a[2]);
            }
            return a[0];
        };
        library["memmove"] = library["memcpy"];
        library["memset"] = [](VM &vm, const uint64_t *a, int) -> uint64_t {
            if (a[2]) memset(vm.Access(a[0], a[2]), (int)a[1], (size_t)a[2]);
            return a[0];
        };
        library["memcmp"] = [](VM &vm, const uint64_t *a, int) -> uint64_t {
            if (!a[2]) return 0;
            auto x = vm.Access(a[0], a[2]);
            auto y = vm.Access(a[1], a[2]);
            int r = memcmp(x, y, (size_t)a[2]);
            return (uint64_t)(int64_t)(r < 0 ? -1 : r > 0 ? 1 : 0);
        };
        library["strlen"] = [](VM &vm, const uint64_t *a, int) -> uint64_t {
            return vm.CString(a[0]).size();
        };
        library["malloc"] = [](VM &vm, const uint64_t *a, int) -> uint64_t {
            return Handle(vm.NewSeg(SEG_HEAP, "", a[0]), 0);
        };
        library["calloc"] = [](VM &vm, const uint64_t *a, int) -> uint64_t {
            return Handle(vm.NewSeg(SEG_HEAP, "", a[0] * a[1]), 0);
        };
        library["realloc"] = [](VM &vm, const uint64_t *a, int) -> uint64_t {
            auto id = vm.NewSeg(SEG_HEAP, "", a[1]);
            if (a[0]) {
                auto &old = vm.Seg(a[0]);
                if (old.kind != SEG_HEAP || !old.alive || OffOf(a[0]) != 0)
                    throw Fault { cat("realloc of ", vm.Describe(old)) };
                memcpy(vm.segs[id].mem.data(), old.mem.data(), (size_t)min(old.size, a[1]));
                vm.Free(a[0]);
            }
            return Handle(id, 0);
        };
        library["free"] = [](VM &vm, const uint64_t *a, int) -> uint64_t {
            if (a[0]) vm.Free(a[0]);
            return 0;
        };
        library["exit"] = [](VM &, const uint64_t *a, int) -> uint64_t { throw Exit { (int)a[0] }; };
        library["_exit"] = library["exit"];
        library["abort"] = [](VM &, const uint64_t *, int) -> uint64_t { throw Fault { "abort() called" }; };
        // The runtime's GS_INTERP branch reserves a data stack with these
        // (§5, §10): a segment that grows on access up to the reservation.
        library["gs_host_reserve"] = [](VM &vm, const uint64_t *a, int) -> uint64_t {
            if (a[0] > 0xffffffffull) throw Fault { cat("stack reservation of ", a[0], " bytes exceeds the 4 GB a handle addresses") };
            auto id = vm.NewSeg(SEG_STACK, "", 0);
            vm.segs[id].limit = a[0];
            return Handle(id, 0);
        };
        library["gs_host_release"] = [](VM &vm, const uint64_t *a, int) -> uint64_t {
            auto &s = vm.Seg(a[0]);
            if (s.kind != SEG_STACK || OffOf(a[0]) != 0) throw Fault { cat("release of ", vm.Describe(s)) };
            vm.Free(a[0]);
            return 0;
        };
    }

    void Free(uint64_t h) {
        auto &s = Seg(h);
        if (s.kind != SEG_HEAP && s.kind != SEG_STACK) throw Fault { cat("free of ", Describe(s)) };
        if (!s.alive) throw Fault { cat("double free of ", Describe(s)) };
        if (OffOf(h) != 0) throw Fault { cat("free of an interior pointer (offset ", OffOf(h), ") of ", Describe(s)) };
        s.alive = false;
        s.mem = {};
    }
};

}  // namespace interp
}  // namespace goose
