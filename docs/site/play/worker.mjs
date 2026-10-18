// The playground's worker (docs/design/interpreter.md §9): one run of
// goose.wasm off the main thread. The page posts the compiled module, the
// tree's files and the command line; the worker instantiates the module on
// the WASI shim, runs the command, resuming the VM piece after piece
// without pause, and posts the output as it goes and the exit status at
// the end, with the files the page asked to read back. Stop is the page
// terminating the worker, so nothing here reads a second message, and the
// VM's step budget matters only as the cadence of the output flushes.
//
// The worker's port is its global in a browser; under node (wasi_test.mjs)
// it is worker_threads' parentPort, which takes the same calls.
import { WasiShim, Tree, SUSPENDED } from "./wasi.mjs";

const port = typeof self !== "undefined" ? self : (await import("node:worker_threads")).parentPort;

// Output is decoded as a stream, so a character split across two writes
// still comes out whole, and posted in batches: at every piece of the VM's
// work, and sooner when a program prints a lot.
const FLUSH_AT = 1 << 16;
const outDec = new TextDecoder(), errDec = new TextDecoder();
let out = "", err = "";
function flush(final = false) {
    if (final) { out += outDec.decode(); err += errDec.decode(); }
    if (out || err) port.postMessage({ type: "output", out, err });
    out = ""; err = "";
}

function run({ module, files, cmdline, stdin = "", env = {}, readback = [] }) {
    const tree = new Tree();
    for (const [path, text] of Object.entries(files)) tree.writeFile(path, text);
    const shim = new WasiShim({
        tree, stdin,
        stdout: b => { out += outDec.decode(b, { stream: true }); if (out.length >= FLUSH_AT) flush(); },
        stderr: b => { err += errDec.decode(b, { stream: true }); if (err.length >= FLUSH_AT) flush(); },
        env: { GOOSE_PROGRESS: "plain", ...env },
    });
    const done = code => {
        flush(true);
        const got = {};
        for (const path of readback) {
            const text = tree.readText(path);
            if (text !== undefined) got[path] = text;
        }
        port.postMessage({ type: "done", code, files: got });
    };
    let instance;
    try {
        instance = new WebAssembly.Instance(module, shim.imports);
        shim.start(instance);
    } catch (e) {
        err += `cannot start goose.wasm: ${e.message}\n`;
        return done(1);
    }
    const { memory, goose_argbuf, goose_run, goose_resume } = instance.exports;
    const line = new TextEncoder().encode(cmdline);
    const buf = goose_argbuf(line.length);
    new Uint8Array(memory.buffer, buf, line.length + 1).set(line);
    // An exit inside the module is a ProcExit the shim recorded; anything
    // else thrown is the module failing.
    const call = f => {
        try {
            return f();
        } catch (e) {
            if (shim.exitCode === null) err += `goose.wasm failed: ${e.message}\n`;
            return shim.exitCode ?? 1;
        }
    };
    let code = call(() => goose_run(buf));
    if (code === SUSPENDED) port.postMessage({ type: "status", text: "running…" });
    while (code === SUSPENDED) {
        flush();
        code = call(goose_resume);
    }
    done(code);
}

port.addEventListener("message", e => run(e.data));
