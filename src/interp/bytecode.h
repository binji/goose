// Goose compiler — the interpreter's bytecode (docs/design/interpreter.md §4,
// step 3 of §14). A stack machine over 64-bit slots: an integer is kept
// sign- or zero-extended as its type says, a float as a double (a float's
// operations round their double result to float, which is exact for the
// four operations and sqrt), a pointer as a handle (§5). Every operation
// names the type it works in, so the VM decides nothing about types. A
// function's locals live in the frame segment at offsets lowering assigns.
#pragma once

namespace goose {
namespace interp {

// Value types, the operand types of instructions.
enum VT : uint8_t { VT_I8, VT_U8, VT_I16, VT_U16, VT_I32, VT_U32, VT_I64, VT_U64, VT_F32, VT_F64, VT_BOOL, VT_VOID };

inline VT VTOf(const CType *t) {
    switch (t->kind) {
        case TK_BOOL: return VT_BOOL;
        case TK_INT:
            switch (t->size) {
                case 1: return t->uns ? VT_U8 : VT_I8;
                case 2: return t->uns ? VT_U16 : VT_I16;
                case 4: return t->uns ? VT_U32 : VT_I32;
                default: return t->uns ? VT_U64 : VT_I64;
            }
        case TK_FLOAT: return t->size == 4 ? VT_F32 : VT_F64;
        case TK_PTR: return VT_U64;
        default: return VT_VOID;   // records and arrays are addressed, never a slot value
    }
}

inline bool IsFloat(VT t) { return t == VT_F32 || t == VT_F64; }
inline bool IsUnsigned(VT t) { return t == VT_U8 || t == VT_U16 || t == VT_U32 || t == VT_U64 || t == VT_BOOL; }
inline int VTSize(VT t) {
    switch (t) {
        case VT_I8: case VT_U8: case VT_BOOL: return 1;
        case VT_I16: case VT_U16: return 2;
        case VT_I32: case VT_U32: case VT_F32: return 4;
        case VT_I64: case VT_U64: case VT_F64: return 8;
        default: return 0;
    }
}
inline const char *VTName(VT t) {
    static const char *names[] = { "i8", "u8", "i16", "u16", "i32", "u32", "i64", "u64", "f32", "f64", "bool", "void" };
    return names[t];
}

// A value as its type keeps it in a slot: truncated and extended.
inline uint64_t Normalize(uint64_t v, VT t) {
    switch (t) {
        case VT_I8: return (uint64_t)(int64_t)(int8_t)v;
        case VT_U8: return (uint8_t)v;
        case VT_I16: return (uint64_t)(int64_t)(int16_t)v;
        case VT_U16: return (uint16_t)v;
        case VT_I32: return (uint64_t)(int64_t)(int32_t)v;
        case VT_U32: return (uint32_t)v;
        case VT_BOOL: return v != 0;
        default: return v;
    }
}

enum Op : uint8_t {
    OP_NOP,
    OP_PUSH,       // imm: a constant (a float's double bits)
    OP_POP,
    OP_DUP,
    OP_LOCAL,      // imm: offset; pushes the handle of a frame slot
    OP_GLOBAL,     // imm: static object index; pushes its handle
    OP_FUNC,       // imm: function index; pushes its handle
    OP_LOAD,       // t: pops an address, pushes the value there
    OP_STORE,      // t: pops a value then an address, stores
    OP_STOREK,     // t: as STORE, and pushes the value back
    OP_COPY,       // imm: size; pops a source then a destination address, copies
    OP_ZERO,       // imm: size; pops an address, zero-fills
    OP_OFFSET,     // imm: pops an address, adds imm to it (a field)
    OP_PTRADD,     // imm: scale; pops an integer then a pointer, pushes pointer + i * scale
    OP_PTRSUB,     // imm: scale; pops an integer then a pointer, pushes pointer - i * scale
    OP_PTRDIFF,    // imm: size; pops b then a, pushes (a - b) / size
    OP_INCDEC,     // t, imm: delta (a pointer's element size), t2: bit 0 post, bit 1 pointer;
                   // pops an address, adds delta to the value there, pushes the old (post) or new value
    OP_NEG, OP_NOT, OP_LNOT, OP_TOBOOL,      // t: unary on the top
    OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD,  // t: binary, pops b then a
    OP_AND, OP_OR, OP_XOR, OP_SHL, OP_SHR,
    OP_EQ, OP_NE, OP_LT, OP_LE, OP_GT, OP_GE,  // t: the operands' type; pushes an int 0 or 1
    OP_CVT,        // t: from, t2: to
    OP_JMP,        // imm: target
    OP_JZ,         // imm: target; pops, jumps if zero
    OP_JNZ,        // imm: target; pops, jumps if not zero
    OP_CALL,       // imm: function index, t2: argument count; the arguments are the top slots
    OP_CALLI,      // t2: argument count; the callee's handle is below the arguments
    OP_RET,        // t: the result type (VT_VOID for none); pops the result
    OP_UNREACHABLE,
};

inline const char *OpName(Op op) {
    static const char *names[] = {
        "nop", "push", "pop", "dup", "local", "global", "func", "load", "store", "storek", "copy",
        "zero", "offset", "ptradd", "ptrsub", "ptrdiff", "incdec", "neg", "not", "lnot", "tobool",
        "add", "sub", "mul", "div", "mod", "and", "or", "xor", "shl", "shr",
        "eq", "ne", "lt", "le", "gt", "ge", "cvt", "jmp", "jz", "jnz", "call", "calli",
        "ret", "unreachable",
    };
    return names[op];
}

struct Insn {
    Op op = OP_NOP;
    VT t = VT_VOID;
    uint8_t t2 = 0;
    int32_t line = 0;
    int64_t imm = 0;
};

// A parameter's slot in the callee's frame. A record parameter is passed as
// the address of the caller's copy and copied into its slot on entry.
struct Param {
    int64_t off = 0;
    VT t = VT_VOID;
    int64_t size = 0;    // a record's size; 0 for a scalar
};

struct Function {
    string name;
    CDecl *decl = nullptr;
    vector<Insn> code;           // empty for an external
    int64_t framesize = 0;
    vector<Param> params;        // the hidden result pointer first where sret
    bool sret = false;           // returns a record through a pointer the caller passes
    VT ret = VT_VOID;            // the result type on the stack (VT_VOID: none, or sret)
    bool external = false;       // declared by the unit, defined by the C library (vm.h)
    bool variadic = false;
};

// A static object: a global, a static local, a string literal, or a
// variable the program only declares (`extern FILE *stdout`), which the VM
// binds to one it provides.
struct StaticObj {
    string name;
    int64_t size = 0;
    string bytes;                // a literal's contents; else empty, zero-filled
    CDecl *decl = nullptr;
    bool isextern = false;
};

struct Program {
    vector<Function> funcs;
    vector<StaticObj> statics;
    vector<string> files;        // for diagnostics: a function's instructions are in its decl's file
    int init = -1;               // the function that runs the static initializers
    int main = -1;

    const Function *Find(const string &name) const {
        for (auto &f : funcs) if (f.name == name) return &f;
        return nullptr;
    }
};

// A listing, for the fixtures that look at lowering and for debugging.
inline string Disasm(const Program &p, const Function &f) {
    string s = cat(f.name, " frame ", f.framesize, f.sret ? " sret" : "", "\n");
    for (size_t i = 0; i < f.code.size(); i++) {
        auto &in = f.code[i];
        Append(s, "  ", i, ": ", OpName(in.op));
        switch (in.op) {
            case OP_PUSH: case OP_POP: case OP_DUP: case OP_NOP: case OP_UNREACHABLE:
                if (in.op == OP_PUSH) Append(s, " ", (int64_t)in.imm);
                break;
            case OP_LOCAL: case OP_OFFSET: case OP_COPY: case OP_ZERO: case OP_PTRADD: case OP_PTRSUB:
            case OP_PTRDIFF: case OP_JMP: case OP_JZ: case OP_JNZ:
                Append(s, " ", in.imm);
                break;
            case OP_GLOBAL: Append(s, " ", in.imm, " (", p.statics[(size_t)in.imm].name, ")"); break;
            case OP_FUNC: Append(s, " ", in.imm, " (", p.funcs[(size_t)in.imm].name, ")"); break;
            case OP_CALL: Append(s, " ", p.funcs[(size_t)in.imm].name, " ", (int)in.t2); break;
            case OP_CALLI: Append(s, " ", (int)in.t2); break;
            case OP_INCDEC:
                Append(s, ".", VTName(in.t), " ", in.imm, (in.t2 & 1) ? " post" : "", (in.t2 & 2) ? " ptr" : "");
                break;
            case OP_CVT: Append(s, ".", VTName(in.t), ".", VTName((VT)in.t2)); break;
            default: Append(s, ".", VTName(in.t)); break;
        }
        s += '\n';
    }
    return s;
}

}  // namespace interp
}  // namespace goose
