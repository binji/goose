// Runs goose.wasm under node's WASI (docs/design/interpreter.md §8, step 11
// of §14), the way the page does: instantiates the reactor, writes the
// command line into the module and calls goose_run, then goose_resume
// while the program is suspended. The directory given is preopened as /,
// so paths in the command line are relative to it. A command with --wasm
// compiles the program to a module instead (docs/design/wasm_backend.md
// §6): goose_run returns READY with the module's bytes in memory, and the
// runner instantiates it on goose.wasm's exports and runs its _start,
// then asks goose_wasm_finish what ended it.
//
//     node scripts/wasm_run.mjs <goose.wasm> <dir> <command line...>
//
// WASM_RUN_CWD names the directory, under <dir>, that relative paths the
// program opens resolve against (a sample's data/ files): the module
// changes to it before the command runs (web_entry.cpp, GOOSE_CWD).
import { WASI } from "node:wasi";
import { readFileSync } from "node:fs";
import { argv, exit } from "node:process";

const [, , wasmPath, dir, ...cmd] = argv;
if (!wasmPath || !dir) {
    console.error("usage: node wasm_run.mjs <goose.wasm> <dir> <command line...>");
    exit(2);
}
const SUSPENDED = -2, READY = -3, REPORT = -4;
const env = { GOOSE_PROGRESS: "plain" };
for (const name of ["INTERP_RUN_VAR"]) if (process.env[name] !== undefined) env[name] = process.env[name];
if (process.env.WASM_RUN_CWD) env.GOOSE_CWD = process.env.WASM_RUN_CWD;
const wasi = new WASI({ version: "preview1", args: ["goose"], env, preopens: { "/": dir }, returnOnExit: true });
const { instance } = await WebAssembly.instantiate(readFileSync(wasmPath),
                                                   { wasi_snapshot_preview1: wasi.wasiImport });
wasi.initialize(instance);
const ex = instance.exports;
const { memory, goose_argbuf, goose_run, goose_resume } = ex;
const text = s => {
    const bytes = new TextEncoder().encode(s);
    const buf = goose_argbuf(bytes.length);
    new Uint8Array(memory.buffer, buf, bytes.length + 1).set(bytes);
    return buf;
};
let code = goose_run(text(cmd.join(" ")));
let resumes = 0;
while (code === SUSPENDED) {
    resumes++;
    code = goose_resume();
}
if (code === READY) {
    const bytes = new Uint8Array(memory.buffer, ex.goose_module_ptr(), ex.goose_module_len()).slice();
    if (process.env.WASM_RUN_MODULE) (await import("node:fs")).writeFileSync(process.env.WASM_RUN_MODULE, bytes);
    const imports = { goose: { memory } };
    for (const name of ["native", "access_slow", "static_handle", "func_handle", "info", "stop", "fault"])
        imports.goose[name] = ex["goose_" + name];
    const prog = new WebAssembly.Instance(new WebAssembly.Module(bytes), imports);
    // A trap ends the run; the host says what it recorded, or takes the
    // engine's message as the fault.
    const run = f => {
        try {
            f();
            return null;
        } catch (e) {
            return e instanceof WebAssembly.RuntimeError && /unreachable/.test(e.message) ? "" : `${e.name}: ${e.message}`;
        }
    };
    let trap = run(() => prog.exports._start());
    code = ex.goose_wasm_finish(trap ? text(trap) : 0);
    while (code === REPORT) {
        const name = new TextDecoder().decode(new Uint8Array(memory.buffer, ex.goose_wasm_report_name(), 64)).split("\0")[0];
        const reporter = prog.exports[name];
        trap = reporter ? run(() => reporter(ex.goose_wasm_report_arg())) : "";
        code = ex.goose_wasm_finish(trap ? text(trap) : 0);
    }
}
if (process.env.WASM_RUN_VERBOSE) console.error(`[wasm_run: exit ${code}, ${resumes} resume(s)]`);
exit(code & 0xff);
