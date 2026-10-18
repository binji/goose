// The playground (docs/design/interpreter.md §9): goose.wasm run on the WASI
// shim over an in-memory tree that holds the standard library and the
// samples (files.json, written by the site build) and the editor's program
// as /prog.goose. Every run is a Worker of its own (worker.mjs), given the
// module the page compiled once: the worker runs the compiler and the
// program back to back, streaming the output, so the page stays responsive
// and Stop ends the run at once by terminating the worker. Check
// typechecks; Generated C writes -o /out.c and shows it.
const $ = id => document.getElementById(id);
const state = { module: null, files: null, worker: null };

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

function setBusy(busy) {
    for (const id of ["run", "check", "emit"]) $(id).disabled = busy || !state.module;
    $("stop").disabled = !busy;
}

// Runs a command line in a fresh worker over the tree of files.json and
// the editor's program. `done(code, files)` is called at the end with the
// exit status, null for a stopped run, and the files of `readback` the run
// left in the tree.
function runCommand(cmdline, readback, done) {
    if (state.worker) return;
    setBusy(true);
    clearViews();
    const started = performance.now();
    let worker;
    try {
        worker = new Worker("worker.mjs", { type: "module" });
    } catch (e) {
        append("messages", `cannot start the worker: ${e.message}\n`, "err");
        setBusy(false);
        return;
    }
    state.worker = worker;
    const finish = (code, files = {}) => {
        worker.terminate();
        state.worker = null;
        const secs = ((performance.now() - started) / 1000).toFixed(1);
        status(code === null ? `stopped after ${secs}s` : `exit ${code} in ${secs}s`);
        setBusy(false);
        done(code, files);
    };
    state.stop = () => finish(null);
    worker.onmessage = ({ data: m }) => {
        if (m.type === "status") status(m.text);
        else if (m.type === "output") {
            if (m.out) append("output", m.out);
            if (m.err) append("messages", m.err, "err");
        } else if (m.type === "done") finish(m.code, m.files);
    };
    worker.onerror = e => {
        append("messages", `the worker failed: ${e.message || "cannot load worker.mjs"}\n`, "err");
        finish(1);
    };
    const files = { ...state.files, "/prog.goose": $("source").value };
    worker.postMessage({ module: state.module, files, cmdline, readback });
    status("compiling…");
}

function optLevel() { return $("opt").value; }

function programArgs() {
    const a = $("args").value.trim();
    return a ? " -- " + a : "";
}

$("run").onclick = () => {
    show("output");
    runCommand(`run /prog.goose ${optLevel()} --stdlib /stdlib${programArgs()}`, [], () => {});
};
$("check").onclick = () => {
    show("messages");
    runCommand(`--check /prog.goose --stdlib /stdlib`, [], code => {
        if (code === 0) append("messages", "no errors\n");
    });
};
$("emit").onclick = () => {
    show("messages");
    runCommand(`${optLevel()} -o /out.c /prog.goose --stdlib /stdlib`, ["/out.c"], (code, files) => {
        if (code === 0) {
            $("cfile").textContent = files["/out.c"] || "";
            show("cfile");
        }
    });
};
$("stop").onclick = () => { if (state.worker) state.stop(); };
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
        state.size = wasm.byteLength;
        // Compiled once; each run's worker is handed the module.
        state.module = await WebAssembly.compile(wasm);
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
    status(`ready (goose.wasm, ${(state.size / 1048576).toFixed(1)} MB)`);
    setBusy(false);
}

load();
