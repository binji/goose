// The interpreter's test package (docs/design/interpreter.md 7.1, step 9
// of 14): a native part with an extern fn of every parameter shape the
// foreign boundary admits, which test/packages/pkg_interp_thunks.goose
// calls. Natively a
// program links or loads the C; the interpreter calls it through the
// thunks generated from include/interptest_api.h.
namespace interptest;

struct Handle { id: u32 }
struct Pair { a: i32, b: i32 }
struct Item { tag: u8, value: i64, weight: f32 }

extern "it_scalars" fn scalars(a: i8, b: u8, c: i16, d: u16, e: i32, f: u32, g: i64, h: u64,
                               x: f32, y: f64, flag: bool) -> i64;
extern "it_sum_bytes" fn sum_bytes(data: const u8[:]) -> i64;
extern "it_fill_bytes" fn fill_bytes(out: u8[:], value: u8) -> i64;
extern "it_sum_items" fn sum_items(items: const Item[:]) -> f64;
extern "it_make_handle" fn make_handle(id: u32) -> Handle;
extern "it_handle_id" fn handle_id(h: Handle) -> u32;
extern "it_pair_swap" fn pair_swap(p: Pair) -> Pair;
extern "it_pair_out" fn pair_out(a: i32, b: i32, out: Pair&);
extern "it_pair_in" fn pair_in(p: const Pair&) -> i64;
extern "it_item_scale" fn item_scale(item: Item&, k: f32);
extern "it_counter" fn counter() -> i64;
extern "it_yield" fn yield_now();
extern "it_many" fn many(a: i64, b: i64, c: i64, d: i64, e: i64, f: i64, g: i64, h: i64,
                         i: i64, j: i64) -> i64;
extern "it_nothing" fn nothing();
extern "it_half" fn half(x: f32) -> f32;
extern "it_bytes_equal" fn bytes_equal(a: const u8[:], b: const u8[:]) -> bool;
