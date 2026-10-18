/* Integer arithmetic at every width: wrapping, promotions, signed and
   unsigned division, shifts, comparisons, conversions; and floating point
   with results shown as bits so float rounding is checked exactly. */
#include "inc/print.h"
typedef signed char i8;
typedef unsigned char u8;
typedef short i16;
typedef unsigned short u16;
typedef int i32;
typedef unsigned int u32;
typedef long long i64;
typedef unsigned long long u64;

static i32 g_i32 = -7;
static u32 g_u32 = 7;

int main(void) {
    i8 a8 = 127;
    u8 b8 = 255;
    i16 a16 = 32767;
    u16 b16 = 65535;
    i32 a32 = 2147483647;
    u32 b32 = 4294967295u;
    i64 a64 = 9223372036854775807LL;
    u64 b64 = 18446744073709551615ull;
    a8++; b8++; a16++; b16++; a32++; b32++; a64++; b64++;
    show("i8 wrap", a8);
    show("u8 wrap", b8);
    show("i16 wrap", a16);
    show("u16 wrap", b16);
    show("i32 wrap", a32);
    show_u("u32 wrap", b32);
    show("i64 wrap", a64);
    show_u("u64 wrap", b64);

    /* Promotions: narrow operands compute in int. */
    u8 x = 200, y = 100;
    show("u8 + u8 in int", x + y);
    show("u8 * u8 in int", x * y);
    u8 z = x + y;
    show("stored back to u8", z);
    i8 neg = -100;
    show("i8 - i8", neg - (i8)100);
    show("~u8 is int", ~b8);
    show("-u8 is int", -(i8)5);
    show("u16 << 4 in int", (u16)65535 << 4);
    show("char is signed", (char)200);

    /* Division and remainder, both signs. */
    show("-7 / 2", g_i32 / 2);
    show("-7 % 2", g_i32 % 2);
    show("7 / -2", 7 / -2);
    show("7 % -2", 7 % -2);
    show_u("7u / 2", g_u32 / 2);
    show_u("-1u / 2", (u32)-1 / 2);
    show("-9 >> 1", -9 >> 1);
    show_u("(u32)-9 >> 1", (u32)-9 >> 1);
    show("1 << 31", 1 << 31);
    show_u("1u << 31", 1u << 31);
    show("1LL << 40", 1LL << 40);
    show("-1 >> 63 (i64)", (i64)-1 >> 63);
    show_u("~0ull >> 63", ~0ull >> 63);

    /* Comparisons: signedness decides. */
    show("-1 < 0", -1 < 0);
    show("-1 < 0u", -1 < 0u);
    show("-1 < 0ull", -1 < 0ull);
    show("(i64)-1 < 0u", (i64)-1 < 0u);
    show("(u8)200 > 100", (u8)200 > 100);
    show("(i8)-56 > 100", (i8)-56 > 100);
    show("3 == 3", 3 == 3);
    show("3 != 3", 3 != 3);
    show("2 <= 2", 2 <= 2);
    show("2 >= 3", 2 >= 3);

    /* Conversions between widths and signedness. */
    i64 big = 0x123456789abcdefLL;
    show("(i32)big", (i32)big);
    show_u("(u32)big", (u32)big);
    show("(i16)big", (i16)big);
    show_u("(u16)big", (u16)big);
    show("(i8)big", (i8)big);
    show_u("(u8)big", (u8)big);
    show("(i64)(u32)-1", (i64)(u32)-1);
    show("(i64)(i32)-1", (i64)(i32)-1);
    show_u("(u64)(i32)-1", (u64)(i32)-1);
    _Bool t = 5, f = 0, tf = 0.5;
    show("_Bool 5", t);
    show("_Bool 0", f);
    show("_Bool 0.5", tf);
    show("!5", !5);
    show("!0", !0);
    show("!0.0", !0.0);
    show("!-0.0", !-0.0);
    show("!1.5", !1.5);

    /* Compound assignment converts through the operation type. */
    u8 c = 250;
    c += 10;
    show("u8 += 10", c);
    c -= 11;
    show("u8 -= 11", c);
    c *= 2;
    show("u8 *= 2", c);
    c /= 3;
    show("u8 /= 3", c);
    c %= 7;
    show("u8 %= 7", c);
    c <<= 7;
    show("u8 <<= 7", c);
    c >>= 3;
    show("u8 >>= 3", c);
    c |= 0x0f;
    c &= 0x3c;
    c ^= 0xff;
    show("u8 |= &= ^=", c);
    i32 d = 10;
    d += 2.7;
    show("int += 2.7", d);
    double e = 1;
    e += 2;
    e *= 3.5;
    show_f("double += *=", e);
    i64 k = 1;
    k <<= 40;
    show("i64 <<= 40", k);
    show("post ++", d++);
    show("after", d);
    show("pre --", --d);

    /* Floating point: binary results are exact, float rounds. */
    double da = 0.1, db = 0.2;
    show_f("0.1 + 0.2", da + db);
    show_f("1.0 / 3", 1.0 / 3);
    show_f("2 * 1e308", 2 * 1e308);
    show_f("-0.0", -0.0);
    show("nan == nan", (0.0 / 0.0) == (0.0 / 0.0));
    show("nan != nan", (0.0 / 0.0) != (0.0 / 0.0));
    show("1.5 < 2.5", 1.5 < 2.5);
    float fa = 0.1f, fb = 0.2f;
    float fs = fa + fb;
    print_str("0.1f + 0.2f bits = ");
    print_f32(fs);
    print_line();
    print_str("as double = ");
    print_f64(fs);
    print_line();
    float third = 1.0f / 3;
    print_str("1/3f bits = ");
    print_f32(third);
    print_line();
    float prod = 16777216.0f + 1.0f;
    print_str("2^24 + 1 in float = ");
    print_dec(prod);
    print_line();
    show_f("(double)(float)0.1", (double)(float)0.1);
    show_f("1e10f", 1e10f);
    show("(int)3.99", (int)3.99);
    show("(int)-3.99", (int)-3.99);
    show("(long long)1e18", (long long)1e18);
    show_u("(u64)1.8e19", (u64)1.8e19);
    show_u("(u32)4294967295.0", (u32)4294967295.0);
    show_u("(u8)255.9", (u8)255.9);
    show_f("(double)(u64)-1", (double)(u64)-1);
    show_f("(double)(i64)-1", (double)(i64)-1);
    show_f("(double)1 << 53 + 1", (double)((1LL << 53) + 1));
    show_f("(float)(1 << 24) + 1", (float)((1 << 24) + 1));
    /* An integer rounds to float once: 2^60 + 2^36 + 1 is above the tie
       and rounds up, where a double in between would round it to the tie
       and then down. */
    static i64 big60[1] = { 1152921573326323713LL };
    static u64 wide64[1] = { 18446744073709551615ULL };
    show_f("(float)(2^60 + 2^36 + 1)", (float)big60[0]);
    show_f("(float)(u64)-1", (float)wide64[0]);
    show_f("-x", -da);
    show_f("int / double", 7 / 2.0);
    show_f("int / int", 7 / 2);
    show_f("u64 / 2.0", b64 / 2.0);
    double dd = 2.5;
    dd++;
    show_f("double ++", dd);
    float ff = 1.0f;
    ff--;
    show_f("float --", ff);
    show("cond float", 0.0 ? 1 : 2);
    show("cond -0.0", -0.0 ? 1 : 2);
    show("cond 1e-300", 1e-300 ? 1 : 2);
    show("ternary type", (1 ? 3 : 2.5) == 3.0);
    return 0;
}
