// Runs goose.wasm under node's WASI (docs/design/interpreter.md §8, step 11
// of §14), the way the page does: instantiates the reactor, writes the
// command line into the module and calls goose_run, then goose_resume
// while the program is suspended. The directory given is preopened as /,
// so paths in the command line are relative to it. A command with --wasm
// compiles the program to a module instead (docs/design/wasm_backend.md
// §6), which the playground's runCommand runs on goose.wasm's exports;
// WASM_RUN_MODULE names a file to write the module to.
//
//     node scripts/wasm_run.mjs <goose.wasm> <dir> <command line...>
//
// WASM_RUN_CWD names the directory, under <dir>, that relative paths the
// program opens resolve against (a sample's data/ files): the module
// changes to it before the command runs (web_entry.cpp, GOOSE_CWD).
import { WASI } from "node:wasi";
import { readFileSync, writeFileSync } from "node:fs";
import { argv, exit } from "node:process";
import { runCommand } from "../docs/site/play/wasi.mjs";

const [, , wasmPath, dir, ...cmd] = argv;
if (!wasmPath || !dir) {
    console.error("usage: node wasm_run.mjs <goose.wasm> <dir> <command line...>");
    exit(2);
}
const env = { GOOSE_PROGRESS: "plain" };
for (const name of ["INTERP_RUN_VAR"]) if (process.env[name] !== undefined) env[name] = process.env[name];
if (process.env.WASM_RUN_CWD) env.GOOSE_CWD = process.env.WASM_RUN_CWD;
const wasi = new WASI({ version: "preview1", args: ["goose"], env, preopens: { "/": dir }, returnOnExit: true });
const { instance } = await WebAssembly.instantiate(readFileSync(wasmPath),
                                                   { wasi_snapshot_preview1: wasi.wasiImport });
wasi.initialize(instance);
let resumes = 0;
const onReady = bytes => { if (process.env.WASM_RUN_MODULE) writeFileSync(process.env.WASM_RUN_MODULE, bytes); };
const code = runCommand(instance, cmd.join(" "), () => { resumes++; return true; }, { onReady });
if (process.env.WASM_RUN_VERBOSE) console.error(`[wasm_run: exit ${code}, ${resumes} resume(s)]`);
exit(code & 0xff);
