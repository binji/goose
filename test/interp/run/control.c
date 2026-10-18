/* Control flow: if, loops, break, continue, switch with fallthrough and
   nesting, goto forward and back, labels after blocks, short circuits,
   the conditional operator, and the comma operator. */
#include "inc/print.h"

static int calls;
static int side(int v) { calls++; return v; }

static const char *classify(int c) {
    switch (c) {
        case 'a': case 'e': case 'i': case 'o': case 'u':
            return "vowel";
        case ' ':
            return "space";
        default:
            if (c >= '0' && c <= '9') return "digit";
            /* fallthrough into the next label is a goto in disguise */
        case '-':
            return "other";
    }
}

static int collatz(int n) {
    int steps = 0;
again:
    if (n == 1) goto done;
    if (n % 2) n = 3 * n + 1;
    else n /= 2;
    steps++;
    goto again;
done:
    return steps;
}

int main(void) {
    int i, sum;

    for (i = 0, sum = 0; i < 10; i++) {
        if (i % 2) continue;
        if (i > 6) break;
        sum += i;
    }
    show("for with continue and break", sum);
    show("i after", i);

    i = 0;
    while (1) {
        if (++i == 5) break;
    }
    show("while(1) break", i);

    i = 100;
    do { i++; } while (i < 10);
    show("do runs once", i);

    sum = 0;
    for (int k = 0; k < 3; k++)
        for (int l = 0; l < 3; l++) {
            if (l == 2) break;
            sum += k * 3 + l;
        }
    show("nested break", sum);

    show_s("classify a", classify('a'));
    show_s("classify 7", classify('7'));
    show_s("classify -", classify('-'));
    show_s("classify x", classify('x'));
    show_s("classify space", classify(' '));

    int fall = 0;
    switch (2) {
        case 1: fall += 1;
        case 2: fall += 2;
        case 3: fall += 4;
            break;
        case 4: fall += 8;
    }
    show("fallthrough", fall);

    int nested = 0;
    for (int k = 0; k < 4; k++) {
        switch (k) {
            case 0: continue;        /* continues the loop */
            case 1: nested += 10; break;
            default:
                switch (k) {
                    case 2: nested += 100; break;
                    case 3: nested += 1000; break;
                }
                break;
        }
        nested += 1;
    }
    show("switch in loop", nested);

    long long wide = 1LL << 40;
    switch (wide) {
        case 1LL << 40: show("switch on long long", 1); break;
        default: show("switch on long long", 0);
    }
    unsigned char uc = 255;
    switch (uc) {
        case 255: show("switch on unsigned char", 1); break;
        case -1: show("switch on unsigned char", -1); break;
    }
    switch (3) { default: show("switch default only", 3); }
    switch (3) { case 1: show("no match", 1); }

    show("collatz 27", collatz(27));
    show("collatz 1", collatz(1));

    calls = 0;
    int r = side(0) && side(1);
    show("&& short circuit", r);
    show("calls", calls);
    calls = 0;
    r = side(1) || side(2);
    show("|| short circuit", r);
    show("calls", calls);
    calls = 0;
    r = side(2) && side(3);
    show("&& both", r);
    show("calls", calls);
    r = side(0) || side(0);
    show("|| neither", r);
    double dz = 0;
    show("&& on doubles", 1.5 && 2.5);
    show("|| on doubles", dz || 0.0);
    const char *ps = "x";
    show("&& on pointers", ps && *ps);

    show("cond true", 1 ? 10 : 20);
    show("cond false", 0 ? 10 : 20);
    show("cond nested", 0 ? 1 : 2 ? 3 : 4);
    const char *which = calls > 100 ? "many" : "few";
    show_s("cond pointers", which);
    show("comma", (side(1), side(2), 7));
    int cx = 0, cy = 0;
    for (cx = 0, cy = 10; cx < cy; cx++, cy--) {}
    show("comma in for", cx);

    goto skip;
    show("skipped", 0);
skip:
    {
        int blockvar = 5;
        show("after goto", blockvar);
    }
    if (0) ; else show("empty then", 1);
    if (1) show("no else", 1);
    return 0;
}
