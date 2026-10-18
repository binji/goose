#!/usr/bin/env python3
"""The interpreter's thunks for a builtin package (docs/design/interpreter.md
section 7.1): one C function per entry of the package's X-macro API lists,
which takes the arguments as the VM laid them out, decodes what is a
pointer, calls the layer's function and stores the result.

    gen_thunks.py <package> <out.c> [--yields name,...] header.h [header.h...]

Each header declares its API as `X(ret, name, (params))` lines inside a
`#define GS_..._API(X)` macro, and above it the typedefs that say what every
parameter type is: a scalar, a flat struct by value, a slice (a struct of
exactly `T *data; int64_t len;`), or a pointer to one of those. Those are
the shapes the foreign boundary admits, so nothing else is expected; a
shape this script does not know is an error naming the function.

The VM lays the arguments out one after another, each at an 8-byte-rounded
slot, in the layout of the generated C (64-bit pointers, packed structs):
a scalar as its own size, a flat struct as its bytes, a slice as a 64-bit
handle and a 64-bit length, a pointer as a 64-bit handle. A thunk reads
handles and lengths as 64-bit fields and decodes each handle to a host
pointer through goose_vm_decode, so the same thunk serves a 64-bit host and
the 32-bit Wasm one. A struct that crosses by value holds no pointers, so
its bytes are the same on both. The result goes into a 64-byte buffer the
VM reads by the prototype's return type.
"""

import os
import re
import sys

SCALARS = {
    "int8_t", "uint8_t", "int16_t", "uint16_t", "int32_t", "uint32_t", "int64_t", "uint64_t",
    "float", "double", "_Bool", "bool", "char", "int", "unsigned", "long", "size_t",
}


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def parse_types(text):
    """{name: ("slice", elemtype) | ("struct",) | ("scalar",)} from the
    header's typedefs."""
    types = {}
    for m in re.finditer(r"typedef\s+struct\s*(?:\w+\s*)?\{([^}]*)\}\s*(\w+)\s*;", text):
        fields, name = m.group(1), m.group(2)
        sm = re.match(r"^\s*(?:const\s+)?(\w+)\s*\*\s*data\s*;\s*int64_t\s+len\s*;\s*$", fields)
        types[name] = ("slice", sm.group(1)) if sm else ("struct",)
    for m in re.finditer(r"typedef\s+((?:unsigned\s+|signed\s+)?\w+)\s+(\w+)\s*;", text):
        if m.group(1) in SCALARS or m.group(1) in types and types[m.group(1)] == ("scalar",):
            types[m.group(2)] = ("scalar",)
    return types


def api_entries(text):
    """(ret, name, [param text, ...]) for every X(...) of every *_API(X) macro."""
    entries = []
    lines = text.split("\n")
    i = 0
    while i < len(lines):
        if not re.match(r"\s*#\s*define\s+\w+_API\s*\(\s*X\s*\)", lines[i]):
            i += 1
            continue
        # The macro runs to the first line without a trailing backslash.
        body = []
        while i < len(lines):
            line = lines[i].rstrip()
            i += 1
            body.append(line.rstrip("\\"))
            if not line.endswith("\\"):
                break
        body = "\n".join(body)
        j = body.find(")", 0) + 1   # past the macro's own (X)
        while True:
            j = body.find("X(", j)
            if j < 0:
                break
            if j > 0 and (body[j - 1].isalnum() or body[j - 1] == "_"):
                j += 2
                continue
            depth, k = 0, j + 1
            while k < len(body):
                if body[k] == "(":
                    depth += 1
                elif body[k] == ")":
                    depth -= 1
                    if depth == 0:
                        break
                k += 1
            inner = body[j + 2:k]
            ret, rest = [s.strip() for s in inner.split(",", 1)]
            name, params = rest.split(",", 1)
            params = params.strip()
            assert params.startswith("(") and params.endswith(")"), inner
            params = params[1:-1].strip()
            plist = [] if params in ("", "void") else [p.strip() for p in params.split(",")]
            entries.append((ret, name.strip(), plist))
            j = k + 1
    return entries


def classify(ptype, types):
    """(kind, base) of a parameter or return type: kind is scalar, struct,
    slice or pointer; base is the type the handle points at, for a pointer."""
    t = re.sub(r"\bconst\b", "", ptype).strip()
    if t.endswith("*"):
        return "pointer", t[:-1].strip()
    if t in types:
        return types[t][0], t
    if t in SCALARS:
        return "scalar", t
    return None, t


def thunk(ret, name, params, types):
    lines = [f"static void thunk_{name}(goose_vm *vm, const uint8_t *args, uint8_t *ret) {{",
             "    (void)vm; (void)args; (void)ret;",
             "    size_t off = 0;"]
    names = []
    for i, p in enumerate(params):
        m = re.match(r"^(.*?)\s*(\w+)$", p.strip())
        if not m:
            raise SystemExit(f"{name}: cannot read the parameter '{p}'")
        ptype, pname = m.group(1).strip(), m.group(2)
        kind, base = classify(ptype, types)
        var = f"a{i}"
        names.append(var)
        if kind == "scalar":
            lines.append(f"    {ptype} {var}; memcpy(&{var}, args + off, sizeof {var}); off += 8;")
        elif kind == "struct":
            lines.append(f"    {ptype} {var}; memcpy(&{var}, args + off, sizeof {var}); "
                         f"off += (sizeof {var} + 7) & ~(size_t)7;")
        elif kind == "slice":
            elem = types[base][1]
            lines.append(f"    {ptype} {var}; {{ uint64_t h; int64_t n; memcpy(&h, args + off, 8); "
                         f"memcpy(&n, args + off + 8, 8); "
                         f"{var}.data = goose_vm_decode(vm, h, (uint64_t)(n < 0 ? 0 : n) * sizeof({elem})); "
                         f"{var}.len = n; off += 16; }}")
        elif kind == "pointer":
            size = "0" if base == "void" else f"sizeof({base})"
            lines.append(f"    {ptype} {var}; {{ uint64_t h; memcpy(&h, args + off, 8); "
                         f"{var} = goose_vm_decode(vm, h, {size}); off += 8; }}")
        else:
            raise SystemExit(f"{name}: parameter {pname} has a type this script does not know: {ptype}")
    lines.append("    (void)off;")
    call = f"{name}({', '.join(names)})"
    if ret == "void":
        lines.append(f"    {call};")
    else:
        kind, _ = classify(ret, types)
        if kind not in ("scalar", "struct"):
            raise SystemExit(f"{name}: returns a type this script does not know: {ret}")
        lines.append(f"    {{ {ret} r = {call}; GOOSE_THUNK_RET_FITS({ret}); memcpy(ret, &r, sizeof r); }}")
    lines.append("}")
    return "\n".join(lines)


def main():
    args = sys.argv[1:]
    if len(args) < 3:
        raise SystemExit(__doc__)
    package, out = args[0], args[1]
    yields = set()
    headers = []
    i = 2
    while i < len(args):
        if args[i] == "--yields":
            yields.update(n for n in args[i + 1].split(",") if n)
            i += 2
        else:
            headers.append(args[i])
            i += 1
    types, entries = {}, []
    for h in headers:
        text = strip_comments(open(h, encoding="utf-8").read())
        types.update(parse_types(text))
        entries += api_entries(text)
    if not entries:
        raise SystemExit(f"{package}: no X(...) entries found in {', '.join(headers)}")
    seen = set()
    entries = [e for e in entries if not (e[1] in seen or seen.add(e[1]))]
    unknown = yields - {e[1] for e in entries}
    if unknown:
        raise SystemExit(f"{package}: thunk_yields names no function of the API: {', '.join(sorted(unknown))}")
    src = [f"/* Generated by scripts/gen_thunks.py for the builtin package {package}: the",
           "   interpreter's thunks for its C API (docs/design/interpreter.md 7.1). Do not",
           "   edit. */", "", "#include <stdint.h>", "#include <string.h>", ""]
    src += [f'#include "{os.path.abspath(h).replace(os.sep, "/")}"' for h in headers]
    src += ["",
            "typedef struct goose_vm goose_vm;",
            "void *goose_vm_decode(goose_vm *vm, uint64_t handle, uint64_t len);",
            "typedef void (*goose_thunk_fn)(goose_vm *vm, const uint8_t *args, uint8_t *ret);",
            "typedef struct { const char *name; goose_thunk_fn fn; int yields; } goose_thunk;",
            "#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L",
            "#define GOOSE_THUNK_RET_FITS(T) _Static_assert(sizeof(T) <= 64, \"a thunk's result fits 64 bytes\")",
            "#else",
            "#define GOOSE_THUNK_RET_FITS(T) (void)0",
            "#endif", ""]
    for ret, name, params in entries:
        src.append(thunk(ret, name, params, types))
        src.append("")
    src.append(f"const goose_thunk *goose_thunks_{package}(void) {{")
    src.append("    static const goose_thunk table[] = {")
    for _, name, _ in entries:
        src.append(f'        {{ "{name}", thunk_{name}, {1 if name in yields else 0} }},')
    src += ["        { 0, 0, 0 }", "    };", "    return table;", "}", ""]
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(src))


if __name__ == "__main__":
    main()
