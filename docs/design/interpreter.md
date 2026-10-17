# An interpreter for the browser

<details>
<summary>Table of contents</summary>

- [1. The goal](#1-the-goal)
- [2. What to interpret](#2-what-to-interpret)
- [3. What the emitted C consists of](#3-what-the-emitted-c-consists-of)
- [4. The interpreter](#4-the-interpreter)
- [5. Memory: handles and segments](#5-memory-handles-and-segments)
- [6. The C library](#6-the-c-library)
- [7. Native packages](#7-native-packages)
- [8. Building for Wasm with clang](#8-building-for-wasm-with-clang)
- [9. The page](#9-the-page)
- [10. Changes to the existing code](#10-changes-to-the-existing-code)
- [11. Testing](#11-testing)
- [12. Limits and follow-ups](#12-limits-and-follow-ups)
- [13. Estimates](#13-estimates)

</details>

A proposal, not implemented. [wasm.md](wasm.md) works out how *compiled*
Goose programs could run on WebAssembly, which needs the emitted code to
check growth and, for programs of any size, to address stacks through
handles. This note is about something smaller: running the Goose compiler
itself in a browser page and executing programs there, as a demonstration
of the language. Speed does not matter; changing as little of the compiler
as possible does. The cost and size figures are estimates, not
measurements.

## 1. The goal

A page where a visitor picks a sample or types a program, presses Run and
sees its output and diagnostics, with no server behind it, and where the
graphical samples draw into a canvas. The compiler is C++ with no platform
dependencies on the path from source to C (`main.cpp`, §1 of
[implementation.md](../implementation.md)), so clang builds it to Wasm as
it is. What it cannot do in a browser is the last step: a JIT run hands
the C to TinyCC, which emits x86-64 machine code into the process (`jit.h`,
[jit_backend.md](jit_backend.md)). The page needs another way to run the
program, and that is what this note designs.

Two binaries come out of it. `goose.wasm` holds the compiler, the
interpreter and the runtime, and runs every program whose externs are the
C library's: the first 26 samples apart from the threaded one. `goose-gfx.wasm`
adds the blessed packages compiled in, as a desktop compiler configured
with `GOOSE_BUILTIN_LIBS` has them (packages.md §7.1), over a browser
backend in place of SDL, and runs the graphical samples as well. Both are
the same source and the same CMake project with one toolchain file; the
first is the smaller download and the one a page needs for everything but
graphics.

## 2. What to interpret

The compiler has four levels at which a program could be executed. In order
of how much of the existing code each would duplicate or disturb:

* **The typed AST**, after typecheck, optimization and BCE. An evaluator
  over `Node` would be a second backend: it would have to reproduce every
  layout decision of §6.1 (packed fields, length prefixes, ADT tags, frame
  objects), the destinations and in-place construction of §6.5, the
  generated walkers, `str()` and `print` rendering, the pool freelists,
  every builtin of `codegen_builtins.h`, and keep all of it equal to what
  the C backend does, since `to_bytes`, serialization and `extern fn`s
  expose the bytes. That is most of the 12,000 lines of `codegen*.h` again,
  drifting from the original with every change.
* **A bytecode of codegen's own.** Codegen writes C text while it runs its
  analyses (top caching, view hoisting, scope restores); a second emitter
  inside it is a rewrite of it.
* **The emitted C, compiled in the browser** by a C compiler built to Wasm
  (clang is tens of megabytes). The generated code would then run on raw
  linear memory, which is exactly the problem wasm.md works out: growth
  checks in the emitted code and fixed partitions or relocation, i.e. real
  changes to codegen and the runtime before anything runs. And a program
  that blocks in a frame loop cannot run on a browser's main thread
  without Asyncify-style transformation of the compiled code.
* **The emitted C, interpreted.** The C file a JIT run compiles is the
  complete definition of the program's behavior, and the one artifact every
  other backend already agrees on. An interpreter for it changes nothing
  before it; because an interpreter owns the representation of pointers,
  it can let stacks move without the emitted code knowing (section 5); and
  because its call stack is data, it can suspend a program in the middle of
  a frame and resume it from the browser's animation callback (section 7).
  That dissolves the wasm problem for the demonstration rather than solving
  it.

The last is the design. The interpreter takes the place of libtcc at the one
seam the driver already has: `RunJit(csrc, ...)` becomes `RunInterp(csrc,
...)`, on the same text, with the same program arguments and the same
package symbol tables, and the compiler is otherwise unaware of it.

Two ways of getting a C interpreter were considered and set aside. An
existing one (PicoC and its relatives) would have to be bent to a dialect
it was not written for, with no control over what it gets wrong; the
runtime is ordinary C99, but the emitted code relies on `#pragma pack(1)`,
on 64-bit arithmetic everywhere, and on unaligned typed accesses through
`uint8_t *` cursors. A bytecode backend for TinyCC would reuse its front
end, which already compiles every program the suite has, but TinyCC's
backend interface is a register machine over a flat address space: pointers
would be addresses again, stacks could not move, and the work lands in
vendored third-party code. A purpose-built interpreter is the smaller
risk, and its size is bounded by what codegen and the runtime actually
emit, which section 3 lists.

## 3. What the emitted C consists of

The text a JIT run compiles is `runtime.h`, `runtime_impl.h`,
`runtime_threads.h`, codegen's types and prototypes, `runtime_ext.h`,
`runtime_os.h`, any `--include` headers, and the program (`assemble` in
`main.cpp`). Of these, the program's part is a narrow dialect, by
construction:

* **Control flow**: `if`, `for`, `while`, `switch` on ADT tags
  (`EmitDispatch`, `match`), `goto` and labels (the scope exits of §6.2 and
  the flat `if` of §3.9 are gotos: 531 in nine samples' bodies), `return`,
  the conditional operator.
* **Data**: packed structs and unions (`{ tag; union }` ADTs), typedefs,
  fixed arrays wrapped in structs, `static const` byte images for string
  literals and `gstatic` globals, one `gs_globals_t` instance, locals and
  temporaries with `= {0}` initializers.
* **Operations**: integer arithmetic through the runtime's `gs_add_i32`
  family (release macros, so plain C operators after preprocessing),
  explicit casts on nearly every operand, pointer arithmetic on
  `uint8_t *` (`base + i * size`, `t139 += 4`), typed accesses `*(T *)p`,
  pointer differences for relative references and pool range tests
  (`(uintptr_t)a - (uintptr_t)b`, the one `uintptr_t` codegen writes), the
  `mem*` functions, and calls, including direct calls to `extern fn`s with
  the prototypes codegen emits for them.
* **Absent**: function pointers (a function value is spliced inline and tag
  dispatch is a `switch`, §6.6), varargs, `alloca`, VLAs, bitfields,
  `setjmp`, inline assembly, `__int128`, `_Alignas`.

The runtime adds what a hand-written C99 library uses: `static inline`
functions, enums, `const char *` strings, `sizeof`, `do`/`while` (twice),
varargs *calls* to `fprintf` and `snprintf`, one function pointer passed to
`qsort` (`gs_os_name_order`), and `__attribute__((noreturn))`, which the
non-MSVC branch of `GS_NORETURN` expands to. Its platform-specific parts are
behind `#ifdef`s that an unknown compiler never enters: the SSE2 and SSSE3
scanners (`!defined(__TINYC__) && defined(__x86_64__)`), the `divq` asm and
`unsigned __int128` (`__SIZEOF_INT128__`), `__builtin_constant_p`
(`__TINYC__`), `__builtin_elementwise_sqrt` (`__has_builtin`), thread-local
storage (`GS_NEED_THREADS`), and `GS_INLINE`, which falls back to `inline`.
An interpreter that predefines none of those macros gets the portable
runtime that was written for compilers nobody tested, plus the `_WIN32` or
POSIX section for regions, faults and the OS, which section 10 replaces.

## 4. The interpreter

Four pieces, in `src/interp/`, built into the compiler like `jit.h` is.

1. **Preprocessor.** Object-like and function-like macros, `#`, `##`,
   `#if`/`#elif` with the constant expressions `runtime.h` uses
   (`GS_STACK_RESERVE > (1ull << 31)`, `defined`), `#include "file"` for
   `--include` headers through the driver's file reading, `#pragma pack`
   kept as a token for the parser, and `#include <name>` of the standard
   headers mapped to a built-in prelude (section 6). The predefined macros
   are `__STDC_VERSION__`, `__GOOSE_INTERP__` and `__has_builtin(x)` as 0,
   and nothing a platform branch tests. (TinyCC's `-E` is the fallback if
   this piece proves tiresome: libtcc's preprocessor has no platform
   dependency, but its own predefined macros would have to be undefined
   first, and the result still has to be parsed.)
2. **Front end.** A C99 parser for the dialect above: declarations with
   the full declarator syntax (codegen's typedefs keep them simple, but the
   runtime has `const char **v` and arrays of structs), struct and union
   layout honoring `#pragma pack(1)` (the generated layouts *must* come out
   as codegen computed them, since the same memory is written through
   `uint8_t *` cursors and read back through the struct), enums, static
   initializers, integer promotions and the usual arithmetic conversions,
   lvalues, `sizeof`, and pointers to interpreted functions, which the
   runtime's one `qsort` needs and which cost little once calls are
   indirect through a function table anyway. `__attribute__((...))` is
   parsed and dropped. Undefined functions resolve against the C library
   (section 6) and then the native packages' thunk tables (section 7).
3. **Lowering** to a typed bytecode: by the time an operation reaches the
   VM its operand types are decided (`add.i32`, `load.u8`, `cvt.f64.i32`,
   `ptradd`, `ptrdiff`), so the VM has no dynamic typing. A stack VM with
   per-function frame slots is enough; locals live in the frame segment
   (section 5), so `&local` is a handle like any other and `gs_rref`s to a
   local `gs_rhdr` need no special case.
4. **The VM.** A `switch` loop over the bytecode. Calls push an explicit
   frame rather than recursing in C++, so interpreted recursion depth costs
   no host stack, and the "native call stack overflow" the runtime
   promises (§7) is the VM's own depth cap, reported through the runtime's
   message. `exit` and the runtime's aborts stop the loop and return the
   status to the driver. Because every frame is data, the loop can also
   *suspend*: return to its caller with the program parked at an
   instruction, to be continued by a later call. Section 7 uses this at
   the end of a frame; the page uses it to keep the browser responsive, a
   yield after a time budget checked every few thousand instructions.

Everything in the C text is interpreted, the runtime included. Only the C
library and the native packages are compiled code. This keeps every write
to a data stack inside the VM, which is what lets stacks move.

## 5. Memory: handles and segments

A pointer is a 64-bit integer that is not an address: a segment id in the
high 32 bits and a byte offset in the low 32. Zero is `NULL`. The VM decodes
a handle only at a load, a store or a call into compiled code; everything
else the emitted code does with pointers is integer arithmetic that works
on the handle unchanged:

* `base + i * size`, field offsets, `t139 += 4`, a relative reference's
  `(uint8_t *)&field + off` with a negative `off`: addition to the low
  half, checked to stay within `[0, limit]` of the segment. A carry into the
  id is an interpreter fault, never a silent wrap.
* `a - b` on pointers, and the `uintptr_t` difference of a pool's range
  test: both must name one segment (else a fault), and the result is the
  difference of offsets.
* `==`, `!=` on the whole handle; `<` and friends on the offsets of one
  segment; `if (p)` is `p != 0`.
* Casts between pointers and integers are the identity, as they are
  natively.

This is the encoding wasm.md §4 proposes for emitted code (`bases[h >> 32]
+ (uint32_t)h`). Here it costs nothing in codegen because the decode is the
interpreter's load and store, which decode anyway.

**Segments** are heap blocks the VM owns, in a table indexed by id, each
with a kind, a current size and a limit:

| Kind | Made by | Growth | Limit |
|---|---|---|---|
| static | string literals, `gstatic` constants, `gs_globals_main`, `gs_errmsgs`, one per object | none | its size |
| frame | the VM, one segment for all interpreted frames | as frames push | the native stack size, which caps recursion |
| heap | `malloc`, `calloc`, `realloc`, one per block; `free` kills it | none | its size |
| stack | `gs_reserve_region`, one per data stack | on access past its size: doubled, zero-filled, moved if the allocator moves it | the reservation |

A load or store decodes `(id, off, n)`: the segment must be alive and
`off + n <= size`; a stack segment with `off + n <= limit` grows instead,
and a stack access past its limit is the data stack overflow the runtime
reports natively (`gs_overflow_text`, with the reservation it names). Any
other out-of-range access is an interpreter fault with the access and the
segment: natively it would have been a guard page or a silent corruption,
so the interpreter doubles as a sanitizer for codegen that knows object
bounds ASan cannot see inside the arenas ([testing.md](../testing.md)).

**Why this needs no growth checks.** The emitted code bumps a cached top
(`t139 += 4`) and writes at it, syncing `->top` later (§6.10). The VM never
looks at `top`; it grows a stack segment the moment a write lands past its
size, which is the role the guard page and the fault handler play natively.
Moving the block is invisible: no handle holds an address, so the
properties the language promises (references survive growth, a stack's
base never moves, §1.2) hold exactly, and the in-place construction that
wasm.md §6 lists as the riskiest part of a compiled port needs nothing.
Relative references and pools are offsets within a stack, and a stack
moves whole. `GS_STACK_RESERVE` keeps its meaning as the limit, and should
be passed as a define of at most 2 GB (the default) so offsets fit the low
half and the `#if GS_STACK_RESERVE > (1ull << 31)` relative-offset checks
compile out as they do today; `--stack-reserve` already exists for the
page to size it below that.

Memory is proportional to use. Peak is at most twice a stack's high-water
mark while it doubles, and the page never has to reserve a fixed partition
per stack up front, which wasm.md §3 identifies as the capacity problem of
a compiled port.

Values are little-endian byte images in the segments, as natively; typed
loads copy bytes, so unaligned packed accesses are just loads. In the Wasm
build the segments are blocks of the module's own linear memory, so a
decoded handle is an ordinary pointer there, and compiled code (the C
library, a package's layer) reads and writes a segment in place for the
duration of a call. Floating point runs on the host's `double` and
`float`, never fused (the runtime's `fp contract(off)` pragmas exist for
exactly this reason), and `print` formats floats with the runtime's own
`gs_fmt_float`, so output matches a JIT run byte for byte.

## 6. The C library

The runtime and the `extern fn`s of ordinary programs call the C library,
and in every build of the interpreter that library is the one the
interpreter itself is linked with: glibc or the platform's natively,
wasi-libc in the Wasm module. The prelude declares, and a table in the
interpreter binds, each name to a small adapter that decodes pointer
arguments and calls the real function. Decoding follows each function's
contract: `memcpy(d, s, n)` decodes `n` bytes of each (growing a stack
segment for a write, as a store would), `fwrite(p, 1, n, f)` decodes `n`,
`strlen` and `fputs` decode up to the NUL within the segment. No adapter
keeps a decoded pointer past its return, which a segment's possible move
enforces. The table:

* **Memory**: `memcpy`, `memmove`, `memset`, `memcmp`, `memchr`, `strlen`;
  `malloc`, `calloc`, `realloc`, `free` as segment operations.
* **Output**: `fwrite`, `fputs`, `fflush`, `setvbuf` (no-op), `fprintf` and
  `snprintf` over the runtime's formats (`%s`, `%d`, `%lld`, `%llu`, `%ld`,
  `%zu`), `stdout` and `stderr` as two handles for the process's streams,
  which in the browser are WASI file descriptors the page reads (section
  9).
* **Process**: `exit`, `_exit`, `abort`, `getenv`, `clock_gettime`.
* **Math**: `sqrt`, `sin`, `cos`, `tan`, `asin`, `acos`, `atan`, `atan2`,
  `exp`, `log`, `log2`, `log10`, `pow`, their `f` forms, `fmod`, `trunc`,
  `floor`, `ceil`, `frexp`, `isinf`, `isnan` (functions in the prelude where
  `math.h` has macros), and what the samples' `extern fn`s name (`cbrt`,
  `hypot`, `abs`).
* **`qsort`**: an interpreted implementation in the prelude rather than an
  adapter, since its comparison function is interpreted code.
* **Files and directories** for `os`: the POSIX subset `runtime_os.h`'s
  `GS_INTERP` branch (section 10) is written against, which wasi-libc
  implements over WASI's `path_open`, `fd_read` and `fd_readdir`, and the
  page's shim over an in-memory directory tree.

Some 60 to 80 entries. An `--include` header is C text in the same unit
and is interpreted with the program, so `samples/24_call_c.goose` runs in
the browser with no C compiler.

## 7. Native packages

The blessed packages (`gfx`, `audio`, `ui`, `physics`;
[packages.md](packages.md) §4.5) are Goose modules over C layers: gfx's
over SDL_GPU, audio's over SDL's audio, ui's over Nuklear with no SDL in it
at all (it draws through gfx *in Goose*, §5.5), physics's over Box3D. A
compiler configured with `GOOSE_BUILTIN_LIBS` links the layers in, and a
JIT run hands TinyCC every function's address from the package's generated
symbol table (`goose_symbols_<name>`, §5.3, §7.1). The interpreter needs
three things more than an address: a way to call compiled code with a
signature it learns at run time, a way to let the program's frame loop
breathe in a browser, and a browser backend for the layers that today sit
on SDL. The first two are the interpreter's; the third is the gfx
package's.

### 7.1 Thunks

Wasm has no way to call a function pointer with a signature chosen at run
time (`call_indirect` is typed, and there is no libffi), so the compiler's
build generates one thunk per extern function of each builtin package,
beside the symbol table it already generates. Each layer declares its C
API as an X-macro list, `X(ret, name, (params))`:

```c
X(uint8_t, gs_gfx_open, (gs_gfx_bytes title, int64_t width, int64_t height, int64_t flags))
X(void, gs_gfx_screen_size, (gs_gfx_int2 *out))
```

`GS_GFX_API`, `GS_UI_API` and `GS_AUDIO_API` list 79, 365 and 12 functions,
and the typedefs above them say what every parameter type is: a scalar, a
handle struct by value (`gs_gfx_texture { uint32_t id; }`), a slice
(`gs_gfx_bytes { uint8_t *data; int64_t len; }`), or a pointer to a flat
struct (`gs_gfx_int2 *`, `const gs_gfx_pipeline_desc *`). These are exactly
the shapes the foreign boundary admits (`CheckExternSpec`, §6.6), so a
script over the header can write, for each line, a function that takes the
VM's argument bytes, decodes what is a pointer, calls, and stores the
result:

```c
static void thunk_gs_gfx_open(gs_vm *vm, const uint8_t *args, uint8_t *ret) {
    gs_gfx_bytes title;  memcpy(&title, args + 0, sizeof title);
    title.data = gs_vm_decode(vm, (uint64_t)title.data, title.len);
    int64_t width, height, flags;  /* ... from args + 16, 24, 32 */
    *ret = gs_gfx_open(title, width, height, flags);
}
```

A slice decodes by its `len` times the element size, a pointer to a struct
by `sizeof` the struct, a scalar not at all. The thunk table is `{ name,
thunk, yields }` in a generated `goose_thunks_<name>.c`, which
`package.cmake` writes next to `goose_symbols_<name>.c`, and `builtin.h`'s
`BuiltinPackage` gains a `thunks` member the driver passes to `RunInterp`
as it passes `symbols` to `RunJit`. The VM, at a call to a function the
unit does not define, lays the arguments out by the prototype codegen
emitted for it (the generated code's layout, packed as its types are) and
calls the thunk by name. A package whose layer has no such list cannot be
built into the interpreter; gfx, audio and ui have one, and physics is
expected to, being written the same way.

**Marshaling.** In the Wasm build the VM's segments are linear memory, so a
decode is a pointer into the module and a layer reads a slice or writes
through an `int2 *` in place. The VM does not run during the call, so
nothing moves. A layer may not retain a pointer past the call, which is
already an obligation the foreign boundary places on C ("may not retain
borrowed arguments beyond the call", §6.6). Builder parameters
(`u8[>..]&`, a `gs_rref` C appends to with `gs_bld_append`) are the one
shape a thunk cannot hand over as is, since `gs_bld_append` writes at a
stack's top with no check: no blessed package uses one, so the interpreter
build rejects an extern with a builder parameter in a *native* package at
compile time, naming it. (An `--include` header's builders work, since its
C is interpreted.) If a package ever needs one, wasm.md §6.3's staging
stack is the mechanism: a scratch block the thunk hands C and appends to
the real stack afterwards.

The same thunks serve the desktop: `goose run --interp` of a gfx program
calls the real SDL layer through them, which is how the mechanism is
tested before any browser exists (section 11).

### 7.2 Frames and suspension

A gfx program is a loop of `while gfx::frame() { ... }`, and natively
`gs_gfx_frame` blocks until the swapchain hands it the next image
(`SDL_WaitAndAcquireGPUSwapchainTexture`). A browser's main thread cannot
block, and WebGL and input live on the main thread. The interpreter's
explicit frames make this easy: the thunk table marks `gs_gfx_frame` as a
*yield point*. When the VM reaches it, it calls the thunk, which in the
browser backend submits the frame's draws and returns at once, and then
the VM suspends: `goose_resume()` returns to the page with the program
parked after the call, and the page's `requestAnimationFrame` callback
feeds the input events that arrived into the layer and calls
`goose_resume()` again. The program sees exactly the sequence it sees
natively: `frame()` returns, and the next iteration's reads of keys and
mouse see what happened in between. A compiled program would need
Asyncify or JSPI for this; the interpreter needs a flag in a table.

Headless runs (`open_headless`, or `GOOSE_GFX_HEADLESS=1`, which the test
runner sets) have no swapchain to wait for and need no yield, which keeps
the test runs synchronous. `main_thread_only` (packages.md §5.4) is
already how the packages are declared, and the interpreter runs
everything on the one thread the page gives it.

### 7.3 The gfx package's browser backend

This is work in the gfx package's repository, not the compiler's, and the
interpreter is indifferent to how it is done; what follows is the shape
that looks shortest. The layer's four gfx files (`gfx_device.c`,
`gfx_draw.c`, `gfx_pipeline.c`, `gfx_resources.c`, 2,600 lines with some
500 SDL calls) become a second backend behind the same `GS_GFX_API`,
selected by a CMake option, written against a few dozen functions the page
provides, declared with clang's import attributes:

```c
__attribute__((import_module("gfx"), import_name("create_buffer")))
uint32_t gfx_js_create_buffer(int64_t size, int32_t usage);
```

The page implements them over **WebGL 2**. WebGPU would be the closer
match to SDL_GPU (command buffers, explicit pipelines, bind groups), but it
takes WGSL and nothing in the toolchain produces WGSL: `embed_shader`
compiles GLSL to a blob of SPIR-V, MSL and HLSL (`src/shaderc.c` over the
vendored `cute_spirv`, `src/gfx_blob.h`), and browsers do not take SPIR-V.
`cute_spirv` does emit **GLSL ES 3.00**, which is WebGL 2's language, and
its emitter already emulates read-only storage buffers for that target. So
`shaderc.c` adds a GLSL ES section to the blob (an offset and size in
`gs_gfx_blob_header`, a version bump both copies of `gfx_blob.h` take),
and the browser backend compiles that text with `glShaderSource` at
pipeline creation. The screen being an RGBA8 texture that `frame()` blits
to the window, rather than the swapchain itself, is what makes the rest
mechanical: the backend draws into a framebuffer and blits to the canvas.
WebGL 2 has uniform buffer objects (std140, as the blob's uniform sizes
assume), textures, samplers, multisampled renderbuffers and instancing,
which cover the graphical samples; it has no compute shaders and no
writable storage, so compute pipelines report "not available" through the
layer's own error path, and a program that needs them says so and stops.

The window and input part of `gfx_device.c` maps onto the canvas and DOM
events the page forwards into the layer's event queue between frames
(the layer already has `gs_gfx_inject_key`, `gs_gfx_inject_mouse` and
`gs_gfx_inject_text`, written for tests, which are exactly what the page
needs). Audio's 334 lines map onto Web Audio the same way, through an
`audio` import module. The ui layer compiles to Wasm unchanged, having no
SDL in it. Physics compiles unchanged if Box3D has no platform dependency
beyond its optional job system, which the single-threaded build does not
use.

With both binaries in hand: `goose.wasm` runs samples 01 to 22, 24, 25 and
26; `goose-gfx.wasm` adds 27 (`gfx`), 29 (`ui`), 30 (`gfx`) and 31 (`gfx`
and `audio`, with its WAD in the in-memory file system). 23 and 28 use
worker threads and wait for section 12.

## 8. Building for Wasm with clang

Clang targets `wasm32` without anything added, but a C++ program needs a
C library and a C++ standard library for the target, and those come from
**wasi-sdk**: wasi-libc, libc++ and libc++abi built for `wasm32-wasi`,
with a sysroot clang takes through `--sysroot`. A CMake toolchain file
(`cmake/wasm32-wasi.cmake`) sets the compiler, the sysroot and the flags
below; the normal `CMakeLists.txt` then builds `goose` as it does on any
other platform, with `GOOSE_NO_JIT` leaving libtcc out and, for the second
binary, `GOOSE_BUILTIN_LIBS=all` with the packages' browser backends
selected. What the compiler's own code needs from the platform, and what
each needs in this build:

* **C++ exceptions.** `CompileError` is thrown across the phases and
  rethrown from the compile thread (77 `throw` sites). Clang implements
  them on Wasm with `-fwasm-exceptions`, over the exception-handling
  proposal every current browser ships, but libc++abi has to be built for
  it, and wasi-sdk's releases have not always been. This is the first
  thing to verify on the chosen release; if its libc++abi lacks exception
  support, build libc++ and libc++abi from the sdk's own recipe with the
  flag, once, into the sysroot.
* **The compile thread.** `RunOnCompilerStack` makes a pthread with a 64 MB
  stack for the typechecker's recursion (§1). WASI has no threads; the
  function already falls back to running on the calling thread when no
  thread can be made, so a `__wasi__` branch takes that path, and the
  linker gives the one stack the size: `-Wl,-z,stack-size=67108864`.
  `toolchain.h` and `progress.cpp` include `<thread>`, which a libc++
  built without threads refuses; a `__wasi__` guard around the include and
  the one `hardware_concurrency` call.
* **Processes and shared libraries.** `process.cpp` (over
  `third_party/subprocess`) spawns the C compiler, cmake and git, and
  `LoadSharedSymbols` opens a package's JIT library, none of which exists
  in WASI. The build swaps in `process_wasi.cpp`: the six functions of
  `process.h` returning "not available". The verbs that reach them
  (`build`, `fetch`, a non-builtin package's native part) are not what the
  page offers.
* **Files.** The compiler reads sources with `fopen` (`ReadFile` in
  `utils.h`, `main.cpp`), finds the standard library beside the executable
  (`StdlibDirs`) and a builtin package's modules under `pkg/<name>/`
  beside it (`BuiltinPackageRoot`), and writes `-o` output and dump files.
  wasi-libc turns all of it into WASI calls on a preopened directory, so
  the page's shim serves a directory tree and nothing in the compiler
  changes.
* **Memory.** wasi-libc's `malloc` grows linear memory as needed; nothing
  caps it below Wasm's 4 GB unless the link says so.
* **Entry.** A WASI *command* runs `_start` once and exits; the page needs
  to call in repeatedly (section 7.2), so the module is built as a
  *reactor* (`-mexec-model=reactor`) that exports a few functions: `goose_run`
  taking a command line, which splits it into `argv` and calls `Main` as
  `main` does, `goose_resume`, and the input-event entry points of the gfx
  layer. A 30-line `web_entry.cpp`.
* **What is left as is.** `shaderc.c` and `cute_spirv` are portable C and
  keep `embed_shader` working, which the gfx binary needs for its blobs.
  `std::filesystem` is in wasi-sdk's libc++. `isatty` and `getenv` exist
  in wasi-libc; the progress line decides it has no terminal and prints
  plain lines.

The compiler's source changes for the build are the three `__wasi__`
branches and the one replacement translation unit, none of them in the
language passes. The gfx binary additionally compiles the packages'
layers with the same toolchain file, through `package.cmake` as on the
desktop, with the browser backend option on.

## 9. The page

A plain page, no framework: a WASI shim, an editor, panes, and for the gfx
binary a canvas.

* **The shim** implements the WASI imports the module uses: `fd_write`
  (stdout and stderr into the panes), `fd_read` (an empty stdin),
  `args_get`, `environ_get` (`GOOSE_GFX_HEADLESS` is not set, so windows
  are real), `clock_time_get`, `random_get`, `proc_exit`, and the file
  calls (`path_open`, `fd_read`, `fd_seek`, `fd_readdir`, `fd_filestat_get`,
  `path_create_directory`, `path_unlink_file`) over an in-memory directory
  tree the page fills before the first run: `/stdlib/*.goose`,
  `/pkg/gfx/*.goose` for the gfx binary, `/samples/` with their data, and
  `/prog.goose` from the editor. A few hundred lines, or a small existing
  shim that does the same. Files a program writes land in the tree and can
  be shown; `-o /out.c` lets the page show the generated C in a tab, which
  is a good part of the demonstration, as are `--check` diagnostics while
  the visitor types and `--specs` and `--stacks`.
* **Running.** The page calls `goose_run("run /prog.goose -O2 -- args")`
  and then `goose_resume()` from `requestAnimationFrame` until the run
  reports it is finished, with the exit status. The VM yields on its time
  budget as well as at frames, so a long-running text program leaves the
  page responsive and Stop is a flag the next resume sees, with no Web
  Worker needed; the text-only binary can still be run in a Worker if a
  page prefers, since nothing in it touches the DOM.
* **Graphics.** The `gfx` and `audio` import modules of section 7.3,
  implemented over a canvas's WebGL 2 context and an `AudioContext`, and
  DOM listeners that queue events for the next resume.

## 10. Changes to the existing code

In order of size, all of them small; the parser, typechecker, optimizer,
BCE, codegen, standard library and `jit.h` are untouched.

1. **`src/runtime/runtime_impl.h`, `runtime_os.h`, `runtime_threads.h`:
   a `GS_INTERP` platform branch** beside `_WIN32` and POSIX. Each existing
   branch of `runtime_impl.h` defines the same eight functions
   (`gs_regions_init`, `gs_native_stack_init`, `gs_native_stack_free`,
   `gs_os_reserve`, `gs_release_region`, `gs_end_first`, `gs_park`, and the
   fault filter or handler); the new branch defines them over two
   interpreter functions, `gs_host_reserve(usable)` returning a fresh stack
   segment and `gs_host_release(base)`, with no fault handler, since the VM
   reports overruns and native overflow itself through `gs_overflow_text`
   and a `gs_native_overflow` this branch adds. The region registry, the
   halving retry of `gs_reserve_region` and the stats stay as they are.
   `runtime_os.h`'s branch declares `gs_os_*` over plain POSIX calls, and
   `runtime_threads.h`'s `gs_os_threads` returns 1. Roughly 100 lines.
2. **`src/main.cpp`**: a `--interp` flag (the default where `GOOSE_NO_JIT`
   is defined), writing `#define GS_INTERP 1` into the assembled text as
   `-D` does and calling `RunInterp` where `RunJit` is called, with the
   builtin packages' thunk tables where the symbol tables go; the
   `__wasi__` branch of `RunOnCompilerStack`. About 40 lines.
3. **`src/builtin.h`**: a `thunks` member of `BuiltinPackage`;
   **`cmake/package.cmake`**: the thunk generator run beside the symbol
   table generator, and the browser-backend option passed to a package's
   CMake project. **`src/shaderc.c`, `src/gfx_blob.h`**: the GLSL ES
   section of the blob.
4. **`src/toolchain.h`, `src/progress.cpp`**: `__wasi__` guards on
   `<thread>`. **`src/process_wasi.cpp`**: new, the stub. **`src/web_entry.cpp`**:
   new, the reactor exports. **`cmake/wasm32-wasi.cmake`**: new.
5. **`CMakeLists.txt`**: the interpreter's sources, `GOOSE_NO_JIT`, the
   Wasm configuration.
6. **`test/run_tests.py`**: `--interp` (section 11).

The assembled C text is the one a JIT run compiles, with one more define.
Nothing about the interpreter leaks into what `-o` writes. The gfx
package's repository gets the browser backend, a build option, and the
header version bump.

## 11. Testing

`goose --interp file.goose` works natively, which is the point of the
design for testing: every fixture and sample the suite runs through TinyCC
(`--jit`, [testing.md](../testing.md)) runs through the interpreter with
`--interp` instead, at `-O0` and `-O2` and with `-DGS_DEBUG=1` on the
selected fixtures, against the same expected output. Interpreter faults
(an access outside a segment, a pointer difference across two) are
findings about codegen or the runtime, not only about the interpreter.
Programs with threads are skipped, as they are where TinyCC cannot run
them. The samples' runner takes the same flag.

A compiler built with the packages runs the `gfx/`, `ui/` and `physics/`
tests through the interpreter and the thunks against the real SDL layer,
headless, as the TinyCC runs do today: they render off screen and read
back pixel-aligned results, so their output is the same whichever backend
runs the Goose side, and the thunk generator, the marshaling and the
yield-free headless path are covered on CI with no browser. Only the
browser backend itself needs a browser, and the page can run the same
headless tests there against the same expected pixels, by hand or under a
headless browser later.

Stacks move in the interpreter whenever a segment doubles, so the suite
exercises relocation on every run without a stress setting. Should the
emitted code or the runtime ever hold a *decoded* pointer across growth,
it cannot: only the VM decodes, and only for one access or one call into
compiled code.

## 12. Limits and follow-ups

* **Speed.** A bytecode interpreter over C runs perhaps 20 to 100 times
  slower than the code TinyCC emits; the samples that time themselves
  (`07_sieve`, `09_sorting`, `10_sudoku`, `21_raytrace` at their default
  sizes) take seconds to minutes, and the graphical ones will run at
  whatever frame rate the interpreted Goose side allows, with the layer's
  draws themselves at native speed. The page should offer smaller
  arguments and say what to expect. `-O2` helps, since inlining removes
  interpreted calls. If it matters later, the usual steps apply in order:
  keep scalar locals in VM registers instead of the frame segment,
  superinstructions for `load`-`op`-`store` on fields, and a cache of the
  last decoded segment.
* **Threads** are out of scope for now, rejected as the JIT rejects them,
  which leaves out samples 23 and 28. Because frames are explicit, workers
  can later be green threads inside one VM: a stack block and `gs_gl` per
  worker, switched on the blocking queue operations, which needs no
  shared-memory Wasm and no cross-origin isolation.
* **Compute shaders** and writable storage are not in WebGL 2. A WebGPU
  backend would lift that, and needs a WGSL emitter in `cute_spirv` or a
  SPIR-V to WGSL step at compile time.
* **Non-builtin packages with plain C sources** (packages.md §5.1) could be
  interpreted with the program like `--include` headers, with no native
  build at all, once the interpreter handles the C they are written in.
  Not planned, but the design allows it.
* **`os`** works to the extent of the in-memory file system: files a
  program writes are visible to it and gone when the page reloads.
* **Memory64** is irrelevant to the interpreter: a 32-bit offset per
  segment holds a 2 GB stack, and the segments together are bounded by the
  module's linear memory, 4 GB.
* **Relation to wasm.md.** The interpreter uses the handle encoding of
  wasm.md §4, so it measures, for free, what a compiled relocating port
  would face: how often stacks move, their high-water marks per program,
  and whether anything observable depends on a base staying put. If a
  compiled port is undertaken later, those numbers are its starting point,
  and the page can switch to it program by program.

## 13. Estimates

Sizes are guesses from the dialect in section 3; the interpreter has not
been started.

| Piece | Where | Lines |
|---|---|---|
| preprocessor | `src/interp/` | 700 to 900 C++ |
| lexer, parser, types, layout, initializers | `src/interp/` | 2,500 to 3,000 C++ |
| lowering to bytecode | `src/interp/` | 1,200 to 1,500 C++ |
| VM, segments, frames, suspension | `src/interp/` | 1,100 to 1,400 C++ |
| prelude and C library table | `src/interp/` | 600 to 800 C++ |
| thunk generator | `cmake/`, a script | 200 to 300 |
| driver flag, runtime branch, `__wasi__` branches, stubs, entry, CMake | existing files and three new ones | 400 to 600 |
| WASI shim and page | `docs/site/` | 800 to 1,200 JS and HTML |
| gfx browser backend, GLSL ES blob section | the gfx package, `shaderc.c` | 2,500 to 3,500 C, 1,000 to 1,500 JS |
| audio browser backend | the gfx package | 300 C, 200 JS |

Roughly 7,000 lines of C++ for the interpreter, none of it in the existing
passes; the gfx package's browser backend is a project of similar size on
its own, and the first binary does not wait for it.
