/* Declarations in the shapes codegen emits and the runtime writes, laid out
   twice: by the interpreter's parser, and by the C++ compiler that builds
   test/interp_decl.cpp, which includes this file. The X-macro lists at the
   end name every record and field the test compares sizeof and offsetof
   of. Valid C and C++ both, so no _Bool, no empty structs, no plain long. */
#include <stdint.h>
#include <stddef.h>

/* The runtime's, naturally aligned. */
typedef struct { uint8_t *top; } gs_stack;
typedef struct { uint8_t *base; uint64_t usable, size; } gs_region;

/* Codegen's, packed. */
#pragma pack(push, 1)
typedef struct { uint8_t *base; int64_t len; } gs_rhdr;
typedef struct { gs_rhdr *hdr; gs_stack *stk; } gs_rref;
typedef struct { gs_rhdr *hdr; gs_stack *stk; gs_rhdr *fl; gs_stack *flstk; } gs_pref;
typedef struct { uint8_t *data; int64_t len; } sl_u8;
typedef struct { double e[23]; } a23_f64;
typedef struct { uint32_t len; int32_t e[7]; } l7_i32;
typedef struct { uint8_t len; uint8_t e[255]; } l255_u8;
typedef struct { uint32_t cap; uint32_t len; int64_t e[1]; } rl_i64;

typedef struct Node_g Node_g;
struct Node_g {
    int32_t key_g;
    int32_t count_g;
    uint32_t left_g;
    uint32_t right_g;
};

typedef struct Item_g {
    uint8_t tag;
    int64_t id;
    double w;
    uint8_t flag;
    sl_u8 name;
    Node_g *node;
} Item_g;

struct Json_g_Num_p { double v_g; };
struct Json_g_Bool_p { uint8_t v_g; };
struct Json_g_Str_p { sl_u8 s; };
typedef struct {
    uint8_t tag;
    union {
        struct Json_g_Num_p v_Num_g;
        struct Json_g_Bool_p v_Bool_g;
        struct Json_g_Str_p v_Str_g;
    } u;
} Json_g;
typedef struct { Json_g e[3]; } a3_Json;
typedef struct { uint16_t tag; union { int32_t a; uint8_t b[5]; } u; } Wide_g;
typedef struct Pair_g { Item_g first; Item_g second; } Pair_g;
typedef struct { gs_rhdr argv_text_g; gs_stack gs_gstk_argv_text_g; gs_rhdr pool_g; } gs_globals_t;
#pragma pack(pop)

/* Natural alignment again, with padding to show. */
typedef struct { uint8_t a; int32_t b; uint8_t c; int64_t d; uint16_t e; } Natural;
typedef struct { uint8_t a; double d; } ByteDouble;
typedef struct { Natural n; uint8_t z; } NestedNatural;
typedef union { uint8_t b[3]; int32_t i; double d; } UnionNatural;
typedef struct { int32_t m[3][4]; int32_t *p[2]; int32_t (*pa)[4]; void (*f)(void *); } Arrays;
typedef struct { char name[16]; float xyz[3]; } Mixed;
typedef struct { int64_t n, link, idx[31], cnt[31]; } gs_spleaf;
typedef struct { uint8_t a; Natural *p; uint8_t b; } PointerAligned;

/* Nested packs. */
#pragma pack(push, 2)
typedef struct { uint8_t a; int32_t b; int64_t c; } Pack2;
#pragma pack(push, 4)
typedef struct { uint8_t a; int64_t c; } Pack4;
#pragma pack(pop)
typedef struct { Pack4 p4; uint8_t a; int64_t c; } Pack2Again;
#pragma pack(pop)
typedef struct { uint8_t a; Pack2 p; int32_t b; } HoldsPacked;

enum Color_g { RED_g, GREEN_g = 5, BLUE_g };
typedef struct { enum Color_g c; uint8_t x; } WithEnum;
typedef struct { struct { uint8_t t; int32_t v; } inner; uint8_t after; } Nested;
typedef uint8_t Buf16[16];
typedef struct { Buf16 b; uint32_t n; } WithTypedefArray;
typedef struct S1 S1;
struct S1 { S1 *next; S1 *prev; uint8_t v; };
typedef struct { size_t n; int8_t s; int16_t h; } Sized;

#define LAYOUT_SIZES(S) \
    S(gs_stack) S(gs_region) S(gs_rhdr) S(gs_rref) S(gs_pref) S(sl_u8) S(a23_f64) S(l7_i32) \
    S(l255_u8) S(rl_i64) S(Node_g) S(Item_g) S(Json_g) S(a3_Json) S(Wide_g) S(Pair_g) \
    S(gs_globals_t) S(Natural) S(ByteDouble) S(NestedNatural) S(UnionNatural) S(Arrays) \
    S(Mixed) S(gs_spleaf) S(PointerAligned) S(Pack2) S(Pack4) S(Pack2Again) S(HoldsPacked) \
    S(WithEnum) S(Nested) S(WithTypedefArray) S(S1) S(Sized)

#define LAYOUT_FIELDS(X) \
    X(gs_region, usable) X(gs_region, size) \
    X(gs_rhdr, base) X(gs_rhdr, len) X(gs_rref, stk) X(gs_pref, flstk) X(sl_u8, len) \
    X(l7_i32, e) X(l255_u8, e) X(rl_i64, len) X(rl_i64, e) \
    X(Node_g, count_g) X(Node_g, left_g) X(Node_g, right_g) \
    X(Item_g, id) X(Item_g, w) X(Item_g, flag) X(Item_g, name) X(Item_g, name.len) X(Item_g, node) \
    X(Json_g, u) X(Json_g, u.v_Num_g) X(Json_g, u.v_Bool_g.v_g) X(Json_g, u.v_Str_g.s.len) \
    X(a3_Json, e) X(Wide_g, u) X(Wide_g, u.b) X(Pair_g, second) X(Pair_g, second.node) \
    X(gs_globals_t, gs_gstk_argv_text_g) X(gs_globals_t, pool_g) X(gs_globals_t, pool_g.len) \
    X(Natural, b) X(Natural, c) X(Natural, d) X(Natural, e) X(ByteDouble, d) \
    X(NestedNatural, z) X(UnionNatural, d) \
    X(Arrays, p) X(Arrays, pa) X(Arrays, f) X(Mixed, xyz) X(gs_spleaf, cnt) \
    X(PointerAligned, p) X(PointerAligned, b) \
    X(Pack2, b) X(Pack2, c) X(Pack4, c) X(Pack2Again, a) X(Pack2Again, c) \
    X(HoldsPacked, p) X(HoldsPacked, p.c) X(HoldsPacked, b) \
    X(WithEnum, x) X(Nested, inner.v) X(Nested, after) X(WithTypedefArray, n) \
    X(S1, prev) X(S1, v) X(Sized, s) X(Sized, h)
