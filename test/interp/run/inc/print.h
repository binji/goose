/* Printing for the run fixtures, in C the interpreter runs: the only
   library call is fwrite, so every digit here is interpreted code. Floats
   print as their bits, which is exact, and as a rough decimal. */
typedef struct FILE FILE;
extern FILE *stdout;
unsigned long fwrite(const void *p, unsigned long size, unsigned long n, FILE *f);
void *memcpy(void *d, const void *s, unsigned long n);

static void print_str(const char *s) {
    unsigned long n = 0;
    while (s[n]) n++;
    fwrite(s, 1, n, stdout);
}
static void print_char(char c) { fwrite(&c, 1, 1, stdout); }
static void print_u64(unsigned long long v) {
    char buf[24];
    int i = 24;
    do {
        buf[--i] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    fwrite(buf + i, 1, (unsigned long)(24 - i), stdout);
}
static void print_i64(long long v) {
    if (v < 0) {
        print_char('-');
        print_u64(0 - (unsigned long long)v);
    } else {
        print_u64((unsigned long long)v);
    }
}
static void print_hex64(unsigned long long v) {
    const char *digits = "0123456789abcdef";
    char buf[16];
    for (int i = 15; i >= 0; i--) {
        buf[i] = digits[v & 15];
        v >>= 4;
    }
    fwrite(buf, 1, 16, stdout);
}
static void print_f64(double d) {
    unsigned long long bits;
    memcpy(&bits, &d, 8);
    print_hex64(bits);
}
static void print_f32(float f) {
    unsigned int bits;
    memcpy(&bits, &f, 4);
    print_hex64(bits);
}
/* Six decimals, truncated; enough to read a value, not to compare one. */
static void print_dec(double d) {
    if (d < 0) {
        print_char('-');
        d = -d;
    }
    unsigned long long whole = (unsigned long long)d;
    print_u64(whole);
    print_char('.');
    double frac = d - (double)whole;
    for (int i = 0; i < 6; i++) {
        frac *= 10;
        int digit = (int)frac;
        print_char((char)('0' + digit));
        frac -= digit;
    }
}
static void print_line(void) { print_char('\n'); }
static void show(const char *label, long long v) {
    print_str(label);
    print_str(" = ");
    print_i64(v);
    print_line();
}
static void show_u(const char *label, unsigned long long v) {
    print_str(label);
    print_str(" = ");
    print_u64(v);
    print_line();
}
static void show_f(const char *label, double v) {
    print_str(label);
    print_str(" = ");
    print_dec(v);
    print_str(" (");
    print_f64(v);
    print_str(")");
    print_line();
}
static void show_s(const char *label, const char *s) {
    print_str(label);
    print_str(" = ");
    print_str(s);
    print_line();
}
