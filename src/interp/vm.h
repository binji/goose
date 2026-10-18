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
// table of adapters over the host's library (§6), in clib.h; this file has
// the primitives they decode through: accesses, strings and streams.
#pragma once

#include <errno.h>

#if defined(__GNUC__) || defined(__clang__)
#define GS_NOINLINE_ATTR __attribute__((noinline))
#elif defined(_MSC_VER)
#define GS_NOINLINE_ATTR __declspec(noinline)
#else
#define GS_NOINLINE_ATTR
#endif

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

// The offset is a signed 32-bit number: a segment's limit is at most 2 GB
// (GS_STACK_RESERVE, §5), so the negative half is for pointers before a
// segment's start, which C forms and never dereferences (codegen's
// relative-reference origin is a pool's base minus one). Access, as
// unsigned, finds them past every size.
inline uint64_t Handle(uint32_t id, uint64_t off) { return ((uint64_t)id << 32) | (uint32_t)off; }
inline uint32_t SegOf(uint64_t h) { return (uint32_t)(h >> 32); }
inline uint64_t OffOf(uint64_t h) { return (uint32_t)h; }
inline int64_t SOffOf(uint64_t h) { return (int32_t)(uint32_t)h; }

struct VM;
using Native = uint64_t (*)(VM &vm, const uint64_t *args, int nargs);

struct Frame {
    const Function *fn;
    size_t pc;
    uint64_t fp;                 // the frame's offset in the frame segment
    size_t sp;                   // the operand stack's height on entry, after the arguments were taken
};

struct VM {
    const Program &prog;

    // The standard streams: what fwrite and friends write to stdout (1) and
    // stderr (2), and what fread and friends read from stdin.
    function<void(int fd, string_view data)> write = [](int fd, string_view data) {
        fwrite(data.data(), 1, data.size(), fd == 2 ? stderr : stdout);
    };
    function<size_t(char *buf, size_t n)> read = [](char *buf, size_t n) {
        return fread(buf, 1, n, stdin);
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
        for (int fd = 0; fd < 3; fd++) {
            auto h = Handle(NewSeg(SEG_STATIC, fd == 0 ? "stdin" : fd == 1 ? "stdout" : "stderr", 1), 0);
            streams[h].fd = fd;
            libobjects[fd == 0 ? "stdin" : fd == 1 ? "stdout" : "stderr"] = h;
        }
        errnoh = Handle(NewSeg(SEG_STATIC, "errno", 4), 0);
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
        if (id >= segs.size()) throw Fault { cat("invalid pointer ", id, ":", SOffOf(h)) };
        return segs[id];
    }

    // The bytes of an access: `n` bytes at the handle, which must be within
    // a live segment, or within a stack segment's limit, which grows it.
    // The common case, within a live segment's size, is decided here; the
    // rest, the growth and every fault, in AccessSlow.
    uint8_t *Access(uint64_t h, uint64_t n) {
        auto id = SegOf(h);
        if (id != 0 && id < segs.size()) {
            auto &s = segs[id];
            auto off = OffOf(h);
            if (s.alive && off + n <= s.size) return s.mem.data() + off;
        }
        return AccessSlow(h, n);
    }

    GS_NOINLINE_ATTR uint8_t *AccessSlow(uint64_t h, uint64_t n) {
        auto &s = Seg(h);
        auto off = OffOf(h);
        if (!s.alive) throw Fault { cat("use after free: ", n, " bytes at offset ", SOffOf(h), " of a ", Describe(s)) };
        if (s.kind == SEG_FUNC) throw Fault { cat("data access to ", Describe(s)) };
        if (SOffOf(h) < 0)
            throw Fault { cat("access of ", n, " bytes at offset ", SOffOf(h), ", before the start of ", Describe(s)) };
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

    // Pointer arithmetic stays within reach of the segment: up to its
    // size, or a stack's limit, and below zero as far as the offset's
    // signed half goes. An integer cast to a pointer (segment 0) is plain
    // arithmetic.
    uint64_t PtrAdd(uint64_t p, int64_t delta) {
        auto id = SegOf(p);
        if (id == 0) return p + (uint64_t)delta;
        if (id >= segs.size()) throw Fault { cat("invalid pointer ", id, ":", SOffOf(p)) };
        auto &s = segs[id];
        auto off = SOffOf(p) + delta;
        auto lim = s.kind == SEG_STACK ? s.limit : s.size;
        if (off < INT32_MIN || off > (int64_t)lim)
            throw Fault { cat("pointer arithmetic leaves ", Describe(s), ": offset ", off) };
        return Handle(id, (uint64_t)off);
    }

    int64_t PtrDiff(uint64_t a, uint64_t b, int64_t size) {
        if (SegOf(a) != SegOf(b))
            throw Fault { cat("difference of pointers into different segments (",
                              SegOf(a) ? Describe(Seg(a)) : "null", " and ",
                              SegOf(b) ? Describe(Seg(b)) : "null", ")") };
        return (SOffOf(a) - SOffOf(b)) / size;
    }

    // ---- Values in memory, little-endian, as the host is

    static_assert(endian::native == endian::little, "the interpreter's hosts are little-endian");

    static uint64_t LoadVal(const uint8_t *p, VT t) {
        switch (t) {
            case VT_I8: return (uint64_t)(int64_t)(int8_t)p[0];
            case VT_U8: return p[0];
            case VT_BOOL: return p[0] != 0;
            case VT_I16: { int16_t v; memcpy(&v, p, 2); return (uint64_t)(int64_t)v; }
            case VT_U16: { uint16_t v; memcpy(&v, p, 2); return v; }
            case VT_I32: { int32_t v; memcpy(&v, p, 4); return (uint64_t)(int64_t)v; }
            case VT_U32: { uint32_t v; memcpy(&v, p, 4); return v; }
            case VT_F32: { float v; memcpy(&v, p, 4); return bit_cast<uint64_t>((double)v); }
            default: { uint64_t v; memcpy(&v, p, 8); return v; }
        }
    }

    static void StoreVal(uint8_t *p, VT t, uint64_t v) {
        switch (t) {
            case VT_I8: case VT_U8: case VT_BOOL: p[0] = (uint8_t)v; break;
            case VT_I16: case VT_U16: { uint16_t x = (uint16_t)v; memcpy(p, &x, 2); break; }
            case VT_I32: case VT_U32: { uint32_t x = (uint32_t)v; memcpy(p, &x, 4); break; }
            case VT_F32: { float x = (float)bit_cast<double>(v); memcpy(p, &x, 4); break; }
            default: memcpy(p, &v, 8); break;
        }
    }

    // ---- The operand stack and frames

    // The operand stack is one block, addressed by index here and through a
    // pointer in the loop; a call makes room for what its callee's code can
    // push (Function::maxstack), so nothing else checks.
    vector<uint64_t> stack = vector<uint64_t>(4096);
    size_t sp = 0;
    vector<Frame> frames;
    vector<uint64_t> statichandles, funchandles;
    vector<Native> natives;

    void Push(uint64_t v) {
        if (sp == stack.size()) stack.resize(stack.size() * 2);
        stack[sp++] = v;
    }
    uint64_t Pop() {
        assert(sp > 0);
        return stack[--sp];
    }
    uint64_t &Top() { return stack[sp - 1]; }

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
            auto args = stack.data() + sp - (size_t)nargs;
            auto r = fp(*this, args, nargs);
            sp -= (size_t)nargs;
            if (f.ret != VT_VOID) Push(Normalize(r, f.ret));
            return false;
        }
        if ((size_t)nargs != f.params.size())
            throw Fault { cat("'", f.name, "' called with ", nargs, " arguments, declared with ", f.params.size()) };
        if (sp + (size_t)f.maxstack > stack.size())
            stack.resize(max(stack.size() * 2, sp + (size_t)f.maxstack));
        auto &fs = segs[frameseg];
        auto fp = RoundUp((int64_t)frametop, 8);
        auto top = (uint64_t)fp + (uint64_t)f.framesize;
        if (top > fs.limit) throw Fault { "native call stack overflow" };
        if (top > fs.mem.size()) fs.mem.resize(max(top, (uint64_t)fs.mem.size() * 2));
        fs.size = top;
        auto frame = fs.mem.data() + fp;
        auto args = stack.data() + sp - (size_t)nargs;
        for (size_t i = 0; i < f.params.size(); i++) {
            auto &p = f.params[i];
            if (p.size) memmove(frame + p.off, Access(args[i], (uint64_t)p.size), (size_t)p.size);
            else StoreVal(frame + p.off, p.t, args[i]);
        }
        sp -= (size_t)nargs;
        frames.push_back(Frame { &f, 0, (uint64_t)fp, sp });
        frametop = top;
        return true;
    }

    void Ret(VT t) {
        uint64_t v = t == VT_VOID ? 0 : Pop();
        auto &fr = frames.back();
        if (sp != fr.sp) throw Fault { cat("internal: operand stack imbalance in '", fr.fn->name, "'") };
        frametop = fr.fp;
        segs[frameseg].size = frametop;
        frames.pop_back();
        if (!frames.empty()) frames.back().pc++;
        if (t != VT_VOID) Push(v);
    }

    // ---- The loop

    // Runs until no frame is left. The top frame's state lives in locals
    // while its code runs and goes back to the frame at a call, a return
    // or a fault, whose report reads the frames.
    void Loop() {
        while (!frames.empty()) {
            auto &fr = frames.back();
            const Function *fn = fr.fn;
            const Insn *code = fn->code.data();
            size_t pc = fr.pc;
            uint64_t fp = fr.fp;
            uint8_t *frame = segs[frameseg].mem.data() + fp;
            uint64_t *base = stack.data();
            uint64_t *sp = base + this->sp;
            try {
                for (;;) {
                    auto &in = code[pc];
                    switch (in.op) {
                        case OP_NOP: break;
                        case OP_PUSH: *sp++ = (uint64_t)in.imm; break;
                        case OP_POP: sp--; break;
                        case OP_DUP: sp[0] = sp[-1]; sp++; break;
                        case OP_LOCAL: *sp++ = Handle(frameseg, fp + (uint64_t)in.imm); break;
                        case OP_LLOAD: *sp++ = LoadVal(frame + in.imm, in.t); break;
                        case OP_LSTORE: StoreVal(frame + in.imm, in.t, *--sp); break;
                        case OP_LSTOREK: StoreVal(frame + in.imm, in.t, sp[-1]); break;
                        case OP_GLOBAL: *sp++ = StaticHandle((size_t)in.imm); break;
                        case OP_FUNC: *sp++ = FuncHandle((size_t)in.imm); break;
                        case OP_LOAD: sp[-1] = LoadVal(Access(sp[-1], (uint64_t)VTSize(in.t)), in.t); break;
                        case OP_STORE:
                        case OP_STOREK: {
                            auto v = sp[-1], a = sp[-2];
                            StoreVal(Access(a, (uint64_t)VTSize(in.t)), in.t, v);
                            sp -= in.op == OP_STOREK ? 1 : 2;
                            if (in.op == OP_STOREK) sp[-1] = v;
                            break;
                        }
                        case OP_COPY: {
                            auto src = sp[-1], dst = sp[-2];
                            sp -= 2;
                            auto n = (uint64_t)in.imm;
                            if (n) {
                                // Both accesses before either copy, since the
                                // second may grow a stack segment and move the first.
                                Access(dst, n);
                                auto s = Access(src, n);
                                auto d = Access(dst, n);
                                memmove(d, s, n);
                            }
                            break;
                        }
                        case OP_ZERO: {
                            auto a = *--sp;
                            if (in.imm) memset(Access(a, (uint64_t)in.imm), 0, (size_t)in.imm);
                            break;
                        }
                        case OP_OFFSET: sp[-1] = PtrAdd(sp[-1], in.imm); break;
                        case OP_PTRADD:
                        case OP_PTRSUB: {
                            auto i = (int64_t)*--sp;
                            auto delta = i * in.imm;
                            sp[-1] = PtrAdd(sp[-1], in.op == OP_PTRADD ? delta : -delta);
                            break;
                        }
                        case OP_PTRDIFF: {
                            auto b = *--sp;
                            sp[-1] = (uint64_t)PtrDiff(sp[-1], b, in.imm);
                            break;
                        }
                        case OP_INCDEC: {
                            auto a = sp[-1];
                            auto p = Access(a, (uint64_t)VTSize(in.t));
                            auto old = LoadVal(p, in.t);
                            uint64_t nv;
                            if (in.t2 & 2) nv = PtrAdd(old, in.imm);
                            else if (IsFloat(in.t)) nv = FloatBits(bit_cast<double>(old) + (double)in.imm, in.t);
                            else nv = Normalize(old + (uint64_t)in.imm, in.t);
                            StoreVal(Access(a, (uint64_t)VTSize(in.t)), in.t, nv);
                            sp[-1] = (in.t2 & 1) ? old : nv;
                            break;
                        }
                        case OP_NEG:
                            if (IsFloat(in.t)) sp[-1] = FloatBits(-bit_cast<double>(sp[-1]), in.t);
                            else sp[-1] = Normalize(0 - sp[-1], in.t);
                            break;
                        case OP_NOT: sp[-1] = Normalize(~sp[-1], in.t); break;
                        case OP_LNOT: sp[-1] = !Truth(sp[-1], in.t); break;
                        case OP_TOBOOL: sp[-1] = Truth(sp[-1], in.t); break;
                        case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV: case OP_MOD:
                        case OP_AND: case OP_OR: case OP_XOR: case OP_SHL: case OP_SHR: {
                            auto b = *--sp;
                            sp[-1] = Arith(in.op, in.t, sp[-1], b);
                            break;
                        }
                        case OP_EQ: case OP_NE: case OP_LT: case OP_LE: case OP_GT: case OP_GE: {
                            auto b = *--sp;
                            sp[-1] = Compare(in.op, in.t, sp[-1], b);
                            break;
                        }
                        case OP_CVT: sp[-1] = Convert(sp[-1], in.t, (VT)in.t2); break;
                        case OP_JMP: pc = (size_t)in.imm; continue;
                        case OP_JZ:
                            if (*--sp == 0) { pc = (size_t)in.imm; continue; }
                            break;
                        case OP_JNZ:
                            if (*--sp != 0) { pc = (size_t)in.imm; continue; }
                            break;
                        case OP_JCMP: {
                            auto b = sp[-1], a = sp[-2];
                            sp -= 2;
                            if (Compare((Op)in.t2, in.t, a, b)) { pc = (size_t)in.imm; continue; }
                            break;
                        }
                        case OP_CALL:
                            fr.pc = pc;
                            this->sp = (size_t)(sp - base);
                            if (Enter(prog.funcs[(size_t)in.imm], in.t2)) goto reload;
                            // A native ran: the stack block stays, the frame
                            // segment may have moved.
                            sp = stack.data() + this->sp;
                            base = stack.data();
                            frame = segs[frameseg].mem.data() + fp;
                            break;
                        case OP_CALLI: {
                            auto h = sp[-1 - in.t2];
                            auto &s = Seg(h);
                            if (s.kind != SEG_FUNC || OffOf(h) != 0)
                                throw Fault { cat("call through a pointer to ", Describe(s)) };
                            // The callee's handle sits below the arguments: move them down over it.
                            memmove(sp - 1 - in.t2, sp - in.t2, in.t2 * sizeof *sp);
                            sp--;
                            fr.pc = pc;
                            this->sp = (size_t)(sp - base);
                            if (Enter(prog.funcs[(size_t)s.func], in.t2)) goto reload;
                            sp = stack.data() + this->sp;
                            base = stack.data();
                            frame = segs[frameseg].mem.data() + fp;
                            break;
                        }
                        case OP_RET:
                            fr.pc = pc;
                            this->sp = (size_t)(sp - base);
                            Ret(in.t);
                            goto reload;
                        case OP_UNREACHABLE: throw Fault { "unreachable code reached" };
                    }
                    pc++;
                }
            } catch (Fault &) {
                // The report reads the frames, so the top one gets its place.
                fr.pc = pc;
                this->sp = (size_t)(sp - base);
                throw;
            }
        reload:;
        }
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

    // ---- What the C library adapters decode through (clib.h has the table)

    unordered_map<string, Native> library;
    unordered_map<string, uint64_t> libobjects;   // extern variables: their values
    uint64_t errnoh = 0;                          // errno's cell

    // A FILE *: one of the three standard streams, or a file the program
    // opened. A handle names one; the segment behind it has no bytes of its
    // own and dies when the file closes.
    struct Stream {
        int fd = -1;                 // 0, 1, 2 for the standard streams; -1 for a file
        FILE *file = nullptr;
        bool eof = false, err = false;
    };
    unordered_map<uint64_t, Stream> streams;
    unordered_map<uint64_t, void *> dirs;        // likewise for a DIR * (a host DIR)
    unordered_map<uint64_t, uint64_t> direntries; // a DIR's struct dirent, filled by each readdir

    Stream &StreamOf(uint64_t h) {
        auto it = streams.find(h);
        if (it == streams.end()) {
            if (h && SegOf(h) < segs.size() && !segs[SegOf(h)].alive)
                throw Fault { cat("use of a closed stream (", Describe(segs[SegOf(h)]), ")") };
            throw Fault { cat("not a stream: ", SegOf(h) ? Describe(Seg(h)) : "null") };
        }
        return it->second;
    }

    void Out(Stream &st, string_view data) {
        if (data.empty()) return;
        if (st.fd >= 0) { write(st.fd, data); return; }
        if (fwrite(data.data(), 1, data.size(), st.file) != data.size()) st.err = true;
    }

    size_t In(Stream &st, char *buf, size_t n) {
        if (n == 0) return 0;
        size_t got = st.fd >= 0 ? read(buf, n) : fread(buf, 1, n, st.file);
        if (got < n) {
            if (st.file && ferror(st.file)) st.err = true;
            else st.eof = true;
        }
        return got;
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

    // A copy of a string, for a C library call that needs a char *.
    string CStr(uint64_t h) { return string(CString(h)); }

    // A fresh static object holding the bytes and a NUL, for what the
    // library hands back as a string it owns (getenv, strerror).
    uint64_t NewString(string_view name, string_view bytes) {
        auto id = NewSeg(SEG_STATIC, string(name), bytes.size() + 1);
        memcpy(segs[id].mem.data(), bytes.data(), bytes.size());
        return Handle(id, 0);
    }

    // The program's errno, in the prelude's numbering (Linux's), from the
    // host's.
    void SetErrno(int hosterr) {
        int e;
        switch (hosterr) {
            case 0: e = 0; break;
            case EPERM: e = 1; break;
            case ENOENT: e = 2; break;
            case EINTR: e = 4; break;
            case EIO: e = 5; break;
            case EBADF: e = 9; break;
            case EAGAIN: e = 11; break;
            case ENOMEM: e = 12; break;
            case EACCES: e = 13; break;
            case EEXIST: e = 17; break;
            case ENOTDIR: e = 20; break;
            case EISDIR: e = 21; break;
            case EINVAL: e = 22; break;
            case EMFILE: e = 24; break;
            case ENOSPC: e = 28; break;
            case ERANGE: e = 34; break;
            case ENOTEMPTY: e = 39; break;
            default: e = 5; break;
        }
        StoreVal(Access(errnoh, 4), VT_I32, (uint64_t)(int64_t)e);
    }

    void Library();   // clib.h

    void Free(uint64_t h) {
        auto &s = Seg(h);
        if (s.kind != SEG_HEAP && s.kind != SEG_STACK) throw Fault { cat("free of ", Describe(s)) };
        if (!s.alive) throw Fault { cat("double free of ", Describe(s)) };
        if (OffOf(h) != 0) throw Fault { cat("free of an interior pointer (offset ", SOffOf(h), ") of ", Describe(s)) };
        s.alive = false;
        s.mem = {};
    }
};

}  // namespace interp
}  // namespace goose
