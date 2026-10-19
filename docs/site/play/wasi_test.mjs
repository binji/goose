// Tests of the playground's WASI shim (wasi.mjs) under node: the file calls
// through a memory of their own, as wasi-libc makes them, and, where a
// goose.wasm is given, the hello-world fixture and the generated-C tab's
// command through the shim end to end, and then the same through the
// playground's worker (worker.mjs) on node's worker threads, whose port
// takes the browser's messages, interpreted and compiled to WebAssembly
// (--wasm).
//
//     node docs/site/play/wasi_test.mjs [goose.wasm]
//
// Exits 0 when everything passes, 1 on a failure.
import { readFileSync } from "node:fs";
import { argv, exit } from "node:process";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";
import { WasiShim, Tree, runCommand } from "./wasi.mjs";

let failures = 0;
function check(what, ok, detail = "") {
    if (ok) console.log(`ok   ${what}`);
    else {
        failures++;
        console.log(`FAIL ${what}${detail ? ": " + detail : ""}`);
    }
}

const enc = new TextEncoder(), dec = new TextDecoder();

// A shim over a memory of its own: paths and buffers are written into it
// and the import functions called as the module would call them.
function harness() {
    const memory = new WebAssembly.Memory({ initial: 1 });
    const out = [], err = [];
    const shim = new WasiShim({ stdout: b => out.push(dec.decode(b)), stderr: b => err.push(dec.decode(b)),
                                stdin: "line one\nline two\n", env: { GOOSE_PROGRESS: "plain" } });
    shim.memory = memory;
    const w = shim.imports.wasi_snapshot_preview1;
    let top = 1024;
    const alloc = n => { const p = top; top += (n + 7) & ~7; return p; };
    const put = text => { const b = enc.encode(text); const p = alloc(b.length); new Uint8Array(memory.buffer, p, b.length).set(b); return [p, b.length]; };
    const view = () => new DataView(memory.buffer);
    const iov = (ptr, len) => { const p = alloc(8); view().setUint32(p, ptr, true); view().setUint32(p + 4, len, true); return p; };
    return { memory, shim, w, alloc, put, view, iov, out, err };
}

{
    const h = harness();
    const { w, alloc, put, view, iov } = h;
    // prestat: fd 3 is the root, named "/".
    const pre = alloc(8);
    check("prestat_get of the preopen", w.fd_prestat_get(3, pre) === 0 && view().getUint32(pre + 4, true) === 1);
    check("prestat_get of a stream is BADF", w.fd_prestat_get(1, pre) === 8);
    // Create a file, write, seek, read back.
    const [p, n] = put("dir/a.txt");
    const fdp = alloc(4);
    check("open of a missing file is NOENT", w.path_open(3, 0, p, n, 0, 0n, 0n, 0, fdp) === 44);
    const [dp, dn] = put("dir");
    check("create_directory", w.path_create_directory(3, dp, dn) === 0);
    check("create_directory again is EXIST", w.path_create_directory(3, dp, dn) === 20);
    check("open with CREAT", w.path_open(3, 0, p, n, 1, 0n, 0n, 0, fdp) === 0);
    const fd = view().getUint32(fdp, true);
    const [tp, tn] = put("hello, file");
    const nw = alloc(4);
    check("write", w.fd_write(fd, iov(tp, tn), 1, nw) === 0 && view().getUint32(nw, true) === tn);
    const [tp2, tn2] = put("!");
    w.fd_write(fd, iov(tp2, tn2), 1, nw);
    const pos = alloc(8);
    check("seek to the start", w.fd_seek(fd, 0n, 0, pos) === 0 && view().getBigUint64(pos, true) === 0n);
    const rb = alloc(64), nr = alloc(4);
    check("read back", w.fd_read(fd, iov(rb, 64), 1, nr) === 0 && view().getUint32(nr, true) === 12 &&
          dec.decode(new Uint8Array(h.memory.buffer, rb, 12)) === "hello, file!");
    check("read at the end gives 0", w.fd_read(fd, iov(rb, 64), 1, nr) === 0 && view().getUint32(nr, true) === 0);
    check("seek from the end", w.fd_seek(fd, -1n, 2, pos) === 0 && view().getBigUint64(pos, true) === 11n);
    const st = alloc(24);
    check("fdstat of a file", w.fd_fdstat_get(fd, st) === 0 && view().getUint8(st) === 4);
    check("close", w.fd_close(fd) === 0 && w.fd_close(fd) === 8);
    // filestat by path.
    const fs = alloc(64);
    check("filestat of the file", w.path_filestat_get(3, 0, p, n, fs) === 0 && view().getUint8(fs + 16) === 4 &&
          view().getBigUint64(fs + 32, true) === 12n);
    check("filestat of the directory", w.path_filestat_get(3, 0, dp, dn, fs) === 0 && view().getUint8(fs + 16) === 3);
    const [mp, mn] = put("dir/missing");
    check("filestat of nothing is NOENT", w.path_filestat_get(3, 0, mp, mn, fs) === 44);
    // EXCL, TRUNC, APPEND.
    check("open with CREAT|EXCL of an existing file is EXIST", w.path_open(3, 0, p, n, 5, 0n, 0n, 0, fdp) === 20);
    check("open with TRUNC", w.path_open(3, 0, p, n, 8, 0n, 0n, 0, fdp) === 0 &&
          w.path_filestat_get(3, 0, p, n, fs) === 0 && view().getBigUint64(fs + 32, true) === 0n);
    w.fd_close(view().getUint32(fdp, true));
    check("open with APPEND", w.path_open(3, 0, p, n, 0, 0n, 0n, 1, fdp) === 0);
    const afd = view().getUint32(fdp, true);
    w.fd_write(afd, iov(tp, tn), 1, nw);
    w.fd_seek(afd, 0n, 0, pos);
    w.fd_write(afd, iov(tp2, tn2), 1, nw);
    check("append writes at the end whatever the position", h.shim.tree.readText("/dir/a.txt") === "hello, file!");
    w.fd_close(afd);
    // readdir: two entries sorted, then a small buffer that truncates.
    const [bp, bn] = put("dir/b.txt");
    w.path_open(3, 0, bp, bn, 1, 0n, 0n, 0, fdp);
    w.fd_close(view().getUint32(fdp, true));
    check("open the directory", w.path_open(3, 0, dp, dn, 2, 0n, 0n, 0, fdp) === 0);
    const dfd = view().getUint32(fdp, true);
    const db = alloc(256), used = alloc(4);
    check("readdir", w.fd_readdir(dfd, db, 256, 0n, used) === 0 && view().getUint32(used, true) === 2 * (24 + 5) &&
          dec.decode(new Uint8Array(h.memory.buffer, db + 24, 5)) === "a.txt" &&
          view().getBigUint64(db, true) === 1n &&
          dec.decode(new Uint8Array(h.memory.buffer, db + 29 + 24, 5)) === "b.txt");
    check("readdir from a cookie", w.fd_readdir(dfd, db, 256, 1n, used) === 0 && view().getUint32(used, true) === 29);
    check("readdir into a small buffer truncates", w.fd_readdir(dfd, db, 30, 0n, used) === 0 && view().getUint32(used, true) === 30);
    check("read of a directory is ISDIR", w.fd_read(dfd, iov(rb, 8), 1, nr) === 31);
    w.fd_close(dfd);
    // rename, unlink, remove_directory.
    const [cp, cn] = put("dir/c.txt");
    check("rename", w.path_rename(3, bp, bn, 3, cp, cn) === 0 && h.shim.tree.exists("/dir/c.txt") && !h.shim.tree.exists("/dir/b.txt"));
    check("rename of nothing is NOENT", w.path_rename(3, bp, bn, 3, cp, cn) === 44);
    check("remove_directory of a full one is NOTEMPTY", w.path_remove_directory(3, dp, dn) === 55);
    check("unlink", w.path_unlink_file(3, p, n) === 0 && w.path_unlink_file(3, cp, cn) === 0);
    check("unlink of a directory is ISDIR", w.path_unlink_file(3, dp, dn) === 31);
    check("remove_directory", w.path_remove_directory(3, dp, dn) === 0 && !h.shim.tree.exists("/dir"));
    // Streams.
    const [sp, sn] = put("to stdout");
    w.fd_write(1, iov(sp, sn), 1, nw);
    const [ep, en] = put("to stderr");
    w.fd_write(2, iov(ep, en), 1, nw);
    check("stdout and stderr reach their callbacks", h.out.join("") === "to stdout" && h.err.join("") === "to stderr");
    check("stdin reads the buffer", w.fd_read(0, iov(rb, 9), 1, nr) === 0 && dec.decode(new Uint8Array(h.memory.buffer, rb, 9)) === "line one\n" &&
          w.fd_read(0, iov(rb, 64), 1, nr) === 0 && view().getUint32(nr, true) === 9 &&
          w.fd_read(0, iov(rb, 64), 1, nr) === 0 && view().getUint32(nr, true) === 0);
    // environment and clock.
    const cnt = alloc(4), sz = alloc(4);
    check("environ sizes", w.environ_sizes_get(cnt, sz) === 0 && view().getUint32(cnt, true) === 1 &&
          view().getUint32(sz, true) === "GOOSE_PROGRESS=plain\0".length);
    const t = alloc(8);
    check("clock_time_get", w.clock_time_get(0, 0n, t) === 0 && view().getBigUint64(t, true) > 1_600_000_000_000_000_000n);
    // The tree's own API, which the page fills it through.
    const tree = new Tree();
    tree.writeFile("/x/y/z.txt", "deep");
    check("writeFile makes the directories", tree.readText("x/y/z.txt") === "deep" && tree.list("/x").join() === "y");
}

// End to end, where a module is at hand.
const wasm = argv[2] || process.env.GOOSE_WASM;
if (wasm) {
    const here = dirname(fileURLToPath(import.meta.url));
    const root = join(here, "..", "..", "..");
    const tree = new Tree();
    // Every file of the standard library, as the page serves it.
    const { readdirSync } = await import("node:fs");
    for (const f of readdirSync(join(root, "stdlib"))) if (f.endsWith(".goose"))
        tree.writeFile(`/stdlib/${f}`, readFileSync(join(root, "stdlib", f)));
    tree.writeFile("/prog.goose", readFileSync(join(root, "test", "codegen", "hello_world.goose")));
    let out = "", err = "";
    const shim = new WasiShim({ tree, stdout: b => { out += dec.decode(b); }, stderr: b => { err += dec.decode(b); },
                                env: { GOOSE_PROGRESS: "plain" } });
    const { instance } = await WebAssembly.instantiate(readFileSync(wasm), shim.imports);
    shim.start(instance);
    const code = runCommand(instance, "run /prog.goose --stdlib /stdlib");
    check("hello world through the shim", code === 0 && out === "Hello, world!\n", `exit ${code}, out ${JSON.stringify(out)}, err ${err.slice(0, 200)}`);
    out = ""; err = "";
    const code2 = runCommand(instance, "-O2 -o /out.c /prog.goose --stdlib /stdlib");
    const c = tree.readText("/out.c") || "";
    check("the generated C lands in the tree", code2 === 0 && c.includes("Hello, world!") && c.includes("gs_rt_start"),
          `exit ${code2}, ${c.length} bytes, err ${err.slice(0, 200)}`);
    out = ""; err = "";
    tree.writeFile("/bad.goose", "fn main() { let x: i64 = \"no\"; }\n");
    const code3 = runCommand(instance, "--check /bad.goose --stdlib /stdlib");
    check("a type error reports and exits 1", code3 === 1 && /error/.test(err), `exit ${code3}, err ${err.slice(0, 200)}`);

    // The worker, as the page drives it: one run per worker, the compiled
    // module posted with the files, the output and the exit status posted
    // back, and the file asked for read back at the end.
    const { Worker } = await import("node:worker_threads");
    const module = await WebAssembly.compile(readFileSync(wasm));
    const files = {};
    for (const f of readdirSync(join(root, "stdlib"))) if (f.endsWith(".goose"))
        files[`/stdlib/${f}`] = readFileSync(join(root, "stdlib", f), "utf8");
    files["/prog.goose"] = readFileSync(join(root, "test", "codegen", "hello_world.goose"), "utf8");
    const viaWorker = (cmdline, readback = []) => new Promise((resolve, reject) => {
        const w = new Worker(new URL("./worker.mjs", import.meta.url));
        const got = { out: "", err: "", status: [], code: undefined, files: {} };
        w.on("message", m => {
            if (m.type === "status") got.status.push(m.text);
            else if (m.type === "output") { got.out += m.out; got.err += m.err; }
            else if (m.type === "done") { got.code = m.code; got.files = m.files; w.terminate().then(() => resolve(got)); }
        });
        w.on("error", reject);
        w.postMessage({ module, files, cmdline, readback });
    });
    const r1 = await viaWorker("run /prog.goose --stdlib /stdlib");
    check("hello world through the worker", r1.code === 0 && r1.out === "Hello, world!\n",
          `exit ${r1.code}, out ${JSON.stringify(r1.out)}, err ${r1.err.slice(0, 200)}`);
    const r2 = await viaWorker("-O2 -o /out.c /prog.goose --stdlib /stdlib", ["/out.c", "/missing.c"]);
    check("the worker reads the generated C back", r2.code === 0 && (r2.files["/out.c"] || "").includes("gs_rt_start") &&
          !("/missing.c" in r2.files), `exit ${r2.code}, files ${Object.keys(r2.files)}, err ${r2.err.slice(0, 200)}`);
    files["/loop.goose"] = "fn main() { var n: i64 = 0; for i in 3000000 { n += i; } print(n); }\n";
    const r3 = await viaWorker("run /loop.goose -O0 --stdlib /stdlib");
    check("a run that suspends reports running and finishes", r3.code === 0 && r3.status.includes("running…") &&
          r3.out === "4499998500000\n", `exit ${r3.code}, status ${r3.status}, out ${JSON.stringify(r3.out)}, err ${r3.err.slice(0, 200)}`);
    files["/bad.goose"] = "fn main() { let x: i64 = \"no\"; }\n";
    const r4 = await viaWorker("--check /bad.goose --stdlib /stdlib");
    check("a type error through the worker", r4.code === 1 && /error/.test(r4.err), `exit ${r4.code}, err ${r4.err.slice(0, 200)}`);
    // The compiled path (docs/design/wasm_backend.md §6): the program's
    // module runs on goose.wasm's exports, its stop read back through
    // goose_wasm_finish.
    const r5 = await viaWorker("run /prog.goose --wasm --stdlib /stdlib");
    check("hello world compiled to WebAssembly", r5.code === 0 && r5.out === "Hello, world!\n" && r5.status.includes("running…"),
          `exit ${r5.code}, status ${r5.status}, out ${JSON.stringify(r5.out)}, err ${r5.err.slice(0, 200)}`);
    const r6 = await viaWorker("run /loop.goose -O2 --wasm --stdlib /stdlib");
    check("a loop compiled to WebAssembly", r6.code === 0 && r6.out === "4499998500000\n",
          `exit ${r6.code}, out ${JSON.stringify(r6.out)}, err ${r6.err.slice(0, 200)}`);
    files["/abort.goose"] = "fn main() { print(\"before\"); abort(\"custom failure\"); }\n";
    const r7 = await viaWorker("run /abort.goose --wasm --stdlib /stdlib");
    check("an abort compiled to WebAssembly keeps the output before it", r7.code !== 0 && r7.out === "before\n" && /custom failure/.test(r7.err),
          `exit ${r7.code}, out ${JSON.stringify(r7.out)}, err ${r7.err.slice(0, 200)}`);
    files["/deep.goose"] = "recursive fn down(n: i64) -> i64 { if n == 0 { 0 } else { 1 + down(n - 1) } }\nfn main() { print(down(100000000)); }\n";
    const r8 = await viaWorker("run /deep.goose --wasm --stdlib /stdlib");
    check("a stack overflow compiled to WebAssembly is reported", r8.code !== 0 && /stack/.test(r8.err),
          `exit ${r8.code}, out ${JSON.stringify(r8.out)}, err ${r8.err.slice(0, 200)}`);
} else {
    console.log("skip the end-to-end checks: no goose.wasm given");
}

if (failures) console.log(`${failures} failure(s)`);
exit(failures ? 1 : 0);
