/* Tokens: pp-numbers, strings and character constants with escapes, every
   punctuator, comments as whitespace, line splices, and #pragma lines. */
int a = 0x1Full, b = 1.5e+3, c = .5, d = 1e-3f, e = 0x1p-2, f = 123456789012345678901234567890;
char *s = "str // not a comment", *t = "/* nor this */", *u = "esc \" \\ \n";
char q = '\'', r = '"', v = '\\', w = '\x41', x = '\0';
int y = a... ; a <<= 1; a >>= 1; p->q; a++; a--; a << 1; a >> 1; a <= b; a >= b; a == b; a != b;
a && b; a || b; a *= 1; a /= 1; a %= 1; a += 1; a -= 1; a &= 1; a ^= 1; a |= 1;
( ) [ ] { } . & * + - ~ ! / % < > ^ | ? : ; = ,
int z = 1 /* a comment
   over two
   lines */ + 2;
int aa = 3 // to the end
    + 4;
int bb = 5 \
    + 6;
#pragma pack(push, 1)
struct s { char c; int i; };
#pragma pack(pop)
#pragma GCC optimize("fp-contract=off")
# pragma   once
/* A null directive, and a directive with leading space. */
#
   # define INDENTED 1
int cc = INDENTED;
/* Other characters pass through one at a time. */
int dd = @ ` $;
/* Lines are numbered physically after a splice and a block comment. */
int ee = __LINE__;
