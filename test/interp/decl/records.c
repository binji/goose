/* Records and enums: forward tags, self-reference, members through
   typedefs, unions, nested unnamed records, packs, and enumerators as
   constants. */
#include <stdint.h>
struct Fwd;
struct Fwd *fwdp;
typedef struct Fwd Fwd;
struct Fwd { Fwd *next; int v; };
#pragma pack(push, 1)
typedef struct { uint8_t *base; int64_t len; } gs_rhdr;
typedef struct Node_g Node_g;
struct Node_g { int32_t key_g; uint32_t left_g; Node_g *parent; gs_rhdr items; };
enum { Op_Push_k = 0, Op_Pop_k = 1, Op_Add_k = 2 };
typedef struct { uint8_t tag; union { struct { int64_t v; } v_Push; struct { } v_Pop; } u; } Op_g;
#pragma pack(pop)
enum Color { RED, GREEN = 5, BLUE, BLACK = BLUE * 2 };
enum Color color = GREEN;
int constants[BLACK];
struct Natural { uint8_t a; int32_t b; double c; };
union U { uint8_t b[3]; int32_t i; struct Natural n; };
struct Nested { struct { uint8_t t; int32_t v; } inner; union U u; };
struct WithArrays { int32_t m[2][3]; struct Natural ns[2]; };
struct Natural natural;
union U u;
