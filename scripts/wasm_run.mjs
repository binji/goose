// Runs goose.wasm under node's WASI (docs/design/interpreter.md §8, step 11
// of §14), the way the page does: instantiates the reactor, writes the
// command line into the module and calls goose_run, then goose_resume
// while the program is suspended. The directory given is preopened as /,
// so paths in the command line are relative to it.
//
//     node scripts/wasm_run.mjs <goose.wasm> <dir> <command line...>
import { WASI } from "node:wasi";
import { readFileSync } from "node:fs";
import { argv, exit } from "node:process";

const [, , wasmPath, dir, ...cmd] = argv;
if (!wasmPath || !dir) {
    console.error("usage: node wasm_run.mjs <goose.wasm> <dir> <command line...>");
    exit(2);
}
const SUSPENDED = -2;
const wasi = new WASI({ version: "preview1", args: ["goose"], env: { GOOSE_PROGRESS: "plain" },
                        preopens: { "/": dir }, returnOnExit: true });
const { instance } = await WebAssembly.instantiate(readFileSync(wasmPath),
                                                   { wasi_snapshot_preview1: wasi.wasiImport });
wasi.initialize(instance);
const { memory, goose_argbuf, goose_run, goose_resume } = instance.exports;
const line = new TextEncoder().encode(cmd.join(" "));
const buf = goose_argbuf(line.length);
new Uint8Array(memory.buffer, buf, line.length + 1).set(line);
let code = goose_run(buf);
let resumes = 0;
while (code === SUSPENDED) {
    resumes++;
    code = goose_resume();
}
if (process.env.WASM_RUN_VERBOSE) console.error(`[wasm_run: exit ${code}, ${resumes} resume(s)]`);
exit(code & 0xff);
