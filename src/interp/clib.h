// Goose compiler — the interpreter's C library (docs/design/interpreter.md
// §6, step 4 of §14). The table of adapters the VM binds the prelude's
// declarations to: each decodes its pointer arguments by the function's own
// contract (memcpy its n bytes, strlen up to the NUL, a FILE * as a stream
// the VM opened) and calls the host's library, keeping no decoded pointer
// past its return. The library is the one the interpreter is linked with,
// glibc natively and wasi-libc in the browser, so files and the clock are
// whatever the host gives. fprintf and snprintf are written here over the
// conversions the runtime uses rather than passed through, since a variadic
// call cannot be forwarded to the host. qsort is C in the prelude, as its
// comparison is interpreted.
#pragma once

#include <math.h>
#include <time.h>

#include <chrono>
#ifndef _WIN32
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace goose {
namespace interp {

// ---- Helpers

inline double D(uint64_t v) { return bit_cast<double>(v); }
inline uint64_t Bits(double d) { return bit_cast<uint64_t>(d); }
inline uint64_t F32Bits(double d) { return bit_cast<uint64_t>((double)(float)d); }
inline uint64_t Int(int64_t v) { return (uint64_t)v; }

// Up to `max` bytes at a string that may lack a NUL within them (`%.*s`,
// strncmp): what is there, stopping at a NUL or the segment's end.
inline string_view Bytes(VM &vm, uint64_t h, uint64_t max) {
    auto &s = vm.Seg(h);
    auto off = OffOf(h);
    if (!s.alive) throw Fault { cat("use after free: string at offset ", off, " of a ", vm.Describe(s)) };
    if (off > s.size) throw Fault { cat("string at offset ", off, " past the end of ", vm.Describe(s)) };
    auto p = (const char *)s.mem.data() + off;
    auto n = (size_t)min<uint64_t>(max, s.size - off);
    auto end = (const char *)memchr(p, 0, n);
    return string_view(p, end ? (size_t)(end - p) : n);
}

// The host's errno value for one of the prelude's.
inline int HostErrno(int e) {
    switch (e) {
        case 1: return EPERM;
        case 2: return ENOENT;
        case 4: return EINTR;
        case 5: return EIO;
        case 9: return EBADF;
        case 11: return EAGAIN;
        case 12: return ENOMEM;
        case 13: return EACCES;
        case 17: return EEXIST;
        case 20: return ENOTDIR;
        case 21: return EISDIR;
        case 22: return EINVAL;
        case 24: return EMFILE;
        case 28: return ENOSPC;
        case 34: return ERANGE;
        case 39: return ENOTEMPTY;
        default: return e;
    }
}

inline string HexOf(uint64_t v) {
    char buf[32];
    snprintf(buf, sizeof buf, "%llx", (unsigned long long)v);
    return buf;
}

template <double (*F)(double)>
uint64_t Math1(VM &, const uint64_t *a, int) { return Bits(F(D(a[0]))); }
template <double (*F)(double)>
uint64_t Math1f(VM &, const uint64_t *a, int) { return F32Bits(F(D(a[0]))); }
template <double (*F)(double, double)>
uint64_t Math2(VM &, const uint64_t *a, int) { return Bits(F(D(a[0]), D(a[1]))); }
template <double (*F)(double, double)>
uint64_t Math2f(VM &, const uint64_t *a, int) { return F32Bits(F(D(a[0]), D(a[1]))); }

// ---- printf

// A format string in a diagnostic, its newlines escaped.
inline string Quoted(string_view s) {
    string q = "\"";
    for (char c : s) {
        if (c == '\n') q += "\\n";
        else if (c == '\t') q += "\\t";
        else if (c == '"') q += "\\\"";
        else q += c;
    }
    return q + "\"";
}

// Formats as printf would, one conversion at a time through the host's
// snprintf with the argument decoded by the conversion's type: `%d` is an
// int in the slot, `%lld`, `%ld` and `%zu` a 64-bit one, `%s` a handle,
// `%f` a double. Faults on a conversion the table lacks or an argument the
// format does not have.
inline string CFormat(VM &vm, string_view fmt, const uint64_t *args, int nargs) {
    string out;
    int ai = 0;
    auto next = [&]() -> uint64_t {
        if (ai >= nargs)
            throw Fault { cat("format ", Quoted(fmt), " needs more than the ", nargs, " argument", nargs == 1 ? "" : "s", " given") };
        return args[ai++];
    };
    auto put = [&](const string &spec, auto value) {
        char buf[512];
        int n = snprintf(buf, sizeof buf, spec.c_str(), value);
        if (n < 0) throw Fault { cat("bad conversion ", Quoted(spec), " in format ", Quoted(fmt)) };
        if ((size_t)n < sizeof buf) { out.append(buf, (size_t)n); return; }
        vector<char> big((size_t)n + 1);
        snprintf(big.data(), big.size(), spec.c_str(), value);
        out.append(big.data(), (size_t)n);
    };
    for (size_t i = 0; i < fmt.size(); i++) {
        char c = fmt[i];
        if (c != '%') { out += c; continue; }
        if (++i >= fmt.size()) throw Fault { cat("format ", Quoted(fmt), " ends in %") };
        string spec = "%";
        while (i < fmt.size() && strchr("-+ #0", fmt[i])) spec += fmt[i++];
        if (i < fmt.size() && fmt[i] == '*') {
            auto w = (int32_t)next();
            if (w < 0) { spec += '-'; w = -w; }
            Append(spec, w);
            i++;
        } else {
            while (i < fmt.size() && isdigit((unsigned char)fmt[i])) spec += fmt[i++];
        }
        int prec = -1;
        if (i < fmt.size() && fmt[i] == '.') {
            i++;
            if (i < fmt.size() && fmt[i] == '*') {
                prec = (int32_t)next();
                i++;
            } else {
                prec = 0;
                while (i < fmt.size() && isdigit((unsigned char)fmt[i])) prec = prec * 10 + (fmt[i++] - '0');
            }
            if (prec >= 0) Append(spec, ".", prec);
        }
        // The length: 0 for int, 1 for the 64-bit ones, -1 and -2 for hh and h.
        int len = 0;
        if (i < fmt.size()) {
            switch (fmt[i]) {
                case 'h':
                    len = -2;
                    i++;
                    if (i < fmt.size() && fmt[i] == 'h') { len = -1; i++; }
                    break;
                case 'l':
                    len = 1;
                    i++;
                    if (i < fmt.size() && fmt[i] == 'l') i++;
                    break;
                case 'z': case 'j': case 't': len = 1; i++; break;
                case 'L': i++; break;
                default: break;
            }
        }
        if (i >= fmt.size()) throw Fault { cat("format ", Quoted(fmt), " ends inside a conversion") };
        char conv = fmt[i];
        switch (conv) {
            case 'd': case 'i': {
                auto v = next();
                int64_t x = len == -1 ? (int8_t)v : len == -2 ? (int16_t)v : len == 0 ? (int32_t)v : (int64_t)v;
                put(spec + "lld", (long long)x);
                break;
            }
            case 'u': case 'o': case 'x': case 'X': {
                auto v = next();
                uint64_t x = len == -1 ? (uint8_t)v : len == -2 ? (uint16_t)v : len == 0 ? (uint32_t)v : v;
                put(cat(spec, "ll", conv), (unsigned long long)x);
                break;
            }
            case 'c':
                put(spec + "c", (int)(int32_t)next());
                break;
            case 's': {
                auto h = next();
                string s = h == 0 ? string("(null)")
                         : prec >= 0 ? string(Bytes(vm, h, (uint64_t)prec)) : vm.CStr(h);
                put(spec + "s", s.c_str());
                break;
            }
            case 'p': {
                auto h = next();
                put(spec + "s", cat("0x", HexOf(h)).c_str());
                break;
            }
            case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A':
                put(spec + conv, D(next()));
                break;
            case '%':
                out += '%';
                break;
            default:
                throw Fault { cat("unsupported conversion '%", conv, "' in format ", Quoted(fmt)) };
        }
    }
    return out;
}

// strtol and friends: the text, the base, and the end pointer stored back
// as a handle into the text.
inline uint64_t StrTo(VM &vm, const uint64_t *a, int base, bool uns) {
    auto s = vm.CStr(a[0]);
    char *end = nullptr;
    errno = 0;
    uint64_t r = uns ? (uint64_t)strtoull(s.c_str(), &end, base) : (uint64_t)strtoll(s.c_str(), &end, base);
    if (errno) vm.SetErrno(errno);
    if (a[1]) VM::StoreVal(vm.Access(a[1], 8), VT_U64, vm.PtrAdd(a[0], end - s.c_str()));
    return r;
}

// snprintf's store: up to n - 1 bytes and a NUL; returns the full length.
inline uint64_t StoreFormatted(VM &vm, uint64_t dst, uint64_t n, const string &s) {
    if (n) {
        auto k = min<uint64_t>(s.size(), n - 1);
        auto p = vm.Access(dst, k + 1);
        memcpy(p, s.data(), (size_t)k);
        p[k] = 0;
    }
    return s.size();
}

// ---- The table

inline void VM::Library() {
    // -- Memory and strings
    library["memcpy"] = [](VM &vm, const uint64_t *a, int) -> uint64_t {
        if (a[2]) {
            vm.Access(a[0], a[2]);
            auto s = vm.Access(a[1], a[2]);
            memmove(vm.Access(a[0], a[2]), s, (size_t)a[2]);
        }
        return a[0];
    };
    library["memmove"] = library["memcpy"];
    library["memset"] = [](VM &vm, const uint64_t *a, int) -> uint64_t {
        if (a[2]) memset(vm.Access(a[0], a[2]), (int)a[1], (size_t)a[2]);
        return a[0];
    };
    library["memcmp"] = [](VM &vm, const uint64_t *a, int) -> uint64_t {
        if (!a[2]) return 0;
        auto x = vm.Access(a[0], a[2]);
        auto y = vm.Access(a[1], a[2]);
        int r = memcmp(x, y, (size_t)a[2]);
        return Int(r < 0 ? -1 : r > 0 ? 1 : 0);
    };
    library["memchr"] = [](VM &vm, const uint64_t *a, int) -> uint64_t {
        if (!a[2]) return 0;
        auto p = vm.Access(a[0], a[2]);
        auto q = (const uint8_t *)memchr(p, (int)a[1], (size_t)a[2]);
        return q ? vm.PtrAdd(a[0], q - p) : 0;
    };
    library["strlen"] = [](VM &vm, const uint64_t *a, int) -> uint64_t { return vm.CString(a[0]).size(); };
    library["strcmp"] = [](VM &vm, const uint64_t *a, int) -> uint64_t {
        int r = vm.CString(a[0]).compare(vm.CString(a[1]));
        return Int(r < 0 ? -1 : r > 0 ? 1 : 0);
    };
    library["strncmp"] = [](VM &vm, const uint64_t *a, int) -> uint64_t {
        if (!a[2]) return 0;
        int r = Bytes(vm, a[0], a[2]).compare(Bytes(vm, a[1], a[2]));
        return Int(r < 0 ? -1 : r > 0 ? 1 : 0);
    };
    library["strchr"] = [](VM &vm, const uint64_t *a, int) -> uint64_t {
        auto s = vm.CString(a[0]);
        char c = (char)a[1];
        if (c == 0) return vm.PtrAdd(a[0], (int64_t)s.size());
        auto at = s.find(c);
        return at == string_view::npos ? 0 : vm.PtrAdd(a[0], (int64_t)at);
    };
    library["strrchr"] = [](VM &vm, const uint64_t *a, int) -> uint64_t {
        auto s = vm.CString(a[0]);
        char c = (char)a[1];
        if (c == 0) return vm.PtrAdd(a[0], (int64_t)s.size());
        auto at = s.rfind(c);
        return at == string_view::npos ? 0 : vm.PtrAdd(a[0], (int64_t)at);
    };
    library["strstr"] = [](VM &vm, const uint64_t *a, int) -> uint64_t {
        auto s = vm.CString(a[0]);
        auto at = s.find(vm.CString(a[1]));
        return at == string_view::npos ? 0 : vm.PtrAdd(a[0], (int64_t)at);
    };
    library["strcpy"] = [](VM &vm, const uint64_t *a, int) -> uint64_t {
        auto s = vm.CStr(a[1]);
        memcpy(vm.Access(a[0], s.size() + 1), s.c_str(), s.size() + 1);
        return a[0];
    };
    library["strncpy"] = [](VM &vm, const uint64_t *a, int) -> uint64_t {
        if (!a[2]) return a[0];
        auto s = string(Bytes(vm, a[1], a[2]));
        auto p = vm.Access(a[0], a[2]);
        memcpy(p, s.data(), s.size());
        memset(p + s.size(), 0, (size_t)(a[2] - s.size()));
        return a[0];
    };
    library["strcat"] = [](VM &vm, const uint64_t *a, int) -> uint64_t {
        auto n = vm.CString(a[0]).size();
        auto s = vm.CStr(a[1]);
        memcpy(vm.Access(a[0], n + s.size() + 1) + n, s.c_str(), s.size() + 1);
        return a[0];
    };
    library["strdup"] = [](VM &vm, const uint64_t *a, int) -> uint64_t {
        auto s = vm.CString(a[0]);
        auto id = vm.NewSeg(SEG_HEAP, "", s.size() + 1);
        memcpy(vm.segs[id].mem.data(), s.data(), s.size());
        return Handle(id, 0);
    };
    library["strerror"] = [](VM &vm, const uint64_t *a, int) -> uint64_t {
        return vm.NewString("strerror", strerror(HostErrno((int)(int32_t)a[0])));
    };

    // -- Allocation
    library["malloc"] = [](VM &vm, const uint64_t *a, int) -> uint64_t {
        return Handle(vm.NewSeg(SEG_HEAP, "", a[0]), 0);
    };
    library["calloc"] = [](VM &vm, const uint64_t *a, int) -> uint64_t {
        return Handle(vm.NewSeg(SEG_HEAP, "", a[0] * a[1]), 0);
    };
    library["realloc"] = [](VM &vm, const uint64_t *a, int) -> uint64_t {
        auto id = vm.NewSeg(SEG_HEAP, "", a[1]);
        if (a[0]) {
            auto &old = vm.Seg(a[0]);
            if (old.kind != SEG_HEAP || !old.alive || OffOf(a[0]) != 0)
                throw Fault { cat("realloc of ", vm.Describe(old)) };
            memcpy(vm.segs[id].mem.data(), old.mem.data(), (size_t)min(old.size, a[1]));
            vm.Free(a[0]);
        }
        return Handle(id, 0);
    };
    library["free"] = [](VM &vm, const uint64_t *a, int) -> uint64_t {
        if (a[0]) vm.Free(a[0]);
        return 0;
    };

    // -- Process
    library["exit"] = [](VM &, const uint64_t *a, int) -> uint64_t { throw Exit { (int)a[0] }; };
    library["_exit"] = library["exit"];
    library["_Exit"] = library["exit"];
    library["abort"] = [](VM &, const uint64_t *, int) -> uint64_t { throw Fault { "abort() called" }; };
    library["getenv"] = [](VM &vm, const uint64_t *a, int) -> uint64_t {
        auto v = getenv(vm.CStr(a[0]).c_str());
        return v ? vm.NewString("getenv", v) : 0;
    };
    library["__errno_location"] = [](VM &vm, const uint64_t *, int) -> uint64_t { return vm.errnoh; };
    library["setvbuf"] = [](VM &, const uint64_t *, int) -> uint64_t { return 0; };

    // -- Numbers from text
    library["abs"] = [](VM &, const uint64_t *a, int) -> uint64_t { return Int(llabs((int32_t)a[0])); };
    library["labs"] = [](VM &, const uint64_t *a, int) -> uint64_t { return Int(llabs((int64_t)a[0])); };
    library["llabs"] = library["labs"];
    library["strtol"] = [](VM &vm, const uint64_t *a, int) -> uint64_t { return StrTo(vm, a, (int)(int32_t)a[2], false); };
    library["strtoll"] = library["strtol"];
    library["strtoul"] = [](VM &vm, const uint64_t *a, int) -> uint64_t { return StrTo(vm, a, (int)(int32_t)a[2], true); };
    library["strtoull"] = library["strtoul"];
    library["atoi"] = [](VM &vm, const uint64_t *a, int) -> uint64_t {
        uint64_t none[2] = { a[0], 0 };
        return Int((int32_t)StrTo(vm, none, 10, false));
    };
    library["atol"] = [](VM &vm, const uint64_t *a, int) -> uint64_t {
        uint64_t none[2] = { a[0], 0 };
        return StrTo(vm, none, 10, false);
    };
    library["strtod"] = [](VM &vm, const uint64_t *a, int) -> uint64_t {
        auto s = vm.CStr(a[0]);
        char *end = nullptr;
        errno = 0;
        double d = strtod(s.c_str(), &end);
        if (errno) vm.SetErrno(errno);
        if (a[1]) VM::StoreVal(vm.Access(a[1], 8), VT_U64, vm.PtrAdd(a[0], end - s.c_str()));
        return Bits(d);
    };
    library["atof"] = [](VM &vm, const uint64_t *a, int) -> uint64_t {
        uint64_t none[2] = { a[0], 0 };
        return vm.library["strtod"](vm, none, 2);
    };

    // -- Math: the double functions, and their float forms as the double
    // function rounded, which is how the VM does float arithmetic anyway.
#define GS_MATH1(name) \
    library[#name] = Math1<static_cast<double (*)(double)>(::name)>; \
    library[#name "f"] = Math1f<static_cast<double (*)(double)>(::name)>;
#define GS_MATH2(name) \
    library[#name] = Math2<static_cast<double (*)(double, double)>(::name)>; \
    library[#name "f"] = Math2f<static_cast<double (*)(double, double)>(::name)>;
    GS_MATH1(sqrt) GS_MATH1(cbrt)
    GS_MATH1(sin) GS_MATH1(cos) GS_MATH1(tan)
    GS_MATH1(asin) GS_MATH1(acos) GS_MATH1(atan)
    GS_MATH1(sinh) GS_MATH1(cosh) GS_MATH1(tanh)
    GS_MATH1(exp) GS_MATH1(exp2) GS_MATH1(expm1)
    GS_MATH1(log) GS_MATH1(log2) GS_MATH1(log10) GS_MATH1(log1p)
    GS_MATH1(fabs) GS_MATH1(trunc) GS_MATH1(floor) GS_MATH1(ceil)
    GS_MATH1(round) GS_MATH1(rint) GS_MATH1(nearbyint)
    GS_MATH2(pow) GS_MATH2(fmod) GS_MATH2(atan2)
    GS_MATH2(hypot) GS_MATH2(fmin) GS_MATH2(fmax) GS_MATH2(copysign)
#undef GS_MATH1
#undef GS_MATH2
    library["frexp"] = [](VM &vm, const uint64_t *a, int) -> uint64_t {
        int e = 0;
        double r = frexp(D(a[0]), &e);
        VM::StoreVal(vm.Access(a[1], 4), VT_I32, Int(e));
        return Bits(r);
    };
    library["frexpf"] = [](VM &vm, const uint64_t *a, int) -> uint64_t {
        return F32Bits(D(vm.library["frexp"](vm, a, 2)));
    };
    library["ldexp"] = [](VM &, const uint64_t *a, int) -> uint64_t { return Bits(ldexp(D(a[0]), (int)(int32_t)a[1])); };
    library["ldexpf"] = [](VM &, const uint64_t *a, int) -> uint64_t { return F32Bits(ldexp(D(a[0]), (int)(int32_t)a[1])); };
    library["modf"] = [](VM &vm, const uint64_t *a, int) -> uint64_t {
        double i = 0;
        double r = modf(D(a[0]), &i);
        VM::StoreVal(vm.Access(a[1], 8), VT_F64, Bits(i));
        return Bits(r);
    };
    library["modff"] = [](VM &vm, const uint64_t *a, int) -> uint64_t {
        double i = 0;
        double r = modf(D(a[0]), &i);
        VM::StoreVal(vm.Access(a[1], 4), VT_F32, Bits(i));
        return F32Bits(r);
    };
    library["isnan"] = [](VM &, const uint64_t *a, int) -> uint64_t { return std::isnan(D(a[0])); };
    library["isinf"] = [](VM &, const uint64_t *a, int) -> uint64_t { return std::isinf(D(a[0])); };
    library["isfinite"] = [](VM &, const uint64_t *a, int) -> uint64_t { return std::isfinite(D(a[0])); };
    library["signbit"] = [](VM &, const uint64_t *a, int) -> uint64_t { return std::signbit(D(a[0])); };

    // -- Streams
    library["fopen"] = [](VM &vm, const uint64_t *a, int) -> uint64_t {
        auto path = vm.CStr(a[0]), mode = vm.CStr(a[1]);
        errno = 0;
        auto f = fopen(path.c_str(), mode.c_str());
        if (!f) { vm.SetErrno(errno); return 0; }
        auto h = Handle(vm.NewSeg(SEG_STATIC, "a stream", 1), 0);
        vm.streams[h].file = f;
        return h;
    };
    library["fclose"] = [](VM &vm, const uint64_t *a, int) -> uint64_t {
        auto &st = vm.StreamOf(a[0]);
        int r = 0;
        if (st.file) r = fclose(st.file) ? -1 : 0;
        if (r) vm.SetErrno(errno);
        vm.streams.erase(a[0]);
        vm.segs[SegOf(a[0])].alive = false;
        return Int(r);
    };
    library["fread"] = [](VM &vm, const uint64_t *a, int) -> uint64_t {
        auto &st = vm.StreamOf(a[3]);
        auto n = a[1] * a[2];
        if (!n) return 0;
        auto got = vm.In(st, (char *)vm.Access(a[0], n), (size_t)n);
        return got / a[1];
    };
    library["fwrite"] = [](VM &vm, const uint64_t *a, int) -> uint64_t {
        auto &st = vm.StreamOf(a[3]);
        auto n = a[1] * a[2];
        if (n) vm.Out(st, string_view((const char *)vm.Access(a[0], n), (size_t)n));
        return st.err ? 0 : a[2];
    };
    library["fgetc"] = [](VM &vm, const uint64_t *a, int) -> uint64_t {
        auto &st = vm.StreamOf(a[0]);
        char c;
        return vm.In(st, &c, 1) ? (uint8_t)c : Int(-1);
    };
    library["getchar"] = [](VM &vm, const uint64_t *, int) -> uint64_t {
        return vm.library["fgetc"](vm, &vm.libobjects["stdin"], 1);
    };
    library["fgets"] = [](VM &vm, const uint64_t *a, int) -> uint64_t {
        auto &st = vm.StreamOf(a[2]);
        auto n = (int32_t)a[1];
        if (n <= 0) return 0;
        string line;
        char c;
        while ((int)line.size() < n - 1 && vm.In(st, &c, 1)) {
            line += c;
            if (c == '\n') break;
        }
        if (line.empty()) return 0;
        memcpy(vm.Access(a[0], line.size() + 1), line.c_str(), line.size() + 1);
        return a[0];
    };
    library["fputc"] = [](VM &vm, const uint64_t *a, int) -> uint64_t {
        char c = (char)a[0];
        vm.Out(vm.StreamOf(a[1]), string_view(&c, 1));
        return (uint8_t)c;
    };
    library["putchar"] = [](VM &vm, const uint64_t *a, int) -> uint64_t {
        char c = (char)a[0];
        vm.Out(vm.StreamOf(vm.libobjects["stdout"]), string_view(&c, 1));
        return (uint8_t)c;
    };
    library["fputs"] = [](VM &vm, const uint64_t *a, int) -> uint64_t {
        vm.Out(vm.StreamOf(a[1]), vm.CString(a[0]));
        return 1;
    };
    library["puts"] = [](VM &vm, const uint64_t *a, int) -> uint64_t {
        vm.Out(vm.StreamOf(vm.libobjects["stdout"]), cat(vm.CString(a[0]), "\n"));
        return 1;
    };
    library["fflush"] = [](VM &vm, const uint64_t *a, int) -> uint64_t {
        if (a[0]) {
            auto &st = vm.StreamOf(a[0]);
            if (st.file && fflush(st.file)) { st.err = true; return Int(-1); }
        }
        return 0;
    };
    library["feof"] = [](VM &vm, const uint64_t *a, int) -> uint64_t { return vm.StreamOf(a[0]).eof; };
    library["ferror"] = [](VM &vm, const uint64_t *a, int) -> uint64_t { return vm.StreamOf(a[0]).err; };
    library["fseek"] = [](VM &vm, const uint64_t *a, int) -> uint64_t {
        auto &st = vm.StreamOf(a[0]);
        if (!st.file) return Int(-1);
        int whence = (int32_t)a[2] == 0 ? SEEK_SET : (int32_t)a[2] == 1 ? SEEK_CUR : SEEK_END;
        if (fseek(st.file, (long)(int64_t)a[1], whence)) { vm.SetErrno(errno); return Int(-1); }
        st.eof = false;
        return 0;
    };
    library["ftell"] = [](VM &vm, const uint64_t *a, int) -> uint64_t {
        auto &st = vm.StreamOf(a[0]);
        return st.file ? Int(ftell(st.file)) : Int(-1);
    };
    library["remove"] = [](VM &vm, const uint64_t *a, int) -> uint64_t {
        if (remove(vm.CStr(a[0]).c_str())) { vm.SetErrno(errno); return Int(-1); }
        return 0;
    };
    library["rename"] = [](VM &vm, const uint64_t *a, int) -> uint64_t {
        if (rename(vm.CStr(a[0]).c_str(), vm.CStr(a[1]).c_str())) { vm.SetErrno(errno); return Int(-1); }
        return 0;
    };
    library["printf"] = [](VM &vm, const uint64_t *a, int n) -> uint64_t {
        auto s = CFormat(vm, vm.CString(a[0]), a + 1, n - 1);
        vm.Out(vm.StreamOf(vm.libobjects["stdout"]), s);
        return s.size();
    };
    library["fprintf"] = [](VM &vm, const uint64_t *a, int n) -> uint64_t {
        auto &st = vm.StreamOf(a[0]);
        auto s = CFormat(vm, vm.CString(a[1]), a + 2, n - 2);
        vm.Out(st, s);
        return s.size();
    };
    library["sprintf"] = [](VM &vm, const uint64_t *a, int n) -> uint64_t {
        auto s = CFormat(vm, vm.CString(a[1]), a + 2, n - 2);
        return StoreFormatted(vm, a[0], s.size() + 1, s);
    };
    library["snprintf"] = [](VM &vm, const uint64_t *a, int n) -> uint64_t {
        auto s = CFormat(vm, vm.CString(a[2]), a + 3, n - 3);
        return StoreFormatted(vm, a[0], a[1], s);
    };

    // -- Time
    library["clock_gettime"] = [](VM &vm, const uint64_t *a, int) -> uint64_t {
        // Through <chrono>, which every host has: CLOCK_MONOTONIC is the
        // steady clock, anything else the wall clock.
        int64_t ns = (int32_t)a[0] == 1
            ? chrono::duration_cast<chrono::nanoseconds>(chrono::steady_clock::now().time_since_epoch()).count()
            : chrono::duration_cast<chrono::nanoseconds>(chrono::system_clock::now().time_since_epoch()).count();
        auto p = vm.Access(a[1], 16);
        VM::StoreVal(p, VT_I64, Int(ns / 1000000000));
        VM::StoreVal(p + 8, VT_I64, Int(ns % 1000000000));
        return 0;
    };
    library["time"] = [](VM &vm, const uint64_t *a, int) -> uint64_t {
        auto t = (int64_t)time(nullptr);
        if (a[0]) VM::StoreVal(vm.Access(a[0], 8), VT_I64, Int(t));
        return Int(t);
    };
    library["nanosleep"] = [](VM &vm, const uint64_t *a, int) -> uint64_t {
        auto p = vm.Access(a[0], 16);
        struct timespec ts;
        ts.tv_sec = (time_t)(int64_t)VM::LoadVal(p, VT_I64);
        ts.tv_nsec = (long)(int64_t)VM::LoadVal(p + 8, VT_I64);
#ifndef _WIN32
        nanosleep(&ts, nullptr);
#endif
        return 0;
    };

#ifndef _WIN32
    // -- Files and directories, the POSIX subset runtime_os.h is written
    // against. Descriptors are the host's; 0, 1 and 2 go through the VM's
    // standard streams. Flags and modes arrive in the prelude's numbering.
    library["open"] = [](VM &vm, const uint64_t *a, int n) -> uint64_t {
        auto path = vm.CStr(a[0]);
        auto f = (int32_t)a[1];
        int flags = (f & 3) == 0 ? O_RDONLY : (f & 3) == 1 ? O_WRONLY : O_RDWR;
        if (f & 0100) flags |= O_CREAT;
        if (f & 0200) flags |= O_EXCL;
        if (f & 01000) flags |= O_TRUNC;
        if (f & 02000) flags |= O_APPEND;
        int fd = open(path.c_str(), flags, n > 2 ? (int)a[2] : 0666);
        if (fd < 0) vm.SetErrno(errno);
        return Int(fd);
    };
    library["read"] = [](VM &vm, const uint64_t *a, int) -> uint64_t {
        auto fd = (int32_t)a[0];
        if (!a[2]) return 0;
        auto p = (char *)vm.Access(a[1], a[2]);
        if (fd == 0) return vm.read(p, (size_t)a[2]);
        auto r = ::read(fd, p, (size_t)a[2]);
        if (r < 0) vm.SetErrno(errno);
        return Int(r);
    };
    library["write"] = [](VM &vm, const uint64_t *a, int) -> uint64_t {
        auto fd = (int32_t)a[0];
        if (!a[2]) return 0;
        auto p = (const char *)vm.Access(a[1], a[2]);
        if (fd == 1 || fd == 2) { vm.write(fd, string_view(p, (size_t)a[2])); return a[2]; }
        auto r = ::write(fd, p, (size_t)a[2]);
        if (r < 0) vm.SetErrno(errno);
        return Int(r);
    };
    library["close"] = [](VM &vm, const uint64_t *a, int) -> uint64_t {
        auto fd = (int32_t)a[0];
        if (fd >= 0 && fd <= 2) return 0;
        if (close(fd)) { vm.SetErrno(errno); return Int(-1); }
        return 0;
    };
    library["fsync"] = [](VM &vm, const uint64_t *a, int) -> uint64_t {
        auto fd = (int32_t)a[0];
        if (fd >= 0 && fd <= 2) return 0;
        if (fsync(fd)) { vm.SetErrno(errno); return Int(-1); }
        return 0;
    };
    library["unlink"] = [](VM &vm, const uint64_t *a, int) -> uint64_t {
        if (unlink(vm.CStr(a[0]).c_str())) { vm.SetErrno(errno); return Int(-1); }
        return 0;
    };
    library["rmdir"] = [](VM &vm, const uint64_t *a, int) -> uint64_t {
        if (rmdir(vm.CStr(a[0]).c_str())) { vm.SetErrno(errno); return Int(-1); }
        return 0;
    };
    library["mkdir"] = [](VM &vm, const uint64_t *a, int) -> uint64_t {
        if (mkdir(vm.CStr(a[0]).c_str(), (mode_t)a[1])) { vm.SetErrno(errno); return Int(-1); }
        return 0;
    };
    library["stat"] = [](VM &vm, const uint64_t *a, int) -> uint64_t {
        struct stat st;
        if (stat(vm.CStr(a[0]).c_str(), &st)) { vm.SetErrno(errno); return Int(-1); }
        auto p = vm.Access(a[1], 24);
        uint32_t mode = S_ISDIR(st.st_mode) ? 0040000u : S_ISREG(st.st_mode) ? 0100000u : 0;
        VM::StoreVal(p, VT_U32, mode | (st.st_mode & 0777u));
        VM::StoreVal(p + 8, VT_I64, Int(st.st_size));
        VM::StoreVal(p + 16, VT_I64, Int(st.st_mtime));
        return 0;
    };
    library["opendir"] = [](VM &vm, const uint64_t *a, int) -> uint64_t {
        auto d = opendir(vm.CStr(a[0]).c_str());
        if (!d) { vm.SetErrno(errno); return 0; }
        auto h = Handle(vm.NewSeg(SEG_STATIC, "a directory", 1), 0);
        vm.dirs[h] = d;
        vm.direntries[h] = Handle(vm.NewSeg(SEG_STATIC, "a directory entry", 256), 0);
        return h;
    };
    library["readdir"] = [](VM &vm, const uint64_t *a, int) -> uint64_t {
        auto it = vm.dirs.find(a[0]);
        if (it == vm.dirs.end()) throw Fault { "readdir of something opendir did not return" };
        errno = 0;
        auto e = readdir((DIR *)it->second);
        vm.SetErrno(errno);
        if (!e) return 0;
        auto h = vm.direntries[a[0]];
        auto p = vm.Access(h, 256);
        auto n = min<size_t>(strlen(e->d_name), 255);
        memcpy(p, e->d_name, n);
        p[n] = 0;
        return h;
    };
    library["closedir"] = [](VM &vm, const uint64_t *a, int) -> uint64_t {
        auto it = vm.dirs.find(a[0]);
        if (it == vm.dirs.end()) throw Fault { "closedir of something opendir did not return" };
        closedir((DIR *)it->second);
        vm.dirs.erase(it);
        vm.segs[SegOf(a[0])].alive = false;
        return 0;
    };
#endif

    // -- The runtime's GS_INTERP branch reserves a data stack with these
    // (§5, §10): a segment that grows on access up to the reservation.
    library["gs_host_reserve"] = [](VM &vm, const uint64_t *a, int) -> uint64_t {
        if (a[0] > 0xffffffffull) throw Fault { cat("stack reservation of ", a[0], " bytes exceeds the 4 GB a handle addresses") };
        auto id = vm.NewSeg(SEG_STACK, "", 0);
        vm.segs[id].limit = a[0];
        return Handle(id, 0);
    };
    library["gs_host_release"] = [](VM &vm, const uint64_t *a, int) -> uint64_t {
        auto &s = vm.Seg(a[0]);
        if (s.kind != SEG_STACK || OffOf(a[0]) != 0) throw Fault { cat("release of ", vm.Describe(s)) };
        vm.Free(a[0]);
        return 0;
    };
}

}  // namespace interp
}  // namespace goose
