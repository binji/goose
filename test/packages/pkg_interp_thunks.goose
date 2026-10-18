// Every parameter shape across the foreign boundary, through the
// interpreter's test package (pkgs/interptest): natively the C is linked
// or loaded; under --interp each call goes through a generated thunk,
// which lays the arguments out, decodes the pointers and stores the result.
import std;
import interptest from "./pkgs/interptest";

fn main() {
    print(interptest::scalars(-5, 200, -300, 60000, -70000, 4000000000, -1, 10, 1.5, 0.25, true));
    let bytes: u8[] = "hello";
    print(interptest::sum_bytes(bytes));
    var buf: u8[8] = [0, 0, 0, 0, 0, 0, 0, 0];
    print(interptest::fill_bytes(buf, 100), " ", buf[0], " ", buf[7]);
    let items: interptest::Item[3] = [
        interptest::Item { tag: 1, value: 10, weight: 0.5 },
        interptest::Item { tag: 2, value: 20, weight: 0.25 },
        interptest::Item { tag: 3, value: 30, weight: 0.125 },
    ];
    print(interptest::sum_items(items));
    let h = interptest::make_handle(21);
    print(h.id, " ", interptest::handle_id(h));
    let p = interptest::pair_swap(interptest::Pair { a: 1, b: 2 });
    print(p.a, " ", p.b);
    var out = interptest::Pair { a: 0, b: 0 };
    interptest::pair_out(3, 4, out);
    print(out.a, " ", out.b, " ", interptest::pair_in(out));
    var item = interptest::Item { tag: 7, value: 21, weight: 3.0 };
    interptest::item_scale(item, 0.5);
    print(item.tag, " ", item.value, " ", item.weight);
    interptest::yield_now();
    interptest::nothing();
    print(interptest::many(1, 2, 3, 4, 5, 6, 7, 8, 9, 10));
    print(interptest::half(3.0));
    print(interptest::bytes_equal("same", "same"), " ", interptest::bytes_equal("same", "other"));
    print("calls ", interptest::counter());
}
