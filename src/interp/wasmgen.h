// Goose compiler — the WebAssembly backend over the interpreter's bytecode
// (docs/design/wasm_backend.md §4 to §6). A Program lowered for the VM is
// emitted as a module whose functions are the unit's reachable ones, each
// instruction translated to the Wasm instructions that do what the VM's
// case for it does: the operand stack as Wasm's own, a value on it typed
// (an f32 or an f64 as itself, every integer and pointer an i64, which is
// the VM's slot with the float cases taken out; §8 item 4), locals in the
// frame segment at the offsets lowering gave them, and a pointer the same
// handle, decoded at a
// load or a store through the host's segment table with the bounds check
// the VM's Access makes, and the host's AccessSlow behind it for growth and
// every fault (§5). The module imports the host's memory and seven
// functions from a module named "goose" (§6) and exports _start, which
// sets the handles up, runs the static initializers and main, and reports
// the status through the stop import.
#pragma once

namespace goose {
namespace interp {

// ---- The binary

enum WasmType : uint8_t { W_I32 = 0x7F, W_I64 = 0x7E, W_F32 = 0x7D, W_F64 = 0x7C, W_FUNCREF = 0x70 };

struct WasmBuf {
    string b;
    void U8(uint8_t v) { b += (char)v; }
    void Uleb(uint64_t v) {
        do {
            uint8_t byte = v & 0x7f;
            v >>= 7;
            if (v) byte |= 0x80;
            U8(byte);
        } while (v);
    }
    void Sleb(int64_t v) {
        for (;;) {
            uint8_t byte = v & 0x7f;
            v >>= 7;
            bool done = (v == 0 && !(byte & 0x40)) || (v == -1 && (byte & 0x40));
            if (!done) byte |= 0x80;
            U8(byte);
            if (done) break;
        }
    }
    void Bytes(string_view s) { b.append(s.data(), s.size()); }
    void Name(string_view s) { Uleb(s.size()); Bytes(s); }
    // A size-prefixed sub-buffer: a section or a function body.
    void Sized(const WasmBuf &inner) { Uleb(inner.b.size()); Bytes(inner.b); }
};

// Opcodes the emitter uses.
enum WOp : uint8_t {
    WO_UNREACHABLE = 0x00, WO_NOP = 0x01, WO_BLOCK = 0x02, WO_LOOP = 0x03, WO_IF = 0x04, WO_ELSE = 0x05,
    WO_END = 0x0B, WO_BR = 0x0C, WO_BR_IF = 0x0D, WO_RETURN = 0x0F, WO_CALL = 0x10, WO_CALL_INDIRECT = 0x11,
    WO_DROP = 0x1A, WO_SELECT = 0x1B,
    WO_LOCAL_GET = 0x20, WO_LOCAL_SET = 0x21, WO_LOCAL_TEE = 0x22, WO_GLOBAL_GET = 0x23, WO_GLOBAL_SET = 0x24,
    WO_I32_LOAD = 0x28, WO_I64_LOAD = 0x29, WO_F32_LOAD = 0x2A, WO_F64_LOAD = 0x2B,
    WO_I64_LOAD8_S = 0x30, WO_I64_LOAD8_U = 0x31, WO_I64_LOAD16_S = 0x32, WO_I64_LOAD16_U = 0x33,
    WO_I64_LOAD32_S = 0x34, WO_I64_LOAD32_U = 0x35,
    WO_I32_STORE = 0x36, WO_I64_STORE = 0x37, WO_F32_STORE = 0x38, WO_F64_STORE = 0x39,
    WO_I64_STORE8 = 0x3C, WO_I64_STORE16 = 0x3D, WO_I64_STORE32 = 0x3E,
    WO_I32_CONST = 0x41, WO_I64_CONST = 0x42, WO_F32_CONST = 0x43, WO_F64_CONST = 0x44,
    WO_I32_EQZ = 0x45, WO_I32_EQ = 0x46, WO_I32_NE = 0x47, WO_I32_LT_U = 0x49, WO_I32_GT_U = 0x4B,
    WO_I32_GE_U = 0x4F,
    WO_I64_EQZ = 0x50, WO_I64_EQ = 0x51, WO_I64_NE = 0x52, WO_I64_LT_S = 0x53, WO_I64_LT_U = 0x54,
    WO_I64_GT_S = 0x55, WO_I64_GT_U = 0x56, WO_I64_LE_S = 0x57, WO_I64_LE_U = 0x58, WO_I64_GE_S = 0x59,
    WO_I64_GE_U = 0x5A,
    WO_F32_EQ = 0x5B, WO_F32_NE = 0x5C, WO_F32_LT = 0x5D, WO_F32_GT = 0x5E, WO_F32_LE = 0x5F, WO_F32_GE = 0x60,
    WO_F64_EQ = 0x61, WO_F64_NE = 0x62, WO_F64_LT = 0x63, WO_F64_GT = 0x64, WO_F64_LE = 0x65, WO_F64_GE = 0x66,
    WO_I32_ADD = 0x6A, WO_I32_SUB = 0x6B, WO_I32_AND = 0x71, WO_I32_OR = 0x72, WO_I32_SHL = 0x74,
    WO_I64_POPCNT = 0x7B, WO_I64_ADD = 0x7C, WO_I64_SUB = 0x7D, WO_I64_MUL = 0x7E, WO_I64_DIV_S = 0x7F, WO_I64_DIV_U = 0x80,
    WO_I64_REM_S = 0x81, WO_I64_REM_U = 0x82, WO_I64_AND = 0x83, WO_I64_OR = 0x84, WO_I64_XOR = 0x85,
    WO_I64_SHL = 0x86, WO_I64_SHR_S = 0x87, WO_I64_SHR_U = 0x88,
    WO_F32_NEG = 0x8C, WO_F32_ADD = 0x92, WO_F32_SUB = 0x93, WO_F32_MUL = 0x94, WO_F32_DIV = 0x95,
    WO_F64_NEG = 0x9A, WO_F64_ADD = 0xA0, WO_F64_SUB = 0xA1, WO_F64_MUL = 0xA2, WO_F64_DIV = 0xA3,
    WO_I32_WRAP_I64 = 0xA7, WO_I64_EXTEND_I32_S = 0xAC, WO_I64_EXTEND_I32_U = 0xAD,
    WO_I64_TRUNC_F64_S = 0xB0, WO_F32_CONVERT_I64_S = 0xB4, WO_F32_CONVERT_I64_U = 0xB5,
    WO_F32_DEMOTE_F64 = 0xB6, WO_F64_CONVERT_I64_S = 0xB9, WO_F64_CONVERT_I64_U = 0xBA,
    WO_F64_PROMOTE_F32 = 0xBB, WO_I64_REINTERPRET_F64 = 0xBD, WO_F64_REINTERPRET_I64 = 0xBF,
    WO_I64_EXTEND8_S = 0xC2, WO_I64_EXTEND16_S = 0xC3, WO_I64_EXTEND32_S = 0xC4,
    WO_PREFIX_FC = 0xFC,
};
// The 0xFC-prefixed ones.
enum WOpFC : uint8_t {
    WFC_I64_TRUNC_SAT_F64_S = 6, WFC_I64_TRUNC_SAT_F64_U = 7, WFC_MEMORY_COPY = 10, WFC_MEMORY_FILL = 11,
};

// The imports, in the order they take in the function index space (§6).
enum HostImport {
    IMP_NATIVE,          // (funcidx i32, args i32, nargs i32) -> i64
    IMP_ACCESS_SLOW,     // (handle i64, n i32) -> addr i32
    IMP_STATIC_HANDLE,   // (static i32) -> handle i64
    IMP_FUNC_HANDLE,     // (func i32, table index i32) -> handle i64
    IMP_INFO,            // (which i32) -> i64
    IMP_STOP,            // (status i32)
    IMP_FAULT,           // (code i32, a i64, b i64): records a fault and never returns
    IMP_COUNT,
};
inline const char *HostImportName(HostImport i) {
    static const char *names[] = { "native", "access_slow", "static_handle", "func_handle", "info", "stop", "fault" };
    return names[i];
}
// What info(which) answers.
enum HostInfo { INFO_SEGTAB, INFO_FRAMEBASE, INFO_FRAMELIMIT, INFO_FRAMESEG, INFO_ARGC, INFO_ARGV, INFO_ENVP };
// What fault(code, a, b) reports: the two handles are what the message names.
enum HostFault { FAULT_NATIVE_OVERFLOW, FAULT_BAD_CALL, FAULT_PTRDIFF };
// The segment table's entry: { base u32, size u32, kind u32, 0 } at id * 16
// (§5); a function segment's base is its index in the module's table.
constexpr uint32_t kSegEntry = 16;
constexpr int kSegEntryShift = 4;

// ---- The emitter

struct WasmGen {
    explicit WasmGen(const Program &prog) : prog(prog) {}

    // The module's bytes.
    string Run() {
        Reach();
        Signatures();
        Wrappers();
        for (auto fi : emitted) Body(fi);
        Start();
        return Module();
    }

private:
    const Program &prog;

    // ---- What is emitted

    vector<int> emitted;                 // the unit's function indices, in module order
    vector<int> wasmidx;                 // function index -> module function index, or -1
    vector<int> staticglobal;            // static object -> its handle's global, or -1
    vector<int> staticbase;              // static object -> the global holding its address (i32), or -1
    vector<int> funcglobal;              // function -> the global holding its handle (OP_FUNC), or -1
    int nglobals = 0;
    vector<WasmType> globaltype;
    enum { G_SEGTAB, G_FRAMEBASE, G_FRAMETOP, G_FRAMELIMIT, G_FRAMESEG, G_FIXED };
    int NewGlobal(WasmType t) { globaltype.push_back(t); return nglobals++; }

    // The static initializer, main, and everything they reach by a call or
    // a function handle; the static objects and the function handles those
    // name get globals.
    void Reach() {
        wasmidx.assign(prog.funcs.size(), -1);
        staticglobal.assign(prog.statics.size(), -1);
        staticbase.assign(prog.statics.size(), -1);
        funcglobal.assign(prog.funcs.size(), -1);
        nglobals = 0;
        globaltype.clear();
        for (int g = 0; g < G_FIXED; g++) NewGlobal(g == G_FRAMESEG ? W_I64 : W_I32);
        vector<bool> reached(prog.funcs.size(), false);
        vector<int> work;
        auto mark = [&](int fi) {
            if (fi < 0 || reached[(size_t)fi]) return;
            reached[(size_t)fi] = true;
            work.push_back(fi);
        };
        mark(prog.init);
        mark(prog.main);
        // The runtime's overflow reporters, which the host calls (§7).
        for (size_t i = 0; i < prog.funcs.size(); i++)
            if (prog.funcs[i].name == "gs_native_overflow" || prog.funcs[i].name == "gs_data_overflow") mark((int)i);
        while (!work.empty()) {
            auto fi = work.back();
            work.pop_back();
            auto &f = prog.funcs[(size_t)fi];
            if (f.external) continue;
            for (auto &in : f.code) {
                if (in.op == OP_CALL) mark((int)in.imm);
                else if (in.op == OP_FUNC) {
                    mark((int)in.imm);
                    if (funcglobal[(size_t)in.imm] < 0) funcglobal[(size_t)in.imm] = NewGlobal(W_I64);
                } else if (in.op == OP_GLOBAL) {
                    if (staticglobal[(size_t)in.imm] < 0) {
                        staticglobal[(size_t)in.imm] = NewGlobal(W_I64);
                        staticbase[(size_t)in.imm] = NewGlobal(W_I32);
                    }
                }
            }
        }
        // Defined functions get module indices after the imports; externals
        // are called through the native import by their unit index, except
        // one whose address is taken, which gets a trampoline in the
        // module so that its handle has a table entry.
        for (size_t i = 0; i < prog.funcs.size(); i++) {
            if (!reached[i] || (prog.funcs[i].external && funcglobal[i] < 0)) continue;
            wasmidx[i] = IMP_COUNT + (int)emitted.size();
            emitted.push_back((int)i);
        }
    }

    // ---- Types

    // A defined function's signature is its parameters' and result's
    // value types; an external's, a trampoline's and an indirect call's is
    // all i64, the slot form (§4 item 5), which a defined function with a
    // float in its signature gets a wrapper for (Wrappers).
    struct Sig {
        vector<WasmType> params, results;
        bool operator==(const Sig &o) const { return params == o.params && results == o.results; }
    };
    vector<Sig> sigs;
    int startsig = -1;

    int SigIndex(const Sig &s) {
        for (size_t i = 0; i < sigs.size(); i++) if (sigs[i] == s) return (int)i;
        sigs.push_back(s);
        return (int)sigs.size() - 1;
    }
    int SlotSig(int nargs, bool result) {
        Sig s;
        s.params.assign((size_t)nargs, W_I64);
        if (result) s.results.push_back(W_I64);
        return SigIndex(s);
    }
    // An external's parameters are its prototype's, with the hidden
    // result pointer first where it returns a record.
    static size_t Arity(const Function &f) {
        if (!f.external) return f.params.size();
        return f.decl->type->params.size() + (f.sret ? 1 : 0);
    }
    // A value's Wasm type: a float as itself, anything else an i64.
    static WasmType WT(VT t) { return t == VT_F32 ? W_F32 : t == VT_F64 ? W_F64 : W_I64; }
    static Sig FuncSig(const Function &f) {
        Sig s;
        if (f.external) s.params.assign(Arity(f), W_I64);
        else for (auto &p : f.params) s.params.push_back(p.size ? W_I64 : WT(p.t));
        if (f.ret != VT_VOID) s.results.push_back(f.external ? W_I64 : WT(f.ret));
        return s;
    }
    vector<int> importsig;
    vector<int> funcsig;                 // per emitted function

    void Signatures() {
        auto sig = [&](vector<WasmType> p, vector<WasmType> r) { return SigIndex(Sig { std::move(p), std::move(r) }); };
        importsig.resize(IMP_COUNT);
        importsig[IMP_NATIVE] = sig({ W_I32, W_I32, W_I32 }, { W_I64 });
        importsig[IMP_ACCESS_SLOW] = sig({ W_I64, W_I32 }, { W_I32 });
        importsig[IMP_STATIC_HANDLE] = sig({ W_I32 }, { W_I64 });
        importsig[IMP_FUNC_HANDLE] = sig({ W_I32, W_I32 }, { W_I64 });
        importsig[IMP_INFO] = sig({ W_I32 }, { W_I64 });
        importsig[IMP_STOP] = sig({ W_I32 }, {});
        importsig[IMP_FAULT] = sig({ W_I32, W_I64, W_I64 }, {});
        startsig = sig({}, {});
        for (auto fi : emitted) funcsig.push_back(SigIndex(FuncSig(prog.funcs[(size_t)fi])));
        Helpers();
    }

    // Where a pointer value came from, as far as it is known (§8 item 3): a
    // promoted local, a pointer-sized frame slot it was loaded from, or a
    // static object. Pointer arithmetic keeps it, since it keeps the
    // segment; a decode of a value with a known origin can use the base
    // and size hoisted for that origin where they are current.
    struct Origin {
        enum Kind : uint8_t { NONE, LOCAL, SLOT, STATIC } kind = NONE;
        int64_t off = 0;                 // LOCAL, SLOT: the frame offset; STATIC: the object
        bool operator==(const Origin &o) const { return kind == o.kind && off == o.off; }
    };
    struct Sym {
        enum Kind : uint8_t { VALUE, FRAME, STATIC } kind = VALUE;
        int64_t off = 0;
        int idx = -1;                    // STATIC: the object
        Origin origin;
        WasmType wt = W_I64;             // VALUE: the Wasm type of the value on the stack
    };
    vector<Sym> syms;
    // A promoted local's value's origin, where it was stored in this block
    // from a value with one.
    unordered_map<int64_t, Origin> localtag;

    // ---- A function

    // The function being emitted and its code.
    const Function *fn = nullptr;
    int fnidx = -1;
    WasmBuf code;
    vector<WasmBuf> bodies;              // per emitted function, locals and code

    // Wasm locals: the parameters first, then the fixed ones, then scratch.
    int nparams = 0;
    int L_FPOFF, L_FP;                   // the frame's offset in the segment and its address (i32)
    int L_FH, L_T1, L_T2, L_T3;          // the frame's handle, and scratch (i64)
    int L_TA;                            // a scratch address (i32)
    vector<int> argspill;                // i64 locals for a CALLI's arguments
    vector<int> spill;                   // i64 locals holding the operand stack across a jump (§4 item 4)
    int tmpf = 0, tmpg = 0;              // f64 and f32 locals for values set aside, as many as the first pass used
    int ntmp[2] = { 0, 0 };
    // Frame objects kept in Wasm locals (§8 item 1): a scalar nothing
    // addresses, reached only by the fused local instructions at its own
    // offset in one type. By frame offset, the local's index, or -1; the
    // local is typed as the object is.
    unordered_map<int64_t, int> promoted;
    int L_COND;                          // a jump's condition (i32)
    int nlocals = 0;
    int64_t scratch = 0;                 // bytes past the frame for a native call's arguments

    void Emit(uint8_t op) { code.U8(op); }
    void OpFC(uint8_t op) { code.U8(WO_PREFIX_FC); code.Uleb(op); }
    void I32(int32_t v) { Emit(WO_I32_CONST); code.Sleb(v); }
    void I64(int64_t v) { Emit(WO_I64_CONST); code.Sleb(v); }
    void F64(double d) {
        Emit(WO_F64_CONST);
        char bits[8];
        memcpy(bits, &d, 8);
        code.Bytes(string_view(bits, 8));
    }
    void F32(float f) {
        Emit(WO_F32_CONST);
        char bits[4];
        memcpy(bits, &f, 4);
        code.Bytes(string_view(bits, 4));
    }
    void LocalGet(int i) { Emit(WO_LOCAL_GET); code.Uleb((uint64_t)i); }
    void LocalSet(int i) { Emit(WO_LOCAL_SET); code.Uleb((uint64_t)i); }
    void LocalTee(int i) { Emit(WO_LOCAL_TEE); code.Uleb((uint64_t)i); }
    void GlobalGet(int i) { Emit(WO_GLOBAL_GET); code.Uleb((uint64_t)i); }
    void GlobalSet(int i) { Emit(WO_GLOBAL_SET); code.Uleb((uint64_t)i); }
    void Call(int wasmfunc) { Emit(WO_CALL); code.Uleb((uint64_t)wasmfunc); }
    void CallImport(HostImport i) { Call((int)i); }
    // A memory access: every one may be unaligned, so the hint is 0.
    void Mem(uint8_t op, int64_t offset) { Emit(op); code.Uleb(0); code.Uleb((uint64_t)offset); }
    void Block(uint8_t op, int type) { Emit(op); if (type < 0) code.U8(0x40); else code.U8((uint8_t)type); }

    void Body(int fi) {
        fn = &prog.funcs[(size_t)fi];
        fnidx = fi;
        code = WasmBuf();
        // GOOSE_WASM_DISASM=name lists that function's bytecode as it is
        // emitted, for looking into the translation.
        if (auto want = getenv("GOOSE_WASM_DISASM"); want && fn->name == want) fputs(Disasm(prog, *fn).c_str(), stderr);
        if (fn->external) { Trampoline(); return; }
        // What the code needs: the scratch room for native calls and the
        // locals for indirect calls.
        scratch = 0;
        size_t maxcalli = 0;
        for (auto &in : fn->code) {
            if (in.op == OP_CALL && prog.funcs[(size_t)in.imm].external) scratch = max<int64_t>(scratch, 8 * in.t2);
            if (in.op == OP_CALLI) maxcalli = max<size_t>(maxcalli, in.t2);
        }
        nparams = (int)fn->params.size();
        nlocals = nparams;
        L_FPOFF = nlocals++;
        L_FP = nlocals++;
        L_FH = nlocals++;
        L_T1 = nlocals++;
        L_T2 = nlocals++;
        L_T3 = nlocals++;
        L_TA = nlocals++;
        L_COND = nlocals++;
        argspill.clear();
        for (size_t i = 0; i < maxcalli; i++) argspill.push_back(nlocals++);
        Graph();
        spill.clear();
        for (int i = 0; i < maxheight; i++) spill.push_back(nlocals++);
        Promote();
        Regions();
        pairlocals = nlocals;
        npairs = 0;
        ntmp[0] = ntmp[1] = 0;
        // Twice: the first finds the pairs each region needs and the
        // typed temporaries the code uses, which come after the pairs.
        for (int pass = 0; pass < 2; pass++) {
            collecting = pass == 0;
            tmpf = pairlocals + 2 * npairs;
            tmpg = tmpf + ntmp[0];
            code = WasmBuf();
            ctx.clear();
            Prologue();
            Activate(0);
            LoadRegionPairs(0);
            DoTree(0);
            // Every path ends in a return; this makes the function's end
            // unreachable for validation.
            Emit(WO_UNREACHABLE);
            Emit(WO_END);
        }
        // The locals: fpoff, fp (i32), fh, t1, t2, t3 (i64), ta, cond
        // (i32), the argument and stack spills and the promoted i64
        // locals (i64), the promoted f64 and f32 ones, the pairs' bases
        // (i32) and sizes (i64), then the f64 and f32 temporaries.
        WasmBuf body;
        body.Uleb(10);
        body.Uleb(2); body.U8(W_I32);
        body.Uleb(4); body.U8(W_I64);
        body.Uleb(2); body.U8(W_I32);
        body.Uleb(argspill.size() + spill.size() + npromotedof[0]); body.U8(W_I64);
        body.Uleb((uint64_t)npromotedof[1]); body.U8(W_F64);
        body.Uleb((uint64_t)npromotedof[2]); body.U8(W_F32);
        body.Uleb((uint64_t)npairs); body.U8(W_I32);
        body.Uleb((uint64_t)npairs); body.U8(W_I64);
        body.Uleb((uint64_t)ntmp[0]); body.U8(W_F64);
        body.Uleb((uint64_t)ntmp[1]); body.U8(W_F32);
        body.Bytes(code.b);
        bodies.push_back(std::move(body));
    }

    // ---- Locals in Wasm locals

    int npromotedof[3] = { 0, 0, 0 };    // i64, f64, f32

    void Promote() {
        promoted.clear();
        npromotedof[0] = npromotedof[1] = npromotedof[2] = 0;
        vector<pair<int64_t, VT>> fresh;  // the locals that are not parameters, with their types
        // What each object is reached by: an address taken, or a fused
        // instruction at some offset in some type.
        struct Use { bool addressed = false, other = false; VT t = VT_VOID; };
        vector<Use> uses(fn->locals.size());
        auto objof = [&](int64_t off) -> int {
            for (size_t i = 0; i < fn->locals.size(); i++) {
                auto &o = fn->locals[i];
                if (off >= o.off && off < o.off + max<int64_t>(o.size, 1)) return (int)i;
            }
            return -1;
        };
        for (auto &in : fn->code) {
            if (in.op == OP_LOCAL) {
                if (auto i = objof(in.imm); i >= 0) uses[(size_t)i].addressed = true;
            } else if (in.op == OP_LLOAD || in.op == OP_LSTORE || in.op == OP_LSTOREK) {
                auto i = objof(in.imm);
                if (i < 0) continue;
                auto &u = uses[(size_t)i];
                auto &o = fn->locals[(size_t)i];
                if (in.imm != o.off || VTSize(in.t) != o.size || (u.t != VT_VOID && u.t != in.t)) u.other = true;
                u.t = in.t;
            }
        }
        for (size_t i = 0; i < fn->locals.size(); i++) {
            auto &o = fn->locals[(size_t)i];
            auto &u = uses[i];
            if (u.addressed || u.other || u.t == VT_VOID) continue;
            if (o.size != 1 && o.size != 2 && o.size != 4 && o.size != 8) continue;
            // A parameter's Wasm local is the parameter itself, unless it
            // is a record, whose local holds the caller's address.
            int param = -1;
            for (size_t p = 0; p < fn->params.size(); p++)
                if (fn->params[p].off == o.off) param = fn->params[p].size ? -2 : (int)p;
            if (param == -2) continue;
            if (param >= 0) promoted[o.off] = param;
            else fresh.push_back({ o.off, u.t });
        }
        // Grouped by type, in the order the body declares them.
        for (int g = 0; g < 3; g++) {
            auto want = g == 0 ? W_I64 : g == 1 ? W_F64 : W_F32;
            for (auto &[off, t] : fresh)
                if (WT(t) == want) promoted[off] = nlocals + npromotedof[0] + npromotedof[1] + npromotedof[2], npromotedof[g]++;
        }
        nlocals += npromotedof[0] + npromotedof[1] + npromotedof[2];
    }

    // ---- Control flow (§4, items 2 to 4)

    // The function's basic blocks: [start, end) of its code, the edges,
    // the operand stack's height on entry, and the dominator tree.
    struct BB {
        size_t start = 0, end = 0;
        vector<int> succ, pred;
        int height = -1;
        int rpo = -1;                    // reverse postorder number; -1 unreachable
        int idom = -1;
        bool loop = false, merge = false;
        vector<int> children;            // in the dominator tree
    };
    vector<BB> bbs;
    vector<int> order;                   // blocks in reverse postorder
    int maxheight = 0;                   // the operand stack's deepest, anywhere

    static bool IsJump(Op op) { return op == OP_JMP || op == OP_JZ || op == OP_JNZ || op == OP_JCMP; }
    static bool Ends(Op op) { return IsJump(op) || op == OP_RET || op == OP_UNREACHABLE; }

    [[noreturn]] void Bad(string_view what) const {
        throw CompileError { cat("wasm backend: ", fn->name, ": ", what) };
    }

    void Graph() {
        auto &code = fn->code;
        auto n = code.size();
        // Leaders: the start, every jump target, and what follows a jump
        // or a return.
        vector<bool> leader(n + 1, false);
        leader[0] = true;
        for (size_t i = 0; i < n; i++) {
            if (IsJump(code[i].op)) leader[(size_t)code[i].imm] = true;
            if (Ends(code[i].op) && i + 1 < n) leader[i + 1] = true;
        }
        bbs.clear();
        vector<int> blockof(n, -1);
        for (size_t i = 0; i < n; i++) {
            if (leader[i]) {
                BB b;
                b.start = i;
                bbs.push_back(b);
            }
            blockof[i] = (int)bbs.size() - 1;
            bbs.back().end = i + 1;
        }
        for (size_t b = 0; b < bbs.size(); b++) {
            auto &bb = bbs[b];
            auto &last = code[bb.end - 1];
            if (IsJump(last.op)) bb.succ.push_back(blockof[(size_t)last.imm]);
            if (last.op != OP_JMP && last.op != OP_RET && last.op != OP_UNREACHABLE && bb.end < n)
                bb.succ.push_back(blockof[bb.end]);
            for (auto s : bb.succ) bbs[(size_t)s].pred.push_back((int)b);
        }
        // Reverse postorder from the entry.
        order.clear();
        {
            vector<int> post;
            vector<char> state(bbs.size(), 0);
            vector<pair<int, size_t>> stack = { { 0, 0 } };
            state[0] = 1;
            while (!stack.empty()) {
                auto &[b, i] = stack.back();
                if (i < bbs[(size_t)b].succ.size()) {
                    auto s = bbs[(size_t)b].succ[i++];
                    if (!state[(size_t)s]) { state[(size_t)s] = 1; stack.push_back({ s, 0 }); }
                } else {
                    post.push_back(b);
                    stack.pop_back();
                }
            }
            for (size_t i = post.size(); i-- > 0;) {
                bbs[(size_t)post[i]].rpo = (int)order.size();
                order.push_back(post[i]);
            }
        }
        // Heights: followed along the edges from the entry at 0; every
        // edge into a block must agree.
        maxheight = 0;
        bbs[0].height = 0;
        for (auto b : order) {
            auto &bb = bbs[(size_t)b];
            int h = bb.height;
            if (h < 0) Bad("a block's stack height is unknown");
            for (size_t i = bb.start; i < bb.end; i++) {
                h += StackEffect(code[i]);
                maxheight = max(maxheight, h + 2);
            }
            if (h < 0) Bad("the operand stack underflows");
            for (auto s : bb.succ) {
                auto &sb = bbs[(size_t)s];
                if (sb.height >= 0 && sb.height != h) Bad("the operand stack's height differs between two paths");
                sb.height = h;
                maxheight = max(maxheight, h);
            }
        }
        // Dominators (Cooper, Harvey and Kennedy), over the reachable blocks.
        for (auto &bb : bbs) bb.idom = -1;
        bbs[0].idom = 0;
        for (bool changed = true; changed;) {
            changed = false;
            for (auto b : order) {
                if (b == 0) continue;
                int d = -1;
                for (auto p : bbs[(size_t)b].pred) {
                    if (bbs[(size_t)p].idom < 0) continue;
                    if (d < 0) { d = p; continue; }
                    // The intersection, walking up by rpo.
                    int x = p, y = d;
                    while (x != y) {
                        while (bbs[(size_t)x].rpo > bbs[(size_t)y].rpo) x = bbs[(size_t)x].idom;
                        while (bbs[(size_t)y].rpo > bbs[(size_t)x].rpo) y = bbs[(size_t)y].idom;
                    }
                    d = x;
                }
                if (bbs[(size_t)b].idom != d) { bbs[(size_t)b].idom = d; changed = true; }
            }
        }
        for (auto b : order) {
            auto &bb = bbs[(size_t)b];
            bb.children.clear();
            bb.loop = bb.merge = false;
        }
        for (auto b : order) {
            auto &bb = bbs[(size_t)b];
            int forward = 0;
            for (auto p : bbs[(size_t)b].pred) {
                if (bbs[(size_t)p].rpo < 0) continue;
                if (bbs[(size_t)p].rpo >= bb.rpo) {
                    bb.loop = true;
                    // Reducible: the header dominates the back edge's source.
                    int x = p;
                    while (x != b && x != 0) x = bbs[(size_t)x].idom;
                    if (x != b) Bad("irreducible control flow");
                } else forward++;
            }
            bb.merge = forward >= 2;
            if (b != 0) bbs[(size_t)bb.idom].children.push_back(b);
        }
    }

    // The structuring (Ramsey, Beyond Relooper, 2022): a loop for a header,
    // a block ending before each merge node placed where its dominator
    // is, a branch to either as a br, and every other edge inlined.
    struct Ctx { enum Kind { BLOCK, LOOP, IF } kind; int label; };
    vector<Ctx> ctx;

    void Br(Ctx::Kind kind, int label) {
        for (size_t i = ctx.size(); i-- > 0;)
            if (ctx[i].kind == kind && ctx[i].label == label) {
                Emit(WO_BR);
                code.Uleb(ctx.size() - 1 - i);
                return;
            }
        Bad("a branch to a block that is not open");
    }

    void DoTree(int b) {
        auto &bb = bbs[(size_t)b];
        if (bb.loop) {
            // The loop's pairs, loaded before it: a back edge does not
            // come this way.
            for (size_t r = 1; r < regions.size(); r++)
                if (regions[r].header == b) LoadRegionPairs((int)r);
            Block(WO_LOOP, -1);
            ctx.push_back({ Ctx::LOOP, b });
        }
        vector<int> merges;
        for (auto c : bb.children) if (bbs[(size_t)c].merge) merges.push_back(c);
        sort(merges.begin(), merges.end(), [&](int x, int y) { return bbs[(size_t)x].rpo > bbs[(size_t)y].rpo; });
        NodeWithin(b, merges, 0);
        if (bb.loop) { ctx.pop_back(); Emit(WO_END); }
    }

    void NodeWithin(int b, const vector<int> &merges, size_t i) {
        if (i < merges.size()) {
            Block(WO_BLOCK, -1);
            ctx.push_back({ Ctx::BLOCK, merges[i] });
            NodeWithin(b, merges, i + 1);
            ctx.pop_back();
            Emit(WO_END);
            DoTree(merges[i]);
            return;
        }
        BlockCode(b);
    }

    void DoBranch(int from, int to) {
        auto &t = bbs[(size_t)to];
        if (t.loop && bbs[(size_t)from].rpo >= t.rpo) Br(Ctx::LOOP, to);
        else if (t.merge) Br(Ctx::BLOCK, to);
        else DoTree(to);
    }

    // The operand stack across an edge: its h values, in slot form, into
    // the spill locals before, and back after.
    void Spill(int h) {
        MaterializeAll();
        for (int d = 0; d < h; d++) Coerce(d, W_I64);
        for (int i = h - 1; i >= 0; i--) LocalSet(spill[(size_t)i]);
    }
    void Reload(int h) {
        localtag.clear();
        syms.assign((size_t)h, Sym {});
        for (int i = 0; i < h; i++) LocalGet(spill[(size_t)i]);
    }

    void BlockCode(int b) {
        auto &bb = bbs[(size_t)b];
        auto &code = fn->code;
        Activate(b);
        Reload(bb.height);
        int h = bb.height;
        auto &last = code[bb.end - 1];
        auto body = Ends(last.op) ? bb.end - 1 : bb.end;
        for (size_t i = bb.start; i < body; i++) { Insn(code[i]); h += StackEffect(code[i]); }
        switch (Ends(last.op) ? last.op : OP_NOP) {
            case OP_RET: Insn(last); return;   // materializes its operand
            case OP_UNREACHABLE: Emit(WO_UNREACHABLE); return;
            case OP_JMP:
                Spill(h);
                DoBranch(b, bb.succ[0]);
                return;
            case OP_JZ: case OP_JNZ: case OP_JCMP: {
                // The condition as an i32, set aside while the rest of the
                // stack spills; then the two edges as the arms of an if.
                MaterializeAll();
                if (last.op == OP_JCMP) { Compare((Op)last.t2, last.t); h -= 2; }
                else { Coerce(0, W_I64); I64(0); Emit(last.op == OP_JZ ? WO_I64_EQ : WO_I64_NE); h -= 1; }
                syms.resize((size_t)h);
                if (h) { LocalSet(L_COND); Spill(h); LocalGet(L_COND); }
                Block(WO_IF, -1);
                ctx.push_back({ Ctx::IF, -1 });
                DoBranch(b, bb.succ[0]);
                Emit(WO_ELSE);
                DoBranch(b, bb.succ[1]);
                ctx.pop_back();
                Emit(WO_END);
                Emit(WO_UNREACHABLE);
                return;
            }
            default:
                // Falls through to the next block.
                Spill(h);
                DoBranch(b, bb.succ[0]);
                return;
        }
    }

    // An external called through its handle: its arguments, which arrive
    // as parameters, spilled past the frame top for the native import, as
    // a direct call spills them past its own frame.
    void Trampoline() {
        auto n = (int)Arity(*fn);
        for (int i = n - 1; i >= 0; i--) {
            GlobalGet(G_FRAMEBASE);
            GlobalGet(G_FRAMETOP);
            Emit(WO_I32_ADD);
            LocalGet(i);
            Mem(WO_I64_STORE, 8 * i);
        }
        I32(fnidx);
        GlobalGet(G_FRAMEBASE);
        GlobalGet(G_FRAMETOP);
        Emit(WO_I32_ADD);
        I32(n);
        CallImport(IMP_NATIVE);
        if (fn->ret == VT_VOID) Emit(WO_DROP);
        else Normalize(fn->ret);
        Emit(WO_END);
        WasmBuf body;
        body.Uleb(0);
        body.Bytes(code.b);
        bodies.push_back(std::move(body));
    }

    // Entry: the frame is bumped out of the frame segment, checked against
    // its limit (the native call stack overflow the runtime reports), and
    // the parameters are stored into their slots, a record one copied from
    // the caller's bytes.
    void Prologue() {
        GlobalGet(G_FRAMETOP);
        LocalTee(L_FPOFF);
        I32((int32_t)(fn->framesize + scratch));
        Emit(WO_I32_ADD);
        LocalTee(L_TA);
        GlobalGet(G_FRAMELIMIT);
        Emit(WO_I32_GT_U);
        Block(WO_IF, -1);
        {
            // The report runs in the room past the limit: lift it first.
            I32(-1);
            GlobalSet(G_FRAMELIMIT);
            int reporter = -1;
            for (size_t i = 0; i < prog.funcs.size(); i++)
                if (prog.funcs[i].name == "gs_native_overflow" && wasmidx[i] >= 0) reporter = wasmidx[i];
            if (reporter >= 0) Call(reporter);
            else { I32(FAULT_NATIVE_OVERFLOW); I64(0); I64(0); CallImport(IMP_FAULT); }
            Emit(WO_UNREACHABLE);
        }
        Emit(WO_END);
        LocalGet(L_TA);
        GlobalSet(G_FRAMETOP);
        GlobalGet(G_FRAMEBASE);
        LocalGet(L_FPOFF);
        Emit(WO_I32_ADD);
        LocalSet(L_FP);
        GlobalGet(G_FRAMESEG);
        LocalGet(L_FPOFF);
        Emit(WO_I64_EXTEND_I32_U);
        Emit(WO_I64_ADD);
        LocalSet(L_FH);
        for (size_t i = 0; i < fn->params.size(); i++) {
            auto &p = fn->params[i];
            if (promoted.count(p.off)) continue;
            if (p.size) {
                LocalGet(L_FP);
                I32((int32_t)p.off);
                Emit(WO_I32_ADD);
                LocalGet((int)i);
                Decode(p.size);
                I32((int32_t)p.size);
                OpFC(WFC_MEMORY_COPY); code.U8(0); code.U8(0);
            } else {
                LocalGet(L_FP);
                LocalGet((int)i);
                Store(p.t, p.off);
            }
        }
    }

    void Epilogue() {
        LocalGet(L_FPOFF);
        GlobalSet(G_FRAMETOP);
    }

    // ---- Memory

    // The handle on top becomes the address of n bytes at it: the VM's
    // Access (§5), with its slow path the host's.
    void Decode(int64_t n) { Decode(n, Origin()); }
    void Decode(int64_t n, Origin origin) {
        if (origin.kind == Origin::STATIC) {
            // A static object: its size is known and its address never
            // changes.
            LocalSet(L_T1);
            LocalGet(L_T1);
            I64(0xffffffffll);
            Emit(WO_I64_AND);
            I64(n);
            Emit(WO_I64_ADD);
            I64(prog.statics[(size_t)origin.off].size);
            Emit(WO_I64_GT_U);
            Block(WO_IF, W_I32);
            LocalGet(L_T1);
            I32((int32_t)n);
            CallImport(IMP_ACCESS_SLOW);
            RefreshPairs();
            Emit(WO_ELSE);
            GlobalGet(staticbase[(size_t)origin.off]);
            LocalGet(L_T1);
            Emit(WO_I32_WRAP_I64);
            Emit(WO_I32_ADD);
            Emit(WO_END);
            return;
        }
        if (auto pair = PairFor(origin)) {
            LocalSet(L_T1);
            LocalGet(L_T1);
            I64(0xffffffffll);
            Emit(WO_I64_AND);
            I64(n);
            Emit(WO_I64_ADD);
            LocalGet(PairSize(*pair));
            Emit(WO_I64_GT_U);
            Block(WO_IF, W_I32);
            LocalGet(L_T1);
            I32((int32_t)n);
            CallImport(IMP_ACCESS_SLOW);
            RefreshPairs();
            Emit(WO_ELSE);
            LocalGet(PairBase(*pair));
            LocalGet(L_T1);
            Emit(WO_I32_WRAP_I64);
            Emit(WO_I32_ADD);
            Emit(WO_END);
            return;
        }
        LocalSet(L_T1);
        // The entry's offset in the table: id * 8, an i32 that must be
        // within the table's capacity (a garbage id reads nothing).
        LocalGet(L_T1);
        I64(32);
        Emit(WO_I64_SHR_U);
        Emit(WO_I32_WRAP_I64);
        I32(kSegEntryShift);
        Emit(WO_I32_SHL);
        LocalSet(L_TA);
        // off + n > size, in 64 bits so that a negative offset cannot wrap
        // past the check; or an id past the table.
        LocalGet(L_T1);
        I64(0xffffffffll);
        Emit(WO_I64_AND);
        I64(n);
        Emit(WO_I64_ADD);
        GlobalGet(G_SEGTAB);
        LocalGet(L_TA);
        Emit(WO_I32_ADD);
        Mem(WO_I64_LOAD32_U, 4);
        Emit(WO_I64_GT_U);
        LocalGet(L_TA);
        I32((int32_t)(kSegEntry * kMaxSegments));
        Emit(WO_I32_GE_U);
        Emit(WO_I32_OR);
        Block(WO_IF, W_I32);
        {
            LocalGet(L_T1);
            I32((int32_t)n);
            CallImport(IMP_ACCESS_SLOW);
            RefreshPairs();
        }
        Emit(WO_ELSE);
        {
            GlobalGet(G_SEGTAB);
            LocalGet(L_TA);
            Emit(WO_I32_ADD);
            Mem(WO_I32_LOAD, 0);
            LocalGet(L_T1);
            Emit(WO_I32_WRAP_I64);
            Emit(WO_I32_ADD);
        }
        Emit(WO_END);
    }

    // ---- Hoisted decodes (§8 item 3)

    // A region is the whole function or a loop's body. An origin whose
    // handle nothing in the region changes gets a pair of locals there,
    // its segment's base and size, loaded on entry to the region and after
    // anything that can grow or free a segment: a call, or a decode's slow
    // path. A decode of a value with that origin then checks against the
    // pair and adds the base, with no table reads. Which origins a region
    // needs is found in a first emission of the function, whose output is
    // discarded; the second has the pairs.
    struct Pair { Origin origin; int base = 0, size = 0; };
    struct Region {
        int header = -1;                 // -1: the function
        vector<char> inbody;             // per block
        vector<pair<int64_t, int64_t>> writes;   // frame ranges stored to within the body
        vector<Pair> pairs;
    };
    vector<Region> regions;
    vector<int> active;                  // the regions holding the block being emitted, outermost first
    vector<pair<int64_t, int64_t>> escapes;   // frame objects whose address something other than a read takes
    bool collecting = false;
    int npairs = 0;
    int pairlocals = 0;                  // the first pair local; bases, then sizes

    static bool Overlaps(const vector<pair<int64_t, int64_t>> &ranges, int64_t off, int64_t size) {
        for (auto &[o, n] : ranges) if (off < o + n && o < off + size) return true;
        return false;
    }

    bool Invariant(const Region &r, Origin o) const {
        switch (o.kind) {
            case Origin::LOCAL: return !Overlaps(r.writes, o.off, 1);
            case Origin::SLOT: return !Overlaps(r.writes, o.off, 8) && !Overlaps(escapes, o.off, 8);
            default: return false;
        }
    }

    void Regions() {
        regions.clear();
        escapes.clear();
        auto &code = fn->code;
        auto objof = [&](int64_t off) -> const FrameObj * {
            for (auto &o : fn->locals) if (off >= o.off && off < o.off + max<int64_t>(o.size, 1)) return &o;
            return nullptr;
        };
        // An address taken for anything but a plain load escapes: the
        // object may be written by anyone from then on.
        for (size_t i = 0; i < code.size(); i++) {
            if (code[i].op != OP_LOCAL) continue;
            size_t j = i + 1;
            while (j < code.size() && code[j].op == OP_OFFSET) j++;
            if (j < code.size() && code[j].op == OP_LOAD) continue;
            if (auto o = objof(code[i].imm)) escapes.push_back({ o->off, max<int64_t>(o->size, 1) });
        }
        // The function, then each loop: its header and what reaches a back
        // edge's source without passing the header.
        Region whole;
        whole.inbody.assign(bbs.size(), 1);
        regions.push_back(whole);
        for (auto h : order) {
            auto &hb = bbs[(size_t)h];
            if (!hb.loop) continue;
            Region r;
            r.header = h;
            r.inbody.assign(bbs.size(), 0);
            r.inbody[(size_t)h] = 1;
            vector<int> work;
            for (auto p : hb.pred)
                if (bbs[(size_t)p].rpo >= hb.rpo && bbs[(size_t)p].rpo >= 0 && !r.inbody[(size_t)p]) { r.inbody[(size_t)p] = 1; work.push_back(p); }
            while (!work.empty()) {
                auto b = work.back();
                work.pop_back();
                for (auto p : bbs[(size_t)b].pred)
                    if (bbs[(size_t)p].rpo >= 0 && !r.inbody[(size_t)p]) { r.inbody[(size_t)p] = 1; work.push_back(p); }
            }
            regions.push_back(r);
        }
        for (auto &r : regions)
            for (size_t b = 0; b < bbs.size(); b++) {
                if (!r.inbody[b] || bbs[b].rpo < 0) continue;
                for (size_t i = bbs[b].start; i < bbs[b].end; i++) {
                    auto &in = code[i];
                    if (in.op == OP_LSTORE || in.op == OP_LSTOREK) r.writes.push_back({ in.imm, VTSize(in.t) });
                }
            }
        // Outer loops first: a larger body is an enclosing one.
        sort(regions.begin() + 1, regions.end(), [](const Region &a, const Region &b) {
            return count(a.inbody.begin(), a.inbody.end(), 1) > count(b.inbody.begin(), b.inbody.end(), 1);
        });
    }

    void Activate(int b) {
        active.clear();
        for (size_t r = 0; r < regions.size(); r++) if (regions[r].inbody[(size_t)b]) active.push_back((int)r);
    }

    // The pair for an origin in the outermost active region where it is
    // invariant, made on the first emission.
    const Pair *PairFor(Origin o) {
        if (o.kind == Origin::NONE) return nullptr;
        for (auto ri : active) {
            auto &r = regions[(size_t)ri];
            if (!Invariant(r, o)) continue;
            for (auto &p : r.pairs) if (p.origin == o) return &p;
            if (!collecting) return nullptr;
            Pair p;
            p.origin = o;
            p.base = npairs;
            p.size = npairs;
            npairs++;
            r.pairs.push_back(p);
            return &r.pairs.back();
        }
        return nullptr;
    }

    // The pair's locals, numbered after the first emission.
    int PairBase(const Pair &p) const { return pairlocals + p.base; }
    int PairSize(const Pair &p) const { return pairlocals + npairs + p.size; }

    // Loads a pair from the table: the origin's handle now, its entry, at
    // the sentinel past the table for an id beyond it.
    void LoadPair(const Pair &p) {
        if (p.origin.kind == Origin::LOCAL) LocalGet(promoted.at(p.origin.off));
        else { LocalGet(L_FP); Mem(WO_I64_LOAD, p.origin.off); }
        I64(32);
        Emit(WO_I64_SHR_U);
        Emit(WO_I32_WRAP_I64);
        I32(kSegEntryShift);
        Emit(WO_I32_SHL);
        LocalTee(L_TA);
        I32((int32_t)(kSegEntry * kMaxSegments));
        LocalGet(L_TA);
        I32((int32_t)(kSegEntry * kMaxSegments));
        Emit(WO_I32_LT_U);
        Emit(WO_SELECT);
        GlobalGet(G_SEGTAB);
        Emit(WO_I32_ADD);
        LocalTee(L_TA);
        Mem(WO_I32_LOAD, 0);
        LocalSet(PairBase(p));
        LocalGet(L_TA);
        Mem(WO_I64_LOAD32_U, 4);
        LocalSet(PairSize(p));
    }

    // After a call or a slow path: every pair of the regions the block is in.
    void RefreshPairs() {
        for (auto ri : active) for (auto &p : regions[(size_t)ri].pairs) LoadPair(p);
    }
    void LoadRegionPairs(int ri) {
        for (auto &p : regions[(size_t)ri].pairs) LoadPair(p);
    }

    // A typed load from the address on top, as the VM's LoadVal: an
    // integer extended to its i64 slot, a float as itself.
    void Load(VT t, int64_t offset) {
        switch (t) {
            case VT_I8: Mem(WO_I64_LOAD8_S, offset); break;
            case VT_U8: Mem(WO_I64_LOAD8_U, offset); break;
            case VT_BOOL: Mem(WO_I64_LOAD8_U, offset); I64(0); Emit(WO_I64_NE); Emit(WO_I64_EXTEND_I32_U); break;
            case VT_I16: Mem(WO_I64_LOAD16_S, offset); break;
            case VT_U16: Mem(WO_I64_LOAD16_U, offset); break;
            case VT_I32: Mem(WO_I64_LOAD32_S, offset); break;
            case VT_U32: Mem(WO_I64_LOAD32_U, offset); break;
            case VT_F32: Mem(WO_F32_LOAD, offset); break;
            case VT_F64: Mem(WO_F64_LOAD, offset); break;
            default: Mem(WO_I64_LOAD, offset); break;
        }
    }

    // A typed store of the value on top, which is of the type's Wasm
    // type, to the address below it.
    void Store(VT t, int64_t offset) {
        switch (t) {
            case VT_I8: case VT_U8: case VT_BOOL: Mem(WO_I64_STORE8, offset); break;
            case VT_I16: case VT_U16: Mem(WO_I64_STORE16, offset); break;
            case VT_I32: case VT_U32: Mem(WO_I64_STORE32, offset); break;
            case VT_F32: Mem(WO_F32_STORE, offset); break;
            case VT_F64: Mem(WO_F64_STORE, offset); break;
            default: Mem(WO_I64_STORE, offset); break;
        }
    }

    // ---- Values

    // The VM's Normalize on the i64 on top.
    void Normalize(VT t) {
        switch (t) {
            case VT_I8: Emit(WO_I64_EXTEND8_S); break;
            case VT_U8: I64(0xff); Emit(WO_I64_AND); break;
            case VT_I16: Emit(WO_I64_EXTEND16_S); break;
            case VT_U16: I64(0xffff); Emit(WO_I64_AND); break;
            case VT_I32: Emit(WO_I64_EXTEND32_S); break;
            case VT_U32: I64(0xffffffffll); Emit(WO_I64_AND); break;
            case VT_BOOL: I64(0); Emit(WO_I64_NE); Emit(WO_I64_EXTEND_I32_U); break;
            default: break;
        }
    }

    // The value on top from one Wasm type to another. An i64 holding a
    // float is the VM's slot form, the bits of its double, so an f32 goes
    // through f64 both ways, which is exact.
    void Convert(WasmType from, WasmType to) {
        if (from == to) return;
        switch (from) {
            case W_I64:
                Emit(WO_F64_REINTERPRET_I64);
                if (to == W_F32) Emit(WO_F32_DEMOTE_F64);
                break;
            case W_F32:
                Emit(WO_F64_PROMOTE_F32);
                if (to == W_I64) Emit(WO_I64_REINTERPRET_F64);
                break;
            default:
                Emit(to == W_F32 ? WO_F32_DEMOTE_F64 : WO_I64_REINTERPRET_F64);
                break;
        }
    }

    // A temporary of a type: the k-th spill local, or its f64 or f32
    // counterpart, of which a function declares as many as its first
    // emission asked for. Those are free between instructions, and within
    // one the first two are scratch.
    int Tmp(WasmType t, int k) {
        if (t == W_I64) return spill[(size_t)k];
        auto &n = ntmp[t == W_F64 ? 0 : 1];
        if (collecting) n = max(n, k + 1);
        else if (k >= n) Bad("a typed temporary the first pass did not see");
        return (t == W_F64 ? tmpf : tmpg) + k;
    }

    // The value at depth d from the top brought to a Wasm type, the
    // values above it, which must be on the stack, set aside meanwhile.
    void Coerce(int d, WasmType want) {
        auto idx = syms.size() - 1 - (size_t)d;
        auto &s = syms[idx];
        if (s.wt == want) return;
        for (size_t j = idx; j < syms.size(); j++) if (syms[j].kind != Sym::VALUE) Bad("a coercion under a deferred address");
        for (int k = 0; k < d; k++) LocalSet(Tmp(syms[idx + 1 + (size_t)(d - 1 - k)].wt, k));
        Convert(s.wt, want);
        s.wt = want;
        for (int k = d - 1; k >= 0; k--) LocalGet(Tmp(syms[idx + 1 + (size_t)(d - 1 - k)].wt, k));
    }

    // The VM's PtrAdd of the delta on top to the handle below it: the sum
    // in the low half, the segment id kept, so that a step before an
    // object's start never borrows from the id. (The VM lets a plain
    // integer, id 0, carry; here it wraps at 32 bits, which no emitted
    // code reaches.) Uses t2 and t3.
    void PtrAdd() {
        LocalSet(L_T3);
        LocalTee(L_T2);
        LocalGet(L_T3);
        Emit(WO_I64_ADD);
        I64(0xffffffffll);
        Emit(WO_I64_AND);
        LocalGet(L_T2);
        I64((int64_t)0xffffffff00000000ull);
        Emit(WO_I64_AND);
        Emit(WO_I64_OR);
    }

    // An i32 truth value on top into an i64 slot.
    void BoolSlot() { Emit(WO_I64_EXTEND_I32_U); }

    // The VM's Truth of the value on top, as an i32.
    void Truth(VT t) {
        Coerce(0, WT(t));
        if (t == VT_F32) { F32(0); Emit(WO_F32_NE); }
        else if (t == VT_F64) { F64(0); Emit(WO_F64_NE); }
        else { I64(0); Emit(WO_I64_NE); }
    }

    // A comparison of the two values on top, leaving an i32.
    void Compare(Op op, VT t) {
        Coerce(1, WT(t));
        Coerce(0, WT(t));
        if (t == VT_F32) {
            static const uint8_t ops[] = { WO_F32_EQ, WO_F32_NE, WO_F32_LT, WO_F32_LE, WO_F32_GT, WO_F32_GE };
            Emit(ops[op - OP_EQ]);
        } else if (t == VT_F64) {
            static const uint8_t ops[] = { WO_F64_EQ, WO_F64_NE, WO_F64_LT, WO_F64_LE, WO_F64_GT, WO_F64_GE };
            Emit(ops[op - OP_EQ]);
        } else if (IsUnsigned(t)) {
            static const uint8_t ops[] = { WO_I64_EQ, WO_I64_NE, WO_I64_LT_U, WO_I64_LE_U, WO_I64_GT_U, WO_I64_GE_U };
            Emit(ops[op - OP_EQ]);
        } else {
            static const uint8_t ops[] = { WO_I64_EQ, WO_I64_NE, WO_I64_LT_S, WO_I64_LE_S, WO_I64_GT_S, WO_I64_GE_S };
            Emit(ops[op - OP_EQ]);
        }
    }

    void Arith(Op op, VT t) {
        Coerce(1, WT(t));
        Coerce(0, WT(t));
        if (IsFloat(t)) {
            // An f32 operation rounds as the VM's double one rounded to
            // float does, since a double holds the exact result's
            // rounding (§8 item 4).
            bool f32 = t == VT_F32;
            switch (op) {
                case OP_ADD: Emit(f32 ? WO_F32_ADD : WO_F64_ADD); break;
                case OP_SUB: Emit(f32 ? WO_F32_SUB : WO_F64_SUB); break;
                case OP_MUL: Emit(f32 ? WO_F32_MUL : WO_F64_MUL); break;
                case OP_DIV: Emit(f32 ? WO_F32_DIV : WO_F64_DIV); break;
                default: throw CompileError { cat("wasm backend: ", OpName(op), " on a float") };
            }
            return;
        }
        int bits = VTSize(t) * 8;
        bool uns = IsUnsigned(t);
        switch (op) {
            case OP_ADD: Emit(WO_I64_ADD); Normalize(t); break;
            case OP_SUB: Emit(WO_I64_SUB); Normalize(t); break;
            case OP_MUL: Emit(WO_I64_MUL); Normalize(t); break;
            // Division by zero traps, as the VM faults; the overflow of the
            // most negative value by -1 traps in 64 bits and wraps below it.
            case OP_DIV: Emit(uns ? WO_I64_DIV_U : WO_I64_DIV_S); Normalize(t); break;
            case OP_MOD: Emit(uns ? WO_I64_REM_U : WO_I64_REM_S); Normalize(t); break;
            case OP_AND: Emit(WO_I64_AND); break;
            case OP_OR: Emit(WO_I64_OR); break;
            case OP_XOR: Emit(WO_I64_XOR); break;
            case OP_SHL: I64(bits - 1); Emit(WO_I64_AND); Emit(WO_I64_SHL); Normalize(t); break;
            case OP_SHR: I64(bits - 1); Emit(WO_I64_AND); Emit(uns ? WO_I64_SHR_U : WO_I64_SHR_S); break;
            default: throw CompileError { cat("wasm backend: ", OpName(op), " is not arithmetic") };
        }
    }

    // The VM's Convert, of the value on top.
    void Convert(VT from, VT to) {
        if (to == VT_BOOL) { Truth(from); BoolSlot(); return; }
        Coerce(0, WT(from));
        if (IsFloat(from)) {
            if (IsFloat(to)) { Convert(WT(from), WT(to)); return; }
            Convert(WT(from), W_F64);
            Call(IsUnsigned(to) ? helper_f2u : helper_f2i);
            Normalize(to);
            return;
        }
        if (IsFloat(to)) {
            if (to == VT_F32) Emit(IsUnsigned(from) ? WO_F32_CONVERT_I64_U : WO_F32_CONVERT_I64_S);
            else Emit(IsUnsigned(from) ? WO_F64_CONVERT_I64_U : WO_F64_CONVERT_I64_S);
            return;
        }
        Normalize(to);
    }

    // ---- The operand stack as emitted

    // What each instruction pops and pushes.
    static int Pops(const struct Insn &in) {
        switch (in.op) {
            case OP_POP: case OP_DUP: case OP_LSTORE: case OP_LSTOREK: case OP_LOAD: case OP_ZERO:
            case OP_OFFSET: case OP_INCDEC: case OP_NEG: case OP_NOT: case OP_LNOT: case OP_TOBOOL:
            case OP_CVT: case OP_JZ: case OP_JNZ:
                return 1;
            case OP_STORE: case OP_STOREK: case OP_COPY: case OP_PTRADD: case OP_PTRSUB: case OP_PTRDIFF:
            case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV: case OP_MOD:
            case OP_AND: case OP_OR: case OP_XOR: case OP_SHL: case OP_SHR:
            case OP_EQ: case OP_NE: case OP_LT: case OP_LE: case OP_GT: case OP_GE: case OP_JCMP:
                return 2;
            case OP_CALL: return in.t2;
            case OP_CALLI: return in.t2 + 1;
            case OP_RET: return in.t != VT_VOID;
            default: return 0;
        }
    }
    static int Pushes(const struct Insn &in) { return Pops(in) + StackEffect(in); }

    // The operand stack as the code has it, in step with Wasm's: an entry
    // is a value on Wasm's stack, or an address nothing has been emitted
    // for yet, a `local` or a `global` and the offsets added to it, which
    // the access that consumes it makes a direct access at the frame
    // pointer or the static object's address, so that a record in the
    // frame and a global are read and written without a decode. A static
    // object's is direct where the offset is within the object, which is
    // known here; any other use materializes the entry as the handle it is.

    bool Direct(const Sym &s, int64_t n) const {
        if (s.kind == Sym::FRAME) return true;
        if (s.kind == Sym::STATIC) return s.off >= 0 && s.off + n <= prog.statics[(size_t)s.idx].size;
        return false;
    }

    void Materialize(size_t idx) {
        vector<WasmType> above;          // the values above, top first
        for (size_t j = syms.size(); j-- > idx + 1;) if (syms[j].kind == Sym::VALUE) above.push_back(syms[j].wt);
        for (size_t k = 0; k < above.size(); k++) LocalSet(Tmp(above[k], (int)k));
        auto &s = syms[idx];
        if (s.kind == Sym::FRAME) { LocalGet(L_FH); I64(s.off); Emit(WO_I64_ADD); }
        else {
            GlobalGet(staticglobal[(size_t)s.idx]);
            if (s.off) { I64(s.off); PtrAdd(); }
            s.origin = Origin { Origin::STATIC, s.idx };
        }
        for (size_t k = above.size(); k-- > 0;) LocalGet(Tmp(above[k], (int)k));
        s.kind = Sym::VALUE;
        s.wt = W_I64;
    }

    // Materializes every deferred entry, or just the popped operands other
    // than those at the given depths from the top that an access of n
    // bytes can reach directly.
    void MaterializeAll() {
        for (size_t i = 0; i < syms.size(); i++) if (syms[i].kind != Sym::VALUE) Materialize(i);
    }
    void MaterializeOperands(int pops, int64_t n = 0, int keep0 = -1, int keep1 = -1) {
        for (int d = 0; d < pops; d++) {
            auto idx = syms.size() - 1 - (size_t)d;
            if (syms[idx].kind == Sym::VALUE) continue;
            if ((d == keep0 || d == keep1) && Direct(syms[idx], n)) continue;
            Materialize(idx);
        }
    }
    // The deferred address at depth d from the top, as an i32 address.
    void DirectAddr(int d) {
        auto &s = syms[syms.size() - 1 - (size_t)d];
        if (s.kind == Sym::FRAME) LocalGet(L_FP);
        else GlobalGet(staticbase[(size_t)s.idx]);
        I32((int32_t)s.off);
        Emit(WO_I32_ADD);
    }
    bool Deferred(int d) const { return syms[syms.size() - 1 - (size_t)d].kind != Sym::VALUE; }
    // A direct typed access at the deferred entry at depth d: its base and
    // the offset for the memarg.
    void DirectBase(int d) {
        auto &s = syms[syms.size() - 1 - (size_t)d];
        if (s.kind == Sym::FRAME) LocalGet(L_FP);
        else GlobalGet(staticbase[(size_t)s.idx]);
    }
    int64_t DirectOff(int d) const { return syms[syms.size() - 1 - (size_t)d].off; }

    // ---- Instructions

    void Insn(const struct Insn &in) {
        int pops = Pops(in), pushes = Pushes(in);
        switch (in.op) {
            case OP_LOAD: case OP_INCDEC: MaterializeOperands(pops, VTSize(in.t), 0); break;
            case OP_ZERO: MaterializeOperands(pops, in.imm, 0); break;
            case OP_OFFSET: case OP_DUP: case OP_POP: MaterializeOperands(pops, 0, 0); break;
            case OP_STORE: case OP_STOREK: MaterializeOperands(pops, VTSize(in.t), 1); break;
            case OP_COPY: MaterializeOperands(pops, in.imm, 0, 1); break;
            default: MaterializeOperands(pops); break;
        }
        Sym result;
        bool deferred = false;
        switch (in.op) {
            case OP_LOCAL: result = Sym { Sym::FRAME, in.imm, -1 }; deferred = true; break;
            case OP_GLOBAL: result = Sym { Sym::STATIC, 0, (int)in.imm }; deferred = true; break;
            case OP_OFFSET: if (Deferred(0)) { result = syms.back(); result.off += in.imm; deferred = true; } break;
            case OP_DUP: if (Deferred(0)) { result = syms.back(); deferred = true; } break;
            default: break;
        }
        // The result's origin, and what a store into a promoted local says
        // about the local's.
        if (!deferred) switch (in.op) {
            case OP_LLOAD:
                if (promoted.count(in.imm)) {
                    auto it = localtag.find(in.imm);
                    result.origin = it != localtag.end() ? it->second : Origin { Origin::LOCAL, in.imm };
                }
                break;
            case OP_LOAD:
                if (syms.back().kind == Sym::FRAME && in.t == VT_U64) result.origin = Origin { Origin::SLOT, syms.back().off };
                break;
            case OP_OFFSET: case OP_DUP: result.origin = syms.back().origin; break;
            case OP_PTRADD: case OP_PTRSUB: result.origin = syms[syms.size() - 2].origin; break;
            case OP_LSTORE: case OP_LSTOREK:
                if (promoted.count(in.imm)) {
                    if (syms.back().origin.kind != Origin::NONE) localtag[in.imm] = syms.back().origin;
                    else localtag.erase(in.imm);
                }
                result.origin = syms.back().origin;
                break;
            default: break;
        }
        if (!deferred) { result.wt = ResultType(in); Code(in); }
        for (int i = 0; i < pops; i++) syms.pop_back();
        for (int i = 0; i < pushes; i++) syms.push_back(result);
    }

    // The Wasm type of what an instruction pushes, which Code produces.
    WasmType ResultType(const struct Insn &in) const {
        switch (in.op) {
            case OP_LLOAD: case OP_LOAD: case OP_LSTOREK: case OP_STOREK: case OP_NEG:
            case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV: case OP_MOD:
            case OP_AND: case OP_OR: case OP_XOR: case OP_SHL: case OP_SHR:
                return WT(in.t);
            case OP_INCDEC: return (in.t2 & 2) ? W_I64 : WT(in.t);
            case OP_CVT: return WT((VT)in.t2);
            case OP_DUP: return syms.back().wt;
            case OP_CALL: {
                auto &callee = prog.funcs[(size_t)in.imm];
                return callee.external ? WT(in.t) : WT(callee.ret);
            }
            case OP_CALLI: return WT(in.t);
            default: return W_I64;
        }
    }

    void Code(const struct Insn &in) {
        switch (in.op) {
            case OP_NOP: break;
            case OP_PUSH: I64(in.imm); break;
            case OP_POP: if (!Deferred(0)) Emit(WO_DROP); break;
            case OP_DUP: LocalTee(Tmp(syms.back().wt, 0)); LocalGet(Tmp(syms.back().wt, 0)); break;
            case OP_LOCAL: LocalGet(L_FH); I64(in.imm); Emit(WO_I64_ADD); break;
            case OP_LLOAD:
                if (auto it = promoted.find(in.imm); it != promoted.end()) { LocalGet(it->second); break; }
                LocalGet(L_FP);
                Load(in.t, in.imm);
                break;
            case OP_LSTORE:
            case OP_LSTOREK:
                // A promoted local holds the value as a load gives it:
                // normalized to its type.
                Coerce(0, WT(in.t));
                if (auto it = promoted.find(in.imm); it != promoted.end()) {
                    Normalize(in.t);
                    if (in.op == OP_LSTOREK) LocalTee(it->second);
                    else LocalSet(it->second);
                    break;
                }
                if (in.op == OP_LSTOREK) LocalTee(Tmp(WT(in.t), 1));
                else LocalSet(Tmp(WT(in.t), 1));
                LocalGet(L_FP);
                LocalGet(Tmp(WT(in.t), 1));
                Store(in.t, in.imm);
                break;
            case OP_GLOBAL: GlobalGet(staticglobal[(size_t)in.imm]); break;
            case OP_FUNC: GlobalGet(funcglobal[(size_t)in.imm]); break;
            case OP_LOAD:
                if (Deferred(0)) { DirectBase(0); Load(in.t, DirectOff(0)); break; }
                Decode(VTSize(in.t), syms.back().origin);
                Load(in.t, 0);
                break;
            case OP_STORE:
            case OP_STOREK:
                // The value above the address: set aside, in a temporary
                // a decode leaves alone, while the address decodes, then
                // stored at it.
                Coerce(0, WT(in.t));
                LocalSet(Tmp(WT(in.t), 1));
                if (Deferred(1)) { DirectBase(1); LocalGet(Tmp(WT(in.t), 1)); Store(in.t, DirectOff(1)); }
                else { Decode(VTSize(in.t), syms[syms.size() - 2].origin); LocalGet(Tmp(WT(in.t), 1)); Store(in.t, 0); }
                if (in.op == OP_STOREK) LocalGet(Tmp(WT(in.t), 1));
                break;
            case OP_COPY: {
                auto n = in.imm;
                bool dst = Deferred(1), src = Deferred(0);
                if (!n) { if (!src) Emit(WO_DROP); if (!dst) Emit(WO_DROP); break; }
                // Both accesses before either copy, as the VM does: the
                // destination's may grow a stack and move the source. The
                // handles wait in t2 and t3, which Decode leaves alone.
                if (!src) LocalSet(L_T3);  // the source
                if (!dst) {
                    LocalTee(L_T2);        // the destination
                    Decode(n, syms[syms.size() - 2].origin);
                    Emit(WO_DROP);
                    LocalGet(L_T2);
                    Decode(n, syms[syms.size() - 2].origin);
                } else DirectAddr(1);
                if (!src) { LocalGet(L_T3); Decode(n, syms.back().origin); }
                else DirectAddr(0);
                I32((int32_t)n);
                OpFC(WFC_MEMORY_COPY); code.U8(0); code.U8(0);
                break;
            }
            case OP_ZERO:
                if (!in.imm) { if (!Deferred(0)) Emit(WO_DROP); break; }
                if (Deferred(0)) DirectAddr(0);
                else Decode(in.imm, syms.back().origin);
                I32(0);
                I32((int32_t)in.imm);
                OpFC(WFC_MEMORY_FILL); code.U8(0);
                break;
            case OP_OFFSET: I64(in.imm); PtrAdd(); break;
            case OP_PTRADD: I64(in.imm); Emit(WO_I64_MUL); PtrAdd(); break;
            case OP_PTRSUB: I64(-in.imm); Emit(WO_I64_MUL); PtrAdd(); break;
            case OP_PTRDIFF:
                // Both in one segment, else the VM's fault; the difference
                // of the signed low halves, divided by the size.
                LocalSet(L_T2);
                LocalTee(L_T1);
                LocalGet(L_T2);
                Emit(WO_I64_XOR);
                I64(32);
                Emit(WO_I64_SHR_U);
                Emit(WO_I64_EQZ);
                Emit(WO_I32_EQZ);
                Block(WO_IF, -1);
                I32(FAULT_PTRDIFF);
                LocalGet(L_T1);
                LocalGet(L_T2);
                CallImport(IMP_FAULT);
                Emit(WO_UNREACHABLE);
                Emit(WO_END);
                LocalGet(L_T1);
                Emit(WO_I64_EXTEND32_S);
                LocalGet(L_T2);
                Emit(WO_I64_EXTEND32_S);
                Emit(WO_I64_SUB);
                I64(in.imm);
                Emit(WO_I64_DIV_S);
                break;
            case OP_INCDEC: {
                auto wt = (in.t2 & 2) ? W_I64 : WT(in.t);
                int old = Tmp(wt, 0), fresh = Tmp(wt, 1);
                if (Deferred(0)) DirectAddr(0);
                else Decode(VTSize(in.t), syms.back().origin);
                LocalTee(L_TA);
                Load(in.t, 0);
                LocalTee(old);
                if (in.t2 & 2) { I64(in.imm); PtrAdd(); }
                else if (in.t == VT_F32) { F32((float)in.imm); Emit(WO_F32_ADD); }
                else if (in.t == VT_F64) { F64((double)in.imm); Emit(WO_F64_ADD); }
                else { I64(in.imm); Emit(WO_I64_ADD); Normalize(in.t); }
                LocalSet(fresh);
                LocalGet(L_TA);
                LocalGet(fresh);
                Store(in.t, 0);
                LocalGet((in.t2 & 1) ? old : fresh);
                break;
            }
            case OP_NEG:
                Coerce(0, WT(in.t));
                if (in.t == VT_F32) Emit(WO_F32_NEG);
                else if (in.t == VT_F64) Emit(WO_F64_NEG);
                else { LocalSet(L_T1); I64(0); LocalGet(L_T1); Emit(WO_I64_SUB); Normalize(in.t); }
                break;
            case OP_NOT: I64(-1); Emit(WO_I64_XOR); Normalize(in.t); break;
            case OP_LNOT: Truth(in.t); Emit(WO_I32_EQZ); BoolSlot(); break;
            case OP_TOBOOL: Truth(in.t); BoolSlot(); break;
            case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV: case OP_MOD:
            case OP_AND: case OP_OR: case OP_XOR: case OP_SHL: case OP_SHR:
                Arith(in.op, in.t);
                break;
            case OP_EQ: case OP_NE: case OP_LT: case OP_LE: case OP_GT: case OP_GE:
                Compare(in.op, in.t);
                BoolSlot();
                break;
            case OP_CVT: Convert(in.t, (VT)in.t2); break;
            case OP_JMP: case OP_JZ: case OP_JNZ: case OP_JCMP: Bad("a jump outside its block"); break;
            case OP_CALL: {
                auto &callee = prog.funcs[(size_t)in.imm];
                // The runtime's portable popcount, a loop, is one instruction here.
                if (callee.name == "gs_popcount" && in.t2 == 1 && in.t == VT_I64) { Emit(WO_I64_POPCNT); break; }
                if (!callee.external) {
                    // The arguments as the callee's signature types them.
                    for (int i = 0; i < (int)in.t2; i++) {
                        auto &p = callee.params[(size_t)i];
                        Coerce((int)in.t2 - 1 - i, p.size ? W_I64 : WT(p.t));
                    }
                    Call(wasmidx[(size_t)in.imm]);
                    RefreshPairs();
                    break;
                }
                // An external: the arguments, in slot form, spilled past
                // the frame, the host called with the unit's index of the
                // function.
                for (int i = 0; i < (int)in.t2; i++) Coerce(i, W_I64);
                for (int i = (int)in.t2 - 1; i >= 0; i--) {
                    LocalSet(L_T2);
                    LocalGet(L_FP);
                    LocalGet(L_T2);
                    Mem(WO_I64_STORE, fn->framesize + 8 * i);
                }
                I32((int32_t)in.imm);
                LocalGet(L_FP);
                I32((int32_t)fn->framesize);
                Emit(WO_I32_ADD);
                I32(in.t2);
                CallImport(IMP_NATIVE);
                if (in.t == VT_VOID) Emit(WO_DROP);
                else { Normalize(in.t); Convert(W_I64, WT(in.t)); }
                RefreshPairs();
                break;
            }
            case OP_CALLI: {
                // The callee's handle is below the arguments: they go into
                // locals, in slot form, the handle decodes to its table
                // index, and they come back for the indirect call.
                for (int i = 0; i < (int)in.t2; i++) Coerce(i, W_I64);
                for (int i = (int)in.t2 - 1; i >= 0; i--) LocalSet(argspill[(size_t)i]);
                LocalTee(L_T1);
                I64(32);
                Emit(WO_I64_SHR_U);
                Emit(WO_I32_WRAP_I64);
                I32(kSegEntryShift);
                Emit(WO_I32_SHL);
                LocalTee(L_TA);
                I32((int32_t)(kSegEntry * kMaxSegments));
                Emit(WO_I32_GE_U);
                // The VM's check: a function segment, at its start.
                GlobalGet(G_SEGTAB);
                LocalGet(L_TA);
                Emit(WO_I32_ADD);
                Mem(WO_I32_LOAD, 8);
                I32(SEG_FUNC);
                Emit(WO_I32_NE);
                Emit(WO_I32_OR);
                LocalGet(L_T1);
                Emit(WO_I32_WRAP_I64);
                Emit(WO_I32_OR);
                Block(WO_IF, -1);
                I32(FAULT_BAD_CALL);
                LocalGet(L_T1);
                I64(0);
                CallImport(IMP_FAULT);
                Emit(WO_UNREACHABLE);
                Emit(WO_END);
                GlobalGet(G_SEGTAB);
                LocalGet(L_TA);
                Emit(WO_I32_ADD);
                Mem(WO_I32_LOAD, 0);
                LocalSet(L_TA);
                for (int i = 0; i < (int)in.t2; i++) LocalGet(argspill[(size_t)i]);
                LocalGet(L_TA);
                Emit(WO_CALL_INDIRECT);
                code.Uleb((uint64_t)SlotSig(in.t2, in.t != VT_VOID));
                code.U8(0);
                if (in.t != VT_VOID) Convert(W_I64, WT(in.t));
                RefreshPairs();
                break;
            }
            case OP_RET:
                if (in.t != VT_VOID) Coerce(0, WT(fn->ret));
                Epilogue();
                Emit(WO_RETURN);
                break;
            case OP_UNREACHABLE: Emit(WO_UNREACHABLE); break;
        }
    }

    // ---- The start function

    WasmBuf startbody;

    void Start() {
        code = WasmBuf();
        for (int which = INFO_SEGTAB; which <= INFO_FRAMESEG; which++) {
            I32(which);
            CallImport(IMP_INFO);
            if (which == INFO_FRAMESEG) GlobalSet(G_FRAMESEG);
            else { Emit(WO_I32_WRAP_I64); GlobalSet(which == INFO_SEGTAB ? G_SEGTAB : which == INFO_FRAMEBASE ? G_FRAMEBASE : G_FRAMELIMIT); }
        }
        I32(0);
        GlobalSet(G_FRAMETOP);
        for (size_t i = 0; i < prog.statics.size(); i++)
            if (staticglobal[i] >= 0) {
                I32((int32_t)i);
                CallImport(IMP_STATIC_HANDLE);
                GlobalSet(staticglobal[i]);
                // Its address, which never changes: a static segment is
                // neither grown nor freed. The table's entry for the handle.
                GlobalGet(staticglobal[i]);
                I64(32);
                Emit(WO_I64_SHR_U);
                Emit(WO_I32_WRAP_I64);
                I32(kSegEntryShift);
                Emit(WO_I32_SHL);
                GlobalGet(G_SEGTAB);
                Emit(WO_I32_ADD);
                Mem(WO_I32_LOAD, 0);
                GlobalSet(staticbase[i]);
            }
        for (size_t i = 0; i < prog.funcs.size(); i++)
            if (funcglobal[i] >= 0) {
                I32((int32_t)i);
                I32(TableIndex((int)i));
                CallImport(IMP_FUNC_HANDLE);
                GlobalSet(funcglobal[i]);
            }
        if (prog.init >= 0) Call(wasmidx[(size_t)prog.init]);
        auto &m = prog.funcs[(size_t)prog.main];
        if (!m.params.empty()) {
            I32(INFO_ARGC); CallImport(IMP_INFO);
            I32(INFO_ARGV); CallImport(IMP_INFO);
            if (m.params.size() > 2) { I32(INFO_ENVP); CallImport(IMP_INFO); }
        }
        Call(wasmidx[(size_t)prog.main]);
        if (m.ret == VT_VOID) I32(0);
        else Emit(WO_I32_WRAP_I64);
        CallImport(IMP_STOP);
        Emit(WO_END);
        startbody = WasmBuf();
        startbody.Uleb(0);
        startbody.Bytes(code.b);
    }

    // ---- The table

    // Every emitted function is in the table at its position, and an
    // indirect call takes the slot form of its signature, so a defined
    // function with a float in its own gets a wrapper after them, which
    // converts and calls it, and is what its handle names.
    vector<int> tableidx;                // per function, or -1
    struct Wrapper { int fi; int sig; WasmBuf body; };
    vector<Wrapper> wrappers;

    void Wrappers() {
        tableidx.assign(prog.funcs.size(), -1);
        for (size_t i = 0; i < emitted.size(); i++) tableidx[(size_t)emitted[i]] = (int)i;
        for (auto fi : emitted) {
            auto &f = prog.funcs[(size_t)fi];
            if (f.external || funcglobal[(size_t)fi] < 0) continue;
            auto typed = FuncSig(f);
            auto slot = SlotSig((int)typed.params.size(), !typed.results.empty());
            if (SigIndex(typed) == slot) continue;
            code = WasmBuf();
            for (size_t i = 0; i < typed.params.size(); i++) { LocalGet((int)i); Convert(W_I64, typed.params[i]); }
            Call(wasmidx[(size_t)fi]);
            if (!typed.results.empty()) Convert(typed.results[0], W_I64);
            Emit(WO_END);
            tableidx[(size_t)fi] = (int)(emitted.size() + wrappers.size());
            Wrapper w { fi, slot, WasmBuf() };
            w.body.Uleb(0);
            w.body.Bytes(code.b);
            wrappers.push_back(std::move(w));
        }
    }
    int TableIndex(int fi) const { return tableidx[(size_t)fi]; }

    // ---- Helpers emitted in Wasm

    // The VM's Convert from a float to an integer mirrors what the host's
    // x86 does outside the defined range, which Wasm's saturating
    // conversions do not: two small functions, (f64) -> i64, with the
    // VM's cases.
    struct Helper { string name; int sig; WasmBuf body; };
    vector<Helper> helpers;
    int helper_f2i = -1, helper_f2u = -1;

    void Helpers() {
        auto sig = SigIndex(Sig { { W_F64 }, { W_I64 } });
        auto ret = [&](int64_t v) { I64(v); Emit(WO_RETURN); };
        // if (d != d) INT64_MIN; if (d >= 2^63 || d < -2^63) INT64_MIN; else (int64_t)d
        code = WasmBuf();
        LocalGet(0); LocalGet(0); Emit(WO_F64_NE); Block(WO_IF, -1); ret(INT64_MIN); Emit(WO_END);
        LocalGet(0); F64(9223372036854775808.0); Emit(WO_F64_GE); Block(WO_IF, -1); ret(INT64_MIN); Emit(WO_END);
        LocalGet(0); F64(-9223372036854775808.0); Emit(WO_F64_LT); Block(WO_IF, -1); ret(INT64_MIN); Emit(WO_END);
        LocalGet(0); OpFC(WFC_I64_TRUNC_SAT_F64_S);
        Emit(WO_END);
        helper_f2i = IMP_COUNT + (int)emitted.size() + (int)helpers.size();
        helpers.push_back(Helper { "f64_to_i64", sig, WasmBuf() });
        helpers.back().body.Uleb(0);
        helpers.back().body.Bytes(code.b);
        // if (d != d) INT64_MIN; if (d <= -1) (uint64_t)(int64_t)d; if (d >= 2^64) 0; else (uint64_t)d
        code = WasmBuf();
        LocalGet(0); LocalGet(0); Emit(WO_F64_NE); Block(WO_IF, -1); ret(INT64_MIN); Emit(WO_END);
        LocalGet(0); F64(-1.0); Emit(WO_F64_LE); Block(WO_IF, -1); LocalGet(0); OpFC(WFC_I64_TRUNC_SAT_F64_S); Emit(WO_RETURN); Emit(WO_END);
        LocalGet(0); F64(18446744073709551616.0); Emit(WO_F64_GE); Block(WO_IF, -1); ret(0); Emit(WO_END);
        LocalGet(0); OpFC(WFC_I64_TRUNC_SAT_F64_U);
        Emit(WO_END);
        helper_f2u = IMP_COUNT + (int)emitted.size() + (int)helpers.size();
        helpers.push_back(Helper { "f64_to_u64", sig, WasmBuf() });
        helpers.back().body.Uleb(0);
        helpers.back().body.Bytes(code.b);
    }

    // ---- Assembly

    string Module() {
        WasmBuf m;
        m.Bytes(string_view("\0asm\1\0\0\0", 8));
        auto section = [&](uint8_t id, const WasmBuf &payload) { m.U8(id); m.Sized(payload); };
        // Types.
        {
            WasmBuf s;
            s.Uleb(sigs.size());
            for (auto &sig : sigs) {
                s.U8(0x60);
                s.Uleb(sig.params.size());
                for (auto t : sig.params) s.U8(t);
                s.Uleb(sig.results.size());
                for (auto t : sig.results) s.U8(t);
            }
            section(1, s);
        }
        // Imports: the functions, then the memory.
        {
            WasmBuf s;
            s.Uleb(IMP_COUNT + 1);
            for (int i = 0; i < IMP_COUNT; i++) {
                s.Name("goose");
                s.Name(HostImportName((HostImport)i));
                s.U8(0x00);
                s.Uleb((uint64_t)importsig[(size_t)i]);
            }
            s.Name("goose");
            s.Name("memory");
            s.U8(0x02);
            s.U8(0x00);
            s.Uleb(0);
            section(2, s);
        }
        // Functions: the emitted ones, then the helpers, the wrappers,
        // and _start.
        auto nfuncs = emitted.size() + helpers.size() + wrappers.size() + 1;
        auto wrapper0 = IMP_COUNT + emitted.size() + helpers.size();
        {
            WasmBuf s;
            s.Uleb(nfuncs);
            for (auto sig : funcsig) s.Uleb((uint64_t)sig);
            for (auto &h : helpers) s.Uleb((uint64_t)h.sig);
            for (auto &w : wrappers) s.Uleb((uint64_t)w.sig);
            s.Uleb((uint64_t)startsig);
            section(3, s);
        }
        // The table, holding every emitted function and the wrappers.
        {
            WasmBuf s;
            s.Uleb(1);
            s.U8(W_FUNCREF);
            s.U8(0x01);
            s.Uleb(emitted.size() + wrappers.size());
            s.Uleb(emitted.size() + wrappers.size());
            section(4, s);
        }
        // Globals: the fixed ones, then a handle and an address per static
        // object and a handle per function handle, all mutable, set by _start.
        {
            WasmBuf s;
            s.Uleb((uint64_t)nglobals);
            for (int g = 0; g < nglobals; g++) {
                s.U8(globaltype[(size_t)g]);
                s.U8(0x01);
                s.U8(globaltype[(size_t)g] == W_I32 ? WO_I32_CONST : WO_I64_CONST);
                s.Sleb(0);
                s.U8(WO_END);
            }
            section(6, s);
        }
        // Exports: _start, and every function by its name, for the host's
        // reporter calls and for debugging.
        {
            WasmBuf s;
            s.Uleb(emitted.size() + 1);
            s.Name("_start");
            s.U8(0x00);
            s.Uleb(IMP_COUNT + nfuncs - 1);
            for (size_t i = 0; i < emitted.size(); i++) {
                s.Name(prog.funcs[(size_t)emitted[i]].name);
                s.U8(0x00);
                s.Uleb(IMP_COUNT + i);
            }
            section(7, s);
        }
        // The element segment filling the table.
        {
            WasmBuf s;
            s.Uleb(1);
            s.U8(0x00);
            s.U8(WO_I32_CONST); s.Sleb(0); s.U8(WO_END);
            s.Uleb(emitted.size() + wrappers.size());
            for (size_t i = 0; i < emitted.size(); i++) s.Uleb(IMP_COUNT + i);
            for (size_t i = 0; i < wrappers.size(); i++) s.Uleb(wrapper0 + i);
            section(9, s);
        }
        // Code.
        {
            WasmBuf s;
            s.Uleb(nfuncs);
            for (auto &b : bodies) s.Sized(b);
            for (auto &h : helpers) s.Sized(h.body);
            for (auto &w : wrappers) s.Sized(w.body);
            s.Sized(startbody);
            section(10, s);
        }
        // Names, so that a trap's stack trace names the function.
        {
            WasmBuf names;
            names.Uleb(IMP_COUNT + nfuncs);
            for (int i = 0; i < IMP_COUNT; i++) { names.Uleb((uint64_t)i); names.Name(HostImportName((HostImport)i)); }
            for (size_t i = 0; i < emitted.size(); i++) { names.Uleb(IMP_COUNT + i); names.Name(prog.funcs[(size_t)emitted[i]].name); }
            for (size_t i = 0; i < helpers.size(); i++) { names.Uleb(IMP_COUNT + emitted.size() + i); names.Name(helpers[i].name); }
            for (size_t i = 0; i < wrappers.size(); i++) { names.Uleb(wrapper0 + i); names.Name(prog.funcs[(size_t)wrappers[i].fi].name + "$slot"); }
            names.Uleb(IMP_COUNT + nfuncs - 1);
            names.Name("_start");
            WasmBuf s;
            s.Name("name");
            s.U8(1);
            s.Sized(names);
            section(0, s);
        }
        return std::move(m.b);
    }
};

}  // namespace interp
}  // namespace goose
