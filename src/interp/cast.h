// Goose compiler — the interpreter's C syntax tree (docs/design/interpreter.md
// §4, step 2 of §14). What the parser (cparse.h) builds: typed expressions,
// initializers and declarations. Every node is typed as it is made, with the
// conversions C implies made explicit (a cast node where an operand converts,
// a decay node where an array or function becomes a pointer), so that
// lowering (lower.h) has nothing left to decide about types.
#pragma once

namespace goose {
namespace interp {

struct CDecl;
struct Init;
struct Stmt;

enum ExprKind {
    E_INT,       // ival, of an integer type
    E_FLOAT,     // fval
    E_STR,       // sval: the bytes, NUL included; type [n]char
    E_VAR,       // decl: a variable or function
    E_FIELD,     // a.field
    E_DEREF,     // *a
    E_ADDR,      // &a
    E_DECAY,     // a, an array or function, as a pointer
    E_UNARY,     // op in "-", "+", "~", "!"
    E_BINARY,    // op: arithmetic, shifts, comparisons, bitwise, "&&", "||"
    E_ASSIGN,    // op "=" or a compound "+=", ...: a = b
    E_INCDEC,    // op "++" or "--", post or pre
    E_COND,      // a ? b : c
    E_CAST,      // (type) a
    E_CALL,      // a(args)
    E_COMMA,     // a, b
    E_COMPOUND,  // (type){ init }: decl is the object it initializes
};

struct Expr {
    ExprKind kind = E_INT;
    CType *type = nullptr;
    int file = 0, line = 0;
    string op;
    Expr *a = nullptr, *b = nullptr, *c = nullptr;
    vector<Expr *> args;
    uint64_t ival = 0;
    double fval = 0;
    string sval;
    CDecl *decl = nullptr;
    const CField *field = nullptr;
    Init *init = nullptr;
    CType *optype = nullptr;  // E_ASSIGN with a compound op: the type the operation is done in
    bool post = false;        // E_INCDEC
    bool lvalue = false;

    bool IsConst() const { return kind == E_INT; }
    int64_t Int() const { return (int64_t)ival; }
};

struct Designator {
    string field;             // .field, or
    Expr *index = nullptr;    // [index]
};

// An initializer: an expression, or a brace list. As parsed, the items may
// carry designators and elide braces; once typed (CParser::TypeInit) the
// list is positional, one item per member of the type in order, null where
// the source wrote none, and a union's one item is for member `ufield`.
struct Init {
    Expr *expr = nullptr;
    vector<Init *> items;
    vector<Designator> desig;
    int ufield = 0;
    int file = 0, line = 0;
    bool IsList() const { return !expr; }
};

enum DeclKind { D_VAR, D_FUNC, D_TYPEDEF };

struct CDecl {
    DeclKind kind = D_VAR;
    string name;
    CType *type = nullptr;
    bool isstatic = false;
    bool isextern = false;
    bool istls = false;
    bool defined = false;        // a function with a body, a variable with an initializer
    bool islocal = false;        // D_VAR: a function's local or parameter, in its frame
    Init *init = nullptr;
    vector<string> paramnames;   // D_FUNC, from the definition's declarator
    size_t bodybegin = 0, bodyend = 0;   // D_FUNC: the body's tokens, `{` to past `}`
    int file = 0, line = 0;
    // D_FUNC, once the body is parsed: the parameters, every local the body
    // declares (compound literals included), and the body itself.
    vector<CDecl *> params, locals;
    Stmt *body = nullptr;
    int64_t frameoff = 0;        // a local's offset in the frame, set by lowering
};

enum StmtKind {
    S_EMPTY,
    S_EXPR,      // expr
    S_DECL,      // decls: locals, initialized in order
    S_BLOCK,     // items
    S_IF,        // expr, a, b (else, or null)
    S_WHILE,     // expr, a
    S_DO,        // a, expr
    S_FOR,       // init (a statement: S_DECL, S_EXPR or S_EMPTY), expr (or null), step (or null), a
    S_SWITCH,    // expr, a; cases: every S_CASE and S_DEFAULT within it
    S_CASE,      // value, a
    S_DEFAULT,   // a
    S_BREAK,
    S_CONTINUE,
    S_GOTO,      // label
    S_LABEL,     // label, a
    S_RETURN,    // expr (or null), converted to the function's result type
};

struct Stmt {
    StmtKind kind = S_EMPTY;
    int file = 0, line = 0;
    Expr *expr = nullptr;
    Expr *step = nullptr;
    Stmt *init = nullptr;
    Stmt *a = nullptr, *b = nullptr;
    vector<Stmt *> items;
    vector<CDecl *> decls;
    vector<Stmt *> cases;
    string label;
    int64_t value = 0;
};

}  // namespace interp
}  // namespace goose
