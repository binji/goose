# A WebAssembly backend from the interpreter's bytecode

<details>
<summary>Table of contents</summary>

- [1. The goal](#1-the-goal)
- [2. Why the bytecode is the right input](#2-why-the-bytecode-is-the-right-input)
- [3. What the measurements say](#3-what-the-measurements-say)
- [4. The design](#4-the-design)
- [5. Memory: the same handles, compiled](#5-memory-the-same-handles-compiled)
- [6. The host contract](#6-the-host-contract)
- [7. Stopping, faults and suspension](#7-stopping-faults-and-suspension)
- [8. Optimizations, in order of return](#8-optimizations-in-order-of-return)
- [9. Expected speed](#9-expected-speed)
- [10. Changes to the existing code](#10-changes-to-the-existing-code)
- [11. Testing](#11-testing)
- [12. Estimates](#12-estimates)
- [13. The plan](#13-the-plan)

</details>

A proposal, not implemented. [interpreter.md](interpreter.md) put the
compiler in a browser page and runs programs there by interpreting the C
that codegen emits, through a typed stack bytecode (`src/interp/lower.h`,
`bytecode.h`) and a `switch` loop over it (`vm.h`). That was the smallest
change that made the page work, and it is 15 to 100 times slower than the
code TinyCC emits for `goose run`. This note designs the next step: a
backend that takes the bytecode the interpreter already produces and emits
a WebAssembly module for the browser's own compiler to run, keeping the
interpreter's memory model, its C library and its native-package thunks as
they are. The speed figures in section 3 are measurements taken on October
19, 2026, on a laptop on mains power (on battery every column was about
twice as slow and the ratios the same); those in section 9 are estimates
derived from them, followed by what the implementation measured.

**Status.** Steps 1 to 6 of the plan (§13) and the first three items of
§8 are implemented, in `src/interp/wasmgen.h` and the host side of
`vm.h` and `interp.cpp`: `goose.wasm` compiles a program's bytecode to a
module and runs it on its own exports under `--wasm`, natively `-o`
writes the module, `test/wasm_backend.py` runs the interpreter's C
fixtures through it, and `test/run_tests.py --wasm` runs the text areas'
fixtures and the samples. The playground's Run compiles by default, with
"interpreted" a choice on the page; the compiled path is one function of
the shim module, `runCompiled` in `docs/site/play/wasi.mjs`, which the
worker and `scripts/wasm_run.mjs` share. Two departures from the design as written: the
operand stack is followed symbolically during emission, so that a frame
record's or a static object's fields are accessed without a decode
(§4 item 7 covered only the function's own scalar slots); and hoisting
keeps a pair of locals per origin and region, refreshed after calls, in
place of the sync-extent analysis sketched in §8, which needed none of
codegen's analyses. Items 4 and 5 of §8 (typed slots) are not started,
nor is the frame yield of §7.

## 1. The goal

A `Run` on the playground that runs the program at a speed within a small
factor of a native build, for everything the interpreter runs today:
samples 01 to 22 and 24 to 26 through `goose.wasm`, and the graphical ones
through `goose-gfx.wasm` once section 7's suspension question is settled.
Nothing in the compiler before lowering changes; nothing in the generated
C changes; the interpreter stays, as the sanitizer it doubles as and as
the fallback for programs the backend does not yet take.

## 2. Why the bytecode is the right input

The question interpreter.md §2 answered for an interpreter has the same
answer for a compiler. The typed AST would need a second backend that
reproduces every layout decision of codegen; codegen itself writes C text
while it runs its analyses, so a second emitter inside it is a rewrite;
and clang in the browser is tens of megabytes and gives back the problem
[wasm.md](wasm.md) works out, growth checks and relocation in the emitted
code. The bytecode has none of these costs:

* It already holds the *whole* program: the runtime, the generated types
  and bodies, and any `--include` header, parsed, laid out under
  `#pragma pack(1)` and lowered to operations whose types are decided
  (`add.i32`, `load.u8`, `cvt.f64.i32`, `ptradd`). The emitter never
  looks at a C type again.
* Its memory model is the one a relocating Wasm port needs (wasm.md §4),
  and it is already paid for: a pointer is a handle, segment id above and
  offset below, and only loads, stores and calls into compiled code decode
  it. The stacks that grow and move under the program do so inside the
  host's `AccessSlow`, which the compiled code calls on the slow path
  exactly as the VM does.
* The C library adapters (`clib.h`, some 150 functions) and the packages'
  thunks (`goose_thunks_<name>.c`) take their arguments as 64-bit slots
  and decode handles through the VM. Compiled code hands them the same
  slots.
* It is a stack machine with a statically known stack height at every
  instruction (lowering tracks it to size `maxstack`), and Wasm is a stack
  machine too. Most instructions translate one to one.

What the bytecode lacks, and the backend must add, is structured control
flow (its jumps are arbitrary, Wasm's are blocks and loops), a decision
about which values live in Wasm locals rather than in the frame segment,
and the structure of a Wasm binary.

## 3. What the measurements say

Four samples time their own kernels on stderr. Each was run under every
backend at hand, at `-O2`, on the machine the interpreter note's numbers
come from (Linux, x86-64, node 26, wasi-sdk 34). The three rightmost
columns are the emitted C built to `wasm32-wasip1` with wasi-sdk's clang
and run under node, over a stand-in host for the runtime's `GS_INTERP`
branch that gives each data stack one `calloc`'d block of its reservation
(16 MB) and never grows it: no handles, no checks, so the `-O2` column is
the *ceiling* for any backend that compiles this program for this engine,
not a thing that can ship. (The same stand-in built natively runs within
a few milliseconds of the real runtime on every kernel, so the stand-in
costs nothing of its own; the native column is the real runtime, `goose
build`.)

| kernel | TinyCC, native | clang -O2, native | interpreter, native | interpreter, in goose.wasm under node | C to Wasm, clang -O2 | clang -O1 | clang -O0 |
|---|---|---|---|---|---|---|---|
| `07_sieve` | 66 ms | 30 ms | 1,295 ms | 1,378 ms | 71 ms | 61 ms | 97 ms |
| `09_sorting` merge sort | 273 | 53 | 6,675 | 7,224 | 102 | 107 | 296 |
| `09_sorting` std sort | 280 | 23 | 5,780 | 6,503 | 58 | 75 | 443 |
| `09_sorting` insertion | 394 | 36 | 16,794 | 19,489 | 287 | 314 | 419 |
| `10_sudoku` hard | 22.5 | 2.5 | 252 | 416 | 3.3 | 3.9 | 9.8 |
| `10_sudoku` sparse | 807 | 101 | 9,700 | 11,924 | 98 | 106 | 276 |
| `21_raytrace` | 34 | 4.4 | 675 | 1,070 | 7.0 | 7.5 | 31 |

Three things follow.

* **The interpreter in the browser is 19 to 153 times slower than compiled
  Wasm of the same program**, and the gap is widest on call-heavy and
  arithmetic code (sorting, sudoku, the raytrace) and narrowest on the
  memory-bound sieve. Wasm at clang's `-O2` runs between 1.0 and 2.5
  times clang's native code on six of the seven kernels (8 times on the
  insertion sort), and *beats TinyCC's native code* on everything but the
  sieve, where it is level, by 1.4 to 8 times. A compiled playground can
  be faster than `goose run` on the desktop.
* **Where the interpreter's time goes** (perf on `21_raytrace`): 72% in
  the dispatch loop itself, 10% in `Access`, 7% in `PtrAdd`, 2.5% in
  `Enter`. A compiler removes the first entirely and turns the second into
  a compare and a branch.
* **The handle decode is cheap once compiled.** A micro-benchmark in C,
  built to Wasm the same way, of the sieve's inner loop storing through a
  64-bit handle with the VM's `base + size` table and bounds check on
  every store, against the raw store:

  | | raw | checked on every access | checked, base and size hoisted out of the loop |
  |---|---|---|---|
  | sieve stores, Wasm under node | 64 ms | 114 ms | 61 ms |
  | sieve stores, native | 41 | 46 | 34 |
  | linked-list walk of a million nodes, 20 passes, Wasm | 1,972 | 2,033 | n/a |

  A decode per access costs 1.8 times on a loop that is nothing but
  stores, 3% on pointer chasing (the cache miss is the cost), and nothing
  when hoisted. This is the 1.1 to 1.5 wasm.md §4 estimated, now measured
  at both ends.

The `-O1` and `-O0` columns matter for the estimate in section 9. Clang
at `-O0` keeps every local in memory and inlines nothing, which is a
translation of the bytecode with every local left in the frame segment,
except that it still addresses memory directly; `-O1` keeps locals in
registers and inlines the small helpers but does no loop transformation,
which is what a translation with scalar locals in Wasm locals gets from
the engine's optimizing tier. The two bracket a straightforward backend,
before the decode's cost.

## 4. The design

A fourth piece in `src/interp/`, `wasmgen.h`, that takes the `Program`
lowering produced and writes a module. In the driver it sits where the VM
does: `RunInterp` lowers as before and then, when the host has asked for
compiled code, emits the module and hands it back instead of running the
loop. The pieces, in the order the bytes flow:

1. **Reachability.** The unit holds the whole runtime (170 to 190 function
   bodies for a sample, of which the program's own are 50 to 60). From
   `main`, the static initializer and every `OP_FUNC`, mark the functions
   reached by `OP_CALL` and `OP_FUNC`; emit only those. `OP_CALLI` can
   reach only functions whose handle some `OP_FUNC` made.
2. **The control-flow graph** of each function: leaders at jump targets
   and after jumps, edges from `JMP`, `JZ`, `JNZ`, `JCMP` and fallthrough,
   `RET` and `UNREACHABLE` ending a block. The operand stack's height at
   each block's entry comes from one pass in reverse postorder, as
   lowering computed it in emission order; the two agree because
   statements leave the stack where they found it and `?:`, `&&` and `||`
   join at equal heights (lowering's `Effect`).
3. **Structuring.** Wasm has no `goto`. The CFGs codegen's C produces are
   reducible (its gotos are scope exits and the flat `if`, forward or to
   a loop's head; `switch` lowers to compare chains), so the dominator
   tree and reverse postorder give a nesting of `block` and `loop` with a
   `br` per edge, by the algorithm of Ramsey's *Beyond Relooper* (2022):
   a `loop` for each header with a back edge, a `block` ending at each
   merge node (two or more forward predecessors), placed in dominator
   order. A function whose CFG turns out irreducible gets the fallback, a
   `loop` over a `br_table` on a block index, which is slow but never
   wrong; the test in section 11 counts how often it happens, and the
   expected count is zero.
4. **Values across edges.** Wasm's stack inside a block starts empty, so a
   jump taken with values below the condition (the `?:` arms, the
   short-circuit operators) cannot leave them on the stack. The emitter
   stores them into scratch locals at the jump and reloads them at the
   target; it knows the height at both. This is rare and the engine's
   register allocator makes it free; multi-value block types could carry
   them instead and are not worth the complexity.
5. **Slots.** A bytecode slot is 64 bits; the module keeps that: `i64`
   for every integer and pointer slot, `f64` for every float slot, so a
   `Function`'s signature is `(i64 | f64) * nargs -> result`, and
   `OP_CALLI`'s `call_indirect` type follows from its arity and the slot
   types. `Normalize` after an arithmetic operation in a narrow type is
   `i64.extend8_s`, `i64.extend16_s`, `i64.extend32_s` or an `i64.and`;
   an `f32` operation is the `f64` operation followed by `f32.demote_f64`
   and `f64.promote_f32`, which is exactly the VM's rounding. Typed slots
   (`i32` where the type is 32 bits or less, `f32` for floats) are a later
   refinement (section 8); on a 64-bit host the engine's `i64` operations
   are native and the extends are one instruction.
6. **Locals.** A scalar local the function only ever reaches through
   `LLOAD`, `LSTORE` and `LSTOREK` becomes a Wasm local, which the engine
   keeps in a register. Everything else, records and every local whose
   address some `OP_LOCAL` takes, stays in the frame segment at the offset
   lowering gave it, addressed as `framebase + fp + off` with `fp` a Wasm
   local set on entry. Lowering gains a per-function list of its locals,
   offset, size and whether addressed, so the emitter need not infer
   extents from `OP_LOCAL`s (section 10).
7. **Frames.** The frame segment is allocated at its limit once and never
   moves, so frame accesses need no decode; `frametop` is a Wasm global.
   Entry bumps it by `framesize` and compares against the limit, which is
   the native-overflow report; exit restores it. A handle to a frame slot
   (`OP_LOCAL`) is `Handle(frameseg, fp + off)` as in the VM and decodes
   through the segment table like any other, whose entry for the frame
   segment has `size = limit`. Record parameters, which the VM copies into
   the callee's frame on entry, are a `memory.copy` from the decoded
   argument into the frame slot.
8. **Calls.** `OP_CALL` to a function of the unit is `call`; to an
   external it is the `native` import of section 6 with the arguments
   spilled to a scratch area at the frame top; `OP_CALLI` is
   `call_indirect` through the segment table's entry for the function
   handle, which holds the table index (section 5). The `mem*` externals
   the runtime leans on (`memcpy`, `memmove`, `memset`, `memcmp`) are
   emitted inline as two decodes and `memory.copy` or `memory.fill`, as
   `OP_COPY` and `OP_ZERO` are.
9. **The binary.** Type, import, function, table, memory import, global,
   export, element, data and code sections, a passive data segment per
   static object with bytes, and a name section so that the engine's
   stack traces name the function a fault happened in. LEB128 and section
   framing are two hundred lines; no validator is needed, the engine
   validates at instantiation and the test suite in section 11 keeps it
   honest.

## 5. Memory: the same handles, compiled

Section 5 of interpreter.md stands unchanged: a handle is a segment id in
the high 32 bits and a byte offset in the low 32, and every pointer
operation but an access is integer arithmetic on it. What changes is that
the decode is now emitted code, so the host keeps the segment table in a
form the code can read:

* **The segment table** is a flat array in linear memory of `{ base: u32,
  size: u32 }` per id, at a fixed address the module imports as a global,
  with a fixed capacity (a million entries, 8 MB of address space that is
  touched as used). The VM's `segs` vector stays as it is for everything
  the host does; `NewSeg`, `Grow`, `free` and `Finish` mirror base and
  size into the flat table. A freed segment's size is 0, so every access
  to it takes the slow path, which reports the use after free as the VM
  does. A function segment's `base` is the function's index in the
  module's table, so `call_indirect` reads its callee from the same entry.
* **The decode** on an access of `n` bytes at `h`: `id = wrap(h >> 32)`,
  `off = wrap(h)`, `size = load(tab + id * 8 + 4)`, and if `off + n >
  size` then `access_slow(h, n)`, else `load(tab + id * 8) + off`. Twelve
  Wasm instructions, of which the two loads are what hoisting removes.
  `id == 0` has `size = 0`, so a null access is the slow path's fault.
* **Growth** stays the host's: `access_slow` is the VM's `AccessSlow`,
  which doubles a stack segment up to its limit, possibly moving it, and
  updates the table before returning the new address. Compiled code holds
  no decoded pointer across anything that can grow, except where the
  hoisting of section 8 refreshes it from the table on the slow path.
* **Pointer arithmetic is unchecked.** The VM's `PtrAdd` faults when an
  offset leaves its segment's reach; the compiled `OP_OFFSET`, `OP_PTRADD`
  and `OP_PTRSUB` are an `i64.add` and a multiply, and an offset that
  wraps into the id is caught, if at all, at the next access. The
  interpreter remains the run that checks arithmetic; `--interp` keeps
  it.
* **`OP_PTRDIFF`** is the difference of the low halves, divided by the
  size; the VM's check that both name one segment is dropped with the
  same justification.

The frame segment and the static objects are segments like any other;
the decode does not know them apart. Only the emitter does, when it
addresses a frame slot of the function it is emitting directly.

## 6. The host contract

The module runs beside the compiler, in the same Web Worker, on the
compiler module's memory. It imports from a module named `goose`:

| import | what it is |
|---|---|
| `memory` | `goose.wasm`'s linear memory, which the reactor already exports for the WASI shim |
| `segtab` (global i32) | the segment table's address |
| `frameseg` (global i64), `framebase`, `framelimit` | the frame segment's handle, address and limit |
| `seg_new(kind, size) -> id` | a static object's or a function handle's segment, called from the module's `start` function |
| `access_slow(h, n) -> addr` | growth, and every fault |
| `native(idx, args, nargs) -> i64` | the C library adapters and the thunks, by index into a table the compiler fixes when it emits the module |
| `stop(status)` | `exit`, and the end of `main` |

and exports `main`, the static initializer as `start`, and the runtime's
two overflow reporters. The worker instantiates it with an import object
built from `goose.wasm`'s exports: `goose_run` returns a new status,
`kWasmReady` (-3), with the module's bytes at an address and length the
worker reads out of memory; the worker instantiates and calls `main`, and
whatever ends it (section 7) is read back through one more export,
`goose_stop_reason()`. Every entry in the table is a Wasm-to-Wasm call
with no JavaScript between, which is what keeps `memcpy` and `fwrite`
cheap. A `goose.wasm` built without the backend, or asked with
`--interp`, runs the VM as today.

Natively there is no engine in the process, and none is added: the
backend's native form is `-o prog.wasm`, which writes the module for
inspection (`wasm-objdump` reads the name section) and for the test
runner, which runs it the way the page does, through node with
`goose.wasm` as the host (section 11). The module is self-contained in
the sense that matters for this: its statics are passive data segments
initialized at `start` by `seg_new` and `memory.init`, and its handles
are globals set there, so the same bytes run in any host that implements
the seven imports.

## 7. Stopping, faults and suspension

**Every stop is a trap with a recorded reason.** The VM today ends a run
with C++ exceptions (`Exit`, `Fault`, `Reroute`) thrown from an adapter
through the loop. Compiled frames sit between the adapter and the
driver, and threading a C++ exception through another module's frames is
possible under the exception-handling proposal the build already uses,
but needless: the host records what happened and executes
`__builtin_trap()`, the engine unwinds the program module with a
`RuntimeError`, and the worker, which called `main`, asks
`goose_stop_reason()`:

* `exit(n)` and a normal return: the status.
* A fault (an access past a segment, a use after free, a null access, a
  division by zero, which Wasm traps on by itself): the message, which the
  worker prints with the engine's stack trace, named by the name section,
  in place of the VM's interpreted call stack.
* A data stack past its reservation: the base of the region, with which
  the worker calls the exported `gs_data_overflow`, a plain function that
  prints the runtime's message from the region table and exits. Native
  call stack overflow is the frame limit in the entry prologue, and also
  the engine's own stack limit, which arrives as a `RangeError` the worker
  reports with the same message; the frame limit is set so that the
  prologue fires first on recursion of ordinary frames.

**Suspension** is the one thing the interpreter does that compiled code
cannot do as a matter of course. The VM yields on a step budget, to keep
a page responsive, and at a yielding thunk (`gs_gfx_frame`), to let the
browser's animation callback run between frames (interpreter.md §7.2).

* The budget is unnecessary: the playground already runs each command in
  a Worker of its own, Stop terminates the worker, and output reaches the
  page through the shim's `fd_write`, which can post each write as it
  comes rather than at the end of a piece. A compiled run is simply
  synchronous in the worker.
* The frame yield is real. Three ways, from least to most work, all
  compatible with the module as designed, since the choice is the host's:
  (a) **JSPI**, the JavaScript Promise Integration API, wraps the
  `native` import as `WebAssembly.Suspending`; a yielding thunk returns a
  promise the animation callback resolves, and the engine parks the
  program's stack. Chrome ships it; the other engines' status should be
  checked when section 13's last step is reached. (b) **An OffscreenCanvas
  in the worker** with a `SharedArrayBuffer` and `Atomics.wait` in the
  frame thunk, which needs the page cross-origin isolated, a header
  GitHub Pages does not send but a service worker can add. (c)
  **Asyncify restricted to the yield's call chain**: the emitter knows
  which functions can reach a yielding thunk (a call-graph walk from the
  thunk table's `yields` entries), typically `main` and the frame loop's
  few callers, and emits only those in an unwind-and-rewind form that
  saves their Wasm locals to the frame segment at the call. This costs
  nothing elsewhere and needs no browser feature, and it is the one the
  compiler controls. The plan takes (a) first where it exists and (c) as
  the portable answer; until one of them is in, `goose-gfx.wasm` compiles
  a program whose unit calls no yielding thunk and interprets the rest,
  which the thunk table tells it at emission time.

## 8. Optimizations, in order of return

None of these is needed for the backend to be correct, and the first two
are what section 9's lower estimate assumes.

1. **Scalar locals in Wasm locals** (section 4, item 6). Without it every
   local is a frame-segment load and store, which the `-O0` column of
   section 3 puts at 1.3 to 6 times slower than the `-O1` column, mostly
   2 to 3.
2. **Dead function elimination** (item 1): compile time and module size,
   not speed.
3. **Decode hoisting.** For a loop whose body has no call, each handle
   that is loop-invariant (a Wasm local assigned only outside the loop, or
   a frame slot never stored in the loop) gets a pair of locals, `base`
   and `size`, loaded from the table once before the loop; an access
   derived from it by `OP_OFFSET` or `OP_PTRADD` keeps its `off + n <=
   size` compare but reads no table, and the slow path, which may have
   moved the segment, reassigns both locals before continuing. The
   sieve's loop goes from the "checked" row of section 3's second table
   to the "hoisted" one. A loop with calls refreshes the pair after each
   call instead, which is still two loads per call rather than two per
   access. This is the extent analysis wasm.md §4 wanted from codegen,
   done once on the bytecode where every access is explicit.
4. **Typed slots**: `i32` for types of 32 bits or less and `f32` for
   floats, removing the extends and demotes. A pass over the symbolic
   stack assigns each producer the type its consumer names (`OP_PUSH` has
   none of its own). Worth doing when a profile shows the extends, and
   not before.
5. **Fused compare-and-branch** is already in the bytecode (`OP_JCMP`),
   and `i64.lt_s; br_if` is what it becomes.

What the engine does on its own: register allocation, instruction
selection, inlining of small functions (the runtime's `static inline`
helpers, which lowering made separate functions, come back inline in the
optimizing tier), and bounds checks on the memory itself at no cost on
64-bit hosts.

## 9. Expected speed

Against the interpreter in the browser, from the ratios of section 3's
table. The first column is a translation that keeps scalar locals in Wasm
locals and decodes on every access: its low end is the `-O0` column's
ratio divided by the decode's worst measured cost (1.8), its high end the
`-O1` column's ratio with no decode cost, as on pointer chasing. The
second column adds hoisting and typed slots, which take the decode out of
the loops that are nothing but accesses, and approaches the `-O2` ceiling
from below.

| kernel | backend with items 1 and 2 of §8 | with hoisting and typed slots | ceiling (clang -O2) |
|---|---|---|---|
| `07_sieve` | 8 to 23x | 15 to 19x | 19x |
| `09_sorting` | 8 to 87x per kernel, 16 to 67x on the three together | 50 to 100x | 70 to 110x |
| `10_sudoku` sparse | 24 to 112x | 80 to 120x | 122x |
| `21_raytrace` | 19 to 143x | 100 to 150x | 153x |

The brackets are wide because the two proxies are far apart on the
sorting kernels and the raytrace, where clang's inlining of the runtime's
helpers is most of the difference and the engine's own inlining is the
unknown. In absolute terms the first column takes the sorting sample from
33 seconds in the browser to between half a second and two, the sudoku
from 12 to under half a second, and the sieve and the raytrace from over
a second to a tenth of one. The second
column is within about 1.5 times of what clang makes of the same C, which
is as far as a backend without an optimizer of its own should expect to
go; the remaining gap is clang's alias analysis and loop transformations,
which the engine does not attempt. Against the desktop, the first
column's midpoint already matches or beats TinyCC's native code on every
kernel but the sieve.

The uncertainty is in the engine's treatment of the decode's table loads
and compares, which section 3's micro-benchmark measured in isolation but
not in the runtime's helpers, where `gs_rref` chasing and the pool
freelists decode several times per operation. If the first column's low
end is what lands, hoisting is the fix and its mechanism is already
decided. The Chrome figures on the machine these were measured on are a
further four times slower than node's for reasons outside the engine
(the interpreter note's §9 records the observation); a compiled module
is affected the same way, so the ratios hold while the absolute figures
there do not.

### Measured

The same kernels after each step, under node, against the interpreter in
`goose.wasm` and the ceiling of section 3:

| kernel | interpreter | translation (§4) | + locals (§8.1) | + frame records | + statics, popcount | + hoisting (§8.3) | ceiling |
|---|---|---|---|---|---|---|---|
| `07_sieve` | 1,378 ms | 350 ms | 262 ms | 178 ms | 180 ms | 174 ms | 71 ms |
| `09_sorting` merge sort | 7,224 | 1,077 | 952 | 577 | 576 | 489 | 102 |
| `09_sorting` std sort | 6,503 | 1,458 | 1,115 | 727 | 731 | 771 | 58 |
| `09_sorting` insertion | 19,489 | 1,983 | 1,629 | 1,051 | 1,041 | 705 | 287 |
| `10_sudoku` sparse | 11,924 | 1,089 | 681 | 675 | 512 | 485 | 98 |
| `21_raytrace` | 1,070 | 245 | 217 | 111 | 104 | 107 | 7.0 |

Eight to twenty-eight times the interpreter, and within 2.5 to 5 times of
the ceiling on all but the raytrace, which at 15 times off points at the
i64 slots and the reinterprets around every float operation, item 4's
case. The first column fell below the table above's lower estimate
because the generated C reads a slice's data pointer through its struct
in the frame on every element access, a decode of the frame segment the
estimate did not count; the third column is that decode removed. The
sieve's inner loop still spends some twenty instructions on the handle
arithmetic and the hoisted check around its one byte store, which is
where the next step lies.

## 10. Changes to the existing code

1. **`src/interp/wasmgen.h`**, new: the emitter of section 4.
2. **`src/interp/bytecode.h`, `lower.h`**: `Function` gains `locals`, a
   list of `{ off, size, t, addressed }` that lowering fills as it
   allocates slots and sets `addressed` where it emits `OP_LOCAL` for
   one. Twenty lines.
3. **`src/interp/vm.h`**: the flat segment table beside `segs`, kept in
   step by `NewSeg`, `Grow`, the `free` adapter and `Finish`; the frame
   segment allocated at its limit when a compiled run is asked for; the
   `native` dispatch by index over the library and thunk tables; the stop
   reason. A hundred lines, none in the loop.
4. **`src/interp/interp.cpp`, `interp.h`**: `kWasmReady` and the
   compiled-run request beside `InterpResumable`; `RunInterp` returning
   the module's bytes; the `--interp` flag's meaning narrowed to "the VM,
   not the backend" where both exist.
5. **`src/main.cpp`**: `-o prog.wasm` writing the module natively.
6. **`src/web_entry.cpp`**: `goose_module_bytes`, `goose_stop_reason`.
7. **`docs/site/play/worker.mjs`**, `scripts/wasm_run.mjs`: instantiate
   the program module on the host's exports, run `main`, report the stop
   reason. Perhaps 120 lines between them.
8. **`test/run_tests.py`**: `--wasm` (section 11).

The parser, typechecker, optimizer, BCE, codegen, the runtime and the
standard library are untouched, as they were by the interpreter.

## 11. Testing

The interpreter's suite is the backend's suite. `test/interp/run/*.c`
are C programs with expected output that reach every instruction and
every kind of segment, growth and fault; each runs through node with
`goose.wasm` as the host and the module the backend wrote, by the runner
`test/wasm_build.py` already uses for two programs, and its expected
output is the VM's. Then the fixture areas interpreter.md §14 step 7
widened over, under `--wasm` in place of `--interp`, with faults compared
by message (the VM's stack listing is not expected). A unit test over the
emitter counts the functions that fell back to the dispatcher loop, and
expects none; another checks that `-o prog.wasm` on the samples
validates, with `wasm-tools validate` where it is installed and
instantiation under node otherwise.

The two backends diverging on a fixture is a finding about one of them,
and the VM, which checks more, is the reference. Programs that need a
yielding thunk run under `--interp` until section 7's answer is in.

## 12. Estimates

Sizes guessed from the pieces in section 4 and the sizes the interpreter's
pieces came to.

| piece | lines |
|---|---|
| binary writer: sections, LEB128, names | 250 to 350 C++ |
| CFG, dominators, structuring, stack heights | 500 to 700 C++ |
| instruction emission, locals, calls, decodes | 900 to 1,300 C++ |
| decode hoisting and typed slots (§8, items 3 and 4) | 400 to 600 C++ |
| host side: segment table, native dispatch, stop reason, entry points | 250 to 350 C++ |
| worker and node runner | 100 to 150 JS |
| test runner mode, fixtures' expectations | 150 to 250 |

About 2,500 to 3,500 lines for the first column of section 9, and 3,000
to 4,000 for the second, against the interpreter's 6,500. The frame
yield's portable form (§7, Asyncify on the call chain) is a further 400
to 600 lines in the emitter and is independent of the rest.

## 13. The plan

Each step is one commit, or a few, with the test that shows it working.

1. **The binary writer and a module with one function.** `wasmgen.h`
   emits a module whose `main` returns a constant; `-o prog.wasm`;
   `wasm_run.mjs` instantiates it on `goose.wasm`'s exports. Test: node
   runs it and reports the status.
2. **Straight-line code, locals and calls.** Every instruction but the
   jumps; `Function::locals`; the segment table, decodes and
   `access_slow`; `native` with the spilled arguments; `seg_new` and the
   statics at `start`. Test: the arithmetic, pointer and static-data
   fixtures of `test/interp/run/`, which are straight-line, through node.
3. **Control flow.** The CFG, stack heights, Ramsey's structuring, the
   dispatcher fallback, values across edges. Test: the rest of
   `test/interp/run/`, and the fallback counter at zero.
4. **Stops.** Faults, `exit`, the overflow reporters, the trap-and-reason
   protocol. Test: the fault fixtures, compared by message.
5. **Goose programs.** `run_tests.py --wasm` over `test/codegen/` first,
   then the other areas as in interpreter.md step 7, fixing what each
   finds. The four timed samples give the first row of section 9's table
   as measurements.
6. **The page.** `worker.mjs` takes the compiled path by default and
   `--interp` as an option; the generated-C tab stays.
7. **Hoisting and typed slots**, each measured on the four samples
   before and after.
8. **The frame yield**: JSPI in the worker where the engine has it, and
   the restricted Asyncify after, measured on `27_gfx_cube`.

Steps 1 to 4 are sequential; the host-side work of step 2 can be written
beside step 1; step 7 and step 8 are independent of each other and of the
page.
