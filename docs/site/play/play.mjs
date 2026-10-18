// The playground (docs/design/interpreter.md §9): goose.wasm run on the WASI
// shim over an in-memory tree that holds the standard library and the
// samples (files.json, written by the site build) and the editor's program
// as /prog.goose. Run compiles and interprets the program in pieces from
// requestAnimationFrame, so the page stays responsive and Stop is seen at
// the next piece; Check typechecks; Generated C writes -o /out.c and shows
// it. The module is instantiated once per run, since a run leaves its
// state behind and the compiler is not written to run twice in a process.
import { WasiShim, Tree, SUSPENDED } from "./wasi.mjs";

const $ = id => document.getElementById(id);
const dec = new TextDecoder();
const state = { wasmBytes: null, files: null, running: false, stop: false };

function status(text) { $("status").textContent = text; }

function show(tab) {
    for (const b of document.querySelectorAll(".right .tab")) b.classList.toggle("active", b.dataset.tab === tab);
    for (const v of document.querySelectorAll(".right .view")) v.classList.toggle("active", v.id === tab);
}

function clearViews() {
    for (const id of ["output", "messages"]) $(id).textContent = "";
}

function append(id, text, cls) {
    const view = $(id);
    if (cls) {
        const span = document.createElement("span");
        span.className = cls;
        span.textContent = text;
        view.appendChild(span);
    } else {
        view.appendChild(document.createTextNode(text));
    }
    view.scrollTop = view.scrollHeight;
}

// The tree a run sees: every file of files.json and the editor's program.
function makeTree() {
    const tree = new Tree();
    for (const [path, text] of Object.entries(state.files)) tree.writeFile(path, text);
    tree.writeFile("/prog.goose", $("source").value);
    return tree;
}

function setBusy(busy) {
    state.running = busy;
    for (const id of ["run", "check", "emit"]) $(id).disabled = busy || !state.wasmBytes;
    $("stop").disabled = !busy;
}

// Runs a command line: compiles and, for `run`, interprets the program in
// pieces of work between animation frames. `done(code, tree)` is called at
// the end, with null for a stopped run.
async function runCommand(cmdline, done) {
    if (state.running) return;
    setBusy(true);
    state.stop = false;
    clearViews();
    const tree = makeTree();
    const shim = new WasiShim({
        tree,
        stdout: b => append("output", dec.decode(b)),
        stderr: b => append("messages", dec.decode(b), "err"),
        env: { GOOSE_PROGRESS: "plain" },
    });
    let instance;
    try {
        ({ instance } = await WebAssembly.instantiate(state.wasmBytes, shim.imports));
        shim.start(instance);
    } catch (e) {
        append("messages", `cannot start goose.wasm: ${e.message}\n`, "err");
        setBusy(false);
        return;
    }
    const { memory, goose_argbuf, goose_run, goose_resume } = instance.exports;
    const line = new TextEncoder().encode(cmdline);
    const buf = goose_argbuf(line.length);
    new Uint8Array(memory.buffer, buf, line.length + 1).set(line);
    const started = performance.now();
    status("compiling…");
    let code;
    try {
        code = goose_run(buf);
    } catch (e) {
        code = shim.exitCode ?? 1;
        if (shim.exitCode === null) append("messages", `goose.wasm failed: ${e.message}\n`, "err");
    }
    const finish = result => {
        const secs = ((performance.now() - started) / 1000).toFixed(1);
        status(result === null ? `stopped after ${secs}s` : `exit ${result} in ${secs}s`);
        setBusy(false);
        done(result, tree);
    };
    if (code !== SUSPENDED) return finish(code);
    // Resume between frames: each piece is a step budget of the VM's.
    status("running…");
    const step = () => {
        if (state.stop) return finish(null);
        let c;
        try {
            c = goose_resume();
        } catch (e) {
            c = shim.exitCode ?? 1;
            if (shim.exitCode === null) append("messages", `goose.wasm failed: ${e.message}\n`, "err");
        }
        if (c === SUSPENDED) requestAnimationFrame(step);
        else finish(c);
    };
    requestAnimationFrame(step);
}

function optLevel() { return $("opt").value; }

function programArgs() {
    const a = $("args").value.trim();
    return a ? " -- " + a : "";
}

$("run").onclick = () => {
    show("output");
    runCommand(`run /prog.goose ${optLevel()} --stdlib /stdlib${programArgs()}`, () => {});
};
$("check").onclick = () => {
    show("messages");
    runCommand(`--check /prog.goose --stdlib /stdlib`, code => {
        if (code === 0) append("messages", "no errors\n");
    });
};
$("emit").onclick = () => {
    show("messages");
    runCommand(`${optLevel()} -o /out.c /prog.goose --stdlib /stdlib`, (code, tree) => {
        if (code === 0) {
            $("cfile").textContent = tree.readText("/out.c") || "";
            show("cfile");
        }
    });
};
$("stop").onclick = () => { state.stop = true; };
for (const b of document.querySelectorAll(".right .tab")) b.onclick = () => show(b.dataset.tab);

$("sample").onchange = () => {
    const path = $("sample").value;
    if (!path) return;
    $("source").value = state.files[path];
    const name = path.replace(/^\/samples\/\d+_/, "").replace(/\.goose$/, "");
    const args = state.files[`/samples/data/${name}.args`];
    $("args").value = args ? args.trim() : "";
};

// The editor's program outlives a reload.
$("source").addEventListener("input", () => {
    try { localStorage.setItem("goose-play-source", $("source").value); } catch (e) { /* no storage */ }
});

async function load() {
    try {
        const [files, wasm] = await Promise.all([
            fetch("files.json").then(r => { if (!r.ok) throw new Error(`files.json: ${r.status}`); return r.json(); }),
            fetch("goose.wasm").then(r => { if (!r.ok) throw new Error(`goose.wasm: ${r.status}`); return r.arrayBuffer(); }),
        ]);
        state.files = files;
        state.wasmBytes = wasm;
    } catch (e) {
        status(`cannot load the playground: ${e.message}`);
        return;
    }
    const picker = $("sample");
    for (const path of Object.keys(state.files).sort()) {
        if (!path.startsWith("/samples/") || !path.endsWith(".goose")) continue;
        const o = document.createElement("option");
        o.value = path;
        o.textContent = path.slice("/samples/".length);
        picker.appendChild(o);
    }
    let saved = null;
    try { saved = localStorage.getItem("goose-play-source"); } catch (e) { /* no storage */ }
    if (saved) $("source").value = saved;
    else {
        picker.value = "/samples/01_tour.goose";
        picker.onchange();
    }
    status(`ready (goose.wasm, ${(state.wasmBytes.byteLength / 1048576).toFixed(1)} MB)`);
    setBusy(false);
}

load();
