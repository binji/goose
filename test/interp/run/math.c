/* The math library: results printed as bits where they are exact on every
   libm, and rounded to six decimals where they are transcendental. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "inc/print.h"

static void near(const char *label, double v) {
    print_str(label);
    print_str(" = ");
    print_dec(v);
    print_line();
}

int main(void) {
    show_f("sqrt(2)", sqrt(2.0));
    show_f("sqrt(1e10)", sqrt(1e10));
    show_f("sqrtf(2)", sqrtf(2.0f));
    near("cbrt(27)", cbrt(27.0));
    show_f("pow(2, 10)", pow(2.0, 10.0));
    near("pow(2, 0.5)", pow(2.0, 0.5));
    near("hypot(3, 4)", hypot(3.0, 4.0));
    show_f("fmod(7.5, 2)", fmod(7.5, 2.0));
    show_f("fmod(-7.5, 2)", fmod(-7.5, 2.0));
    show_f("fmodf(7.5, 2)", fmodf(7.5f, 2.0f));
    show_f("trunc(-2.7)", trunc(-2.7));
    show_f("floor(-2.7)", floor(-2.7));
    show_f("ceil(-2.7)", ceil(-2.7));
    show_f("round(2.5)", round(2.5));
    show_f("round(-2.5)", round(-2.5));
    show_f("rint(2.5)", rint(2.5));
    show_f("floorf(2.7)", floorf(2.7f));
    show_f("ceilf(2.2)", ceilf(2.2f));
    show_f("truncf(-2.7)", truncf(-2.7f));
    show_f("fabs(-3)", fabs(-3.0));
    show_f("fabsf(-3)", fabsf(-3.0f));
    show_f("fmin(1, 2)", fmin(1.0, 2.0));
    show_f("fmax(1, 2)", fmax(1.0, 2.0));
    show_f("copysign(3, -1)", copysign(3.0, -1.0));
    show_f("exp(0)", exp(0.0));
    show_f("log(1)", log(1.0));
    near("log2(1024)", log2(1024.0));
    near("log10(1000)", log10(1000.0));
    near("exp2(10)", exp2(10.0));
    show_f("sin(0)", sin(0.0));
    show_f("cos(0)", cos(0.0));
    show_f("atan2(0, 1)", atan2(0.0, 1.0));
    show_f("ldexp(1.5, 4)", ldexp(1.5, 4));
    show_f("ldexpf(1.5, 4)", ldexpf(1.5f, 4));
    int e = 0;
    double m = frexp(48.0, &e);
    show_f("frexp(48) mantissa", m);
    show("frexp(48) exponent", e);
    float mf = frexpf(0.375f, &e);
    show_f("frexpf(0.375) mantissa", mf);
    show("frexpf(0.375) exponent", e);
    double ip = 0;
    double frac = modf(3.75, &ip);
    show_f("modf(3.75) frac", frac);
    show_f("modf(3.75) int", ip);

    near("sin(1)", sin(1.0));
    near("cos(1)", cos(1.0));
    near("tan(1)", tan(1.0));
    near("asin(0.5)", asin(0.5));
    near("acos(0.5)", acos(0.5));
    near("atan(1)", atan(1.0));
    near("sinh(1)", sinh(1.0));
    near("cosh(1)", cosh(1.0));
    near("tanh(1)", tanh(1.0));
    near("exp(1)", exp(1.0));
    near("log(10)", log(10.0));
    near("expm1(1e-5)", expm1(1e-5) * 1e5);
    near("log1p(1e-5)", log1p(1e-5) * 1e5);
    near("sinf(1)", sinf(1.0f));
    near("powf(2, 0.5)", powf(2.0f, 0.5f));
    near("M_PI", M_PI);

    double inf = INFINITY, nan = NAN;
    show("isinf(inf)", isinf(inf));
    show("isinf(-inf)", isinf(-inf));
    show("isinf(1)", isinf(1.0));
    show("isnan(nan)", isnan(nan));
    show("isnan(1)", isnan(1.0));
    show("isfinite(1)", isfinite(1.0));
    show("isfinite(inf)", isfinite(inf));
    show("signbit(-0.0)", signbit(-0.0));
    show("signbit(1)", signbit(1.0));
    show("inf > 1e308", inf > 1e308);
    show("HUGE_VAL == inf", HUGE_VAL == inf);
    show("nan == nan", nan == nan);
    show("nan != nan", nan != nan);
    show("isinf(sqrt(-1)) == 0 and isnan", isinf(sqrt(-1.0)) == 0 && isnan(sqrt(-1.0)));
    show("abs(-5)", abs(-5));
    show("labs(-5000000000)", labs(-5000000000L));
    show("llabs(INT64_MIN + 1)", llabs(-9223372036854775807LL));
    return 0;
}
