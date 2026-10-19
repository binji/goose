// The WASI shim the playground runs goose.wasm on (docs/design/interpreter.md
// §9): the wasi_snapshot_preview1 imports the module uses, over an in-memory
// directory tree the page fills before the first run, with stdout and
// stderr going to callbacks and stdin read from a buffer. A plain module
// with no dependencies, so node runs it too (wasi_test.mjs), which is how
// the file calls are tested.
//
//   const shim = new WasiShim({ stdout: bytes => ..., stderr: bytes => ... });
//   shim.tree.writeFile("/prog.goose", text);
//   const { instance } = await WebAssembly.instantiate(bytes, shim.imports);
//   shim.start(instance);   // a reactor: calls _initialize
//
// Sleeps (poll_oneoff on a clock) return at once: a page cannot block, and
// a sleeping program is not what the playground is for.

const E = {
    SUCCESS: 0, TOOBIG: 1, ACCES: 2, BADF: 8, EXIST: 20, INVAL: 28, ISDIR: 31, NOENT: 44,
    NOSYS: 52, NOTDIR: 54, NOTEMPTY: 55, NOTSUP: 58, SPIPE: 70,
};
const FILETYPE = { CHAR: 2, DIR: 3, FILE: 4 };
const OFLAGS = { CREAT: 1, DIRECTORY: 2, EXCL: 4, TRUNC: 8 };
const FDFLAGS = { APPEND: 1 };
const PREOPEN_FD = 3;

const encoder = new TextEncoder();
const decoder = new TextDecoder();

export class ProcExit extends Error {
    constructor(code) {
        super(`proc_exit(${code})`);
        this.code = code;
    }
}

// The tree: a directory is a Map of name to node; a file is { file: Uint8Array,
// mtime }. Paths are "/a/b" or "a/b" from the root alike.
class Dir {
    constructor() { this.entries = new Map(); this.mtime = Date.now(); }
}
class File {
    constructor(bytes) { this.file = bytes; this.mtime = Date.now(); }
}

export class Tree {
    constructor() { this.root = new Dir(); }

    static parts(path) {
        const out = [];
        for (const p of path.split("/")) {
            if (p === "" || p === ".") continue;
            if (p === "..") out.pop();
            else out.push(p);
        }
        return out;
    }

    // The node at a path, or undefined.
    lookup(path, from = this.root) {
        let node = from;
        for (const p of Tree.parts(path)) {
            if (!(node instanceof Dir)) return undefined;
            node = node.entries.get(p);
            if (!node) return undefined;
        }
        return node;
    }

    // The directory holding the last component of a path, and that name.
    parent(path, from = this.root) {
        const parts = Tree.parts(path);
        if (parts.length === 0) return { dir: undefined, name: "" };
        const name = parts.pop();
        let node = from;
        for (const p of parts) {
            if (!(node instanceof Dir)) return { dir: undefined, name };
            node = node.entries.get(p);
            if (!node) return { dir: undefined, name };
        }
        return { dir: node instanceof Dir ? node : undefined, name };
    }

    mkdirp(path) {
        let node = this.root;
        for (const p of Tree.parts(path)) {
            let next = node.entries.get(p);
            if (!next) {
                next = new Dir();
                node.entries.set(p, next);
            }
            if (!(next instanceof Dir)) throw new Error(`${path}: not a directory`);
            node = next;
        }
        return node;
    }

    writeFile(path, data) {
        const bytes = typeof data === "string" ? encoder.encode(data) : data;
        const { dir, name } = this.parent(path);
        const d = dir || this.mkdirp(path.slice(0, path.lastIndexOf("/")));
        d.entries.set(name, new File(bytes));
    }

    readFile(path) {
        const node = this.lookup(path);
        return node instanceof File ? node.file : undefined;
    }

    readText(path) {
        const bytes = this.readFile(path);
        return bytes === undefined ? undefined : decoder.decode(bytes);
    }

    exists(path) { return this.lookup(path) !== undefined; }

    list(path) {
        const node = this.lookup(path);
        return node instanceof Dir ? [...node.entries.keys()].sort() : undefined;
    }
}

// An open descriptor: a file with a position, a directory, or a stream.
class Fd {
    constructor(kind, node, path) {
        this.kind = kind;   // "file", "dir", "stdin", "stdout", "stderr"
        this.node = node;
        this.path = path;
        this.pos = 0;
        this.append = false;
    }
}

export class WasiShim {
    constructor({ tree = new Tree(), stdin = new Uint8Array(0), stdout = () => {}, stderr = () => {},
                  env = {} } = {}) {
        this.tree = tree;
        this.stdin = typeof stdin === "string" ? encoder.encode(stdin) : stdin;
        this.stdinPos = 0;
        this.stdout = stdout;
        this.stderr = stderr;
        this.env = env;
        this.memory = null;
        this.fds = new Map();
        this.fds.set(0, new Fd("stdin"));
        this.fds.set(1, new Fd("stdout"));
        this.fds.set(2, new Fd("stderr"));
        this.fds.set(PREOPEN_FD, new Fd("dir", tree.root, "/"));
        this.nextFd = PREOPEN_FD + 1;
        this.exitCode = null;
        const self = this;
        const fns = {};
        for (const name of Object.getOwnPropertyNames(WasiShim.prototype)) {
            if (name.startsWith("wasi_")) fns[name.slice(5)] = (...a) => self[name](...a);
        }
        this.imports = { wasi_snapshot_preview1: fns };
    }

    // Binds the instance's memory and runs a reactor's initializer.
    start(instance) {
        this.memory = instance.exports.memory;
        if (instance.exports._initialize) instance.exports._initialize();
    }

    // ---- memory helpers

    view() { return new DataView(this.memory.buffer); }
    bytes(ptr, len) { return new Uint8Array(this.memory.buffer, ptr, len); }
    str(ptr, len) { return decoder.decode(this.bytes(ptr, len)); }
    u32(ptr) { return this.view().getUint32(ptr, true); }
    setU32(ptr, v) { this.view().setUint32(ptr, v, true); }
    setU64(ptr, v) { this.view().setBigUint64(ptr, BigInt(v), true); }
    setU8(ptr, v) { this.view().setUint8(ptr, v); }
    setU16(ptr, v) { this.view().setUint16(ptr, v, true); }

    // Reads the bytes of an iovec array into one buffer each.
    iovecs(ptr, len) {
        const out = [];
        for (let i = 0; i < len; i++) {
            const buf = this.u32(ptr + i * 8), n = this.u32(ptr + i * 8 + 4);
            out.push(this.bytes(buf, n));
        }
        return out;
    }

    fd(fd) { return this.fds.get(fd); }

    // The node a path names from a directory fd, as { node, dir, name }.
    resolve(fd, pathPtr, pathLen) {
        const d = this.fd(fd);
        if (!d) return { err: E.BADF };
        if (d.kind !== "dir") return { err: E.NOTDIR };
        const path = this.str(pathPtr, pathLen);
        const { dir, name } = this.tree.parent(path, d.node);
        const node = Tree.parts(path).length === 0 ? d.node : dir ? dir.entries.get(name) : undefined;
        return { path, dir, name, node };
    }

    // ---- environment and clocks

    envBytes() {
        return Object.entries(this.env).map(([k, v]) => encoder.encode(`${k}=${v}\0`));
    }
    wasi_environ_sizes_get(countPtr, sizePtr) {
        const e = this.envBytes();
        this.setU32(countPtr, e.length);
        this.setU32(sizePtr, e.reduce((n, b) => n + b.length, 0));
        return E.SUCCESS;
    }
    wasi_environ_get(environPtr, bufPtr) {
        let p = bufPtr;
        this.envBytes().forEach((b, i) => {
            this.setU32(environPtr + i * 4, p);
            this.bytes(p, b.length).set(b);
            p += b.length;
        });
        return E.SUCCESS;
    }
    wasi_clock_time_get(id, _precision, timePtr) {
        const ms = id === 1 && typeof performance !== "undefined" ? performance.now() : Date.now();
        this.setU64(timePtr, BigInt(Math.round(ms * 1e6)));
        return E.SUCCESS;
    }
    wasi_poll_oneoff(inPtr, outPtr, n, neventsPtr) {
        // Every subscription is answered at once: a clock as expired, a
        // read or write as ready. Subscriptions are 48 bytes, events 32.
        for (let i = 0; i < n; i++) {
            const sub = inPtr + i * 48, ev = outPtr + i * 32;
            const userdata = this.view().getBigUint64(sub, true);
            const type = this.view().getUint8(sub + 8);
            this.view().setBigUint64(ev, userdata, true);
            this.setU16(ev + 8, E.SUCCESS);
            this.setU8(ev + 10, type);
            this.setU64(ev + 16, 0);
            this.setU16(ev + 24, 0);
        }
        this.setU32(neventsPtr, n);
        return E.SUCCESS;
    }
    wasi_proc_exit(code) {
        this.exitCode = code;
        throw new ProcExit(code);
    }

    // ---- descriptors

    wasi_fd_close(fd) {
        if (!this.fds.has(fd)) return E.BADF;
        if (fd > 2) this.fds.delete(fd);
        return E.SUCCESS;
    }
    wasi_fd_fdstat_get(fd, buf) {
        const d = this.fd(fd);
        if (!d) return E.BADF;
        const type = d.kind === "dir" ? FILETYPE.DIR : d.kind === "file" ? FILETYPE.FILE : FILETYPE.CHAR;
        this.setU8(buf, type);
        this.setU16(buf + 2, d.append ? FDFLAGS.APPEND : 0);
        this.setU64(buf + 8, 0xffffffffffffffffn);
        this.setU64(buf + 16, 0xffffffffffffffffn);
        return E.SUCCESS;
    }
    wasi_fd_fdstat_set_flags(fd, flags) {
        const d = this.fd(fd);
        if (!d) return E.BADF;
        d.append = (flags & FDFLAGS.APPEND) !== 0;
        return E.SUCCESS;
    }
    wasi_fd_prestat_get(fd, buf) {
        if (fd !== PREOPEN_FD) return E.BADF;
        this.setU8(buf, 0);
        this.setU32(buf + 4, 1);   // the name "/"
        return E.SUCCESS;
    }
    wasi_fd_prestat_dir_name(fd, pathPtr, pathLen) {
        if (fd !== PREOPEN_FD) return E.BADF;
        if (pathLen >= 1) this.setU8(pathPtr, "/".charCodeAt(0));
        return E.SUCCESS;
    }
    wasi_fd_sync() { return E.SUCCESS; }

    wasi_fd_read(fd, iovs, iovsLen, nreadPtr) {
        const d = this.fd(fd);
        if (!d) return E.BADF;
        let total = 0;
        for (const buf of this.iovecs(iovs, iovsLen)) {
            let n = 0;
            if (d.kind === "stdin") {
                n = Math.min(buf.length, this.stdin.length - this.stdinPos);
                buf.set(this.stdin.subarray(this.stdinPos, this.stdinPos + n));
                this.stdinPos += n;
            } else if (d.kind === "file") {
                const data = d.node.file;
                n = Math.max(0, Math.min(buf.length, data.length - d.pos));
                buf.set(data.subarray(d.pos, d.pos + n));
                d.pos += n;
            } else {
                return d.kind === "dir" ? E.ISDIR : E.BADF;
            }
            total += n;
            if (n < buf.length) break;
        }
        this.setU32(nreadPtr, total);
        return E.SUCCESS;
    }

    wasi_fd_write(fd, iovs, iovsLen, nwrittenPtr) {
        const d = this.fd(fd);
        if (!d) return E.BADF;
        let total = 0;
        for (const buf of this.iovecs(iovs, iovsLen)) {
            if (d.kind === "stdout") this.stdout(buf.slice());
            else if (d.kind === "stderr") this.stderr(buf.slice());
            else if (d.kind === "file") {
                const data = d.node.file;
                const at = d.append ? data.length : d.pos;
                if (at + buf.length > data.length) {
                    const grown = new Uint8Array(at + buf.length);
                    grown.set(data);
                    d.node.file = grown;
                }
                d.node.file.set(buf, at);
                d.node.mtime = Date.now();
                d.pos = at + buf.length;
            } else {
                return d.kind === "dir" ? E.ISDIR : E.BADF;
            }
            total += buf.length;
        }
        this.setU32(nwrittenPtr, total);
        return E.SUCCESS;
    }

    wasi_fd_seek(fd, offset, whence, newPtr) {
        const d = this.fd(fd);
        if (!d) return E.BADF;
        if (d.kind !== "file") return E.SPIPE;
        const off = Number(offset);
        const base = whence === 0 ? 0 : whence === 1 ? d.pos : d.node.file.length;
        if (base + off < 0) return E.INVAL;
        d.pos = base + off;
        this.setU64(newPtr, d.pos);
        return E.SUCCESS;
    }

    wasi_fd_readdir(fd, buf, bufLen, cookie, usedPtr) {
        const d = this.fd(fd);
        if (!d) return E.BADF;
        if (d.kind !== "dir") return E.NOTDIR;
        const names = [...d.node.entries.keys()].sort();
        let p = buf, used = 0;
        for (let i = Number(cookie); i < names.length; i++) {
            const name = encoder.encode(names[i]);
            const node = d.node.entries.get(names[i]);
            const entry = new Uint8Array(24 + name.length);
            const v = new DataView(entry.buffer);
            v.setBigUint64(0, BigInt(i + 1), true);                        // d_next
            v.setBigUint64(8, BigInt(i + 1), true);                        // d_ino
            v.setUint32(16, name.length, true);                            // d_namlen
            v.setUint8(20, node instanceof Dir ? FILETYPE.DIR : FILETYPE.FILE);
            entry.set(name, 24);
            // What fits: a truncated last entry tells the caller to come
            // back with a larger buffer.
            const n = Math.min(entry.length, bufLen - used);
            this.bytes(p, n).set(entry.subarray(0, n));
            p += n;
            used += n;
            if (n < entry.length) break;
        }
        this.setU32(usedPtr, used);
        return E.SUCCESS;
    }

    // ---- paths

    wasi_path_create_directory(fd, pathPtr, pathLen) {
        const r = this.resolve(fd, pathPtr, pathLen);
        if (r.err) return r.err;
        if (!r.dir) return E.NOENT;
        if (r.dir.entries.has(r.name)) return E.EXIST;
        r.dir.entries.set(r.name, new Dir());
        return E.SUCCESS;
    }

    wasi_path_filestat_get(fd, _flags, pathPtr, pathLen, buf) {
        const r = this.resolve(fd, pathPtr, pathLen);
        if (r.err) return r.err;
        if (!r.node) return E.NOENT;
        this.bytes(buf, 64).fill(0);
        this.setU64(buf, 1);                                                // dev
        this.setU64(buf + 8, 1);                                            // ino
        this.setU8(buf + 16, r.node instanceof Dir ? FILETYPE.DIR : FILETYPE.FILE);
        this.setU64(buf + 24, 1);                                           // nlink
        this.setU64(buf + 32, r.node instanceof File ? r.node.file.length : 0);
        const t = BigInt(Math.round(r.node.mtime * 1e6));
        this.setU64(buf + 40, t);
        this.setU64(buf + 48, t);
        this.setU64(buf + 56, t);
        return E.SUCCESS;
    }

    wasi_path_open(fd, _dirflags, pathPtr, pathLen, oflags, _rightsBase, _rightsInheriting, fdflags,
                   openedPtr) {
        const r = this.resolve(fd, pathPtr, pathLen);
        if (r.err) return r.err;
        let node = r.node;
        if (node) {
            if ((oflags & OFLAGS.CREAT) && (oflags & OFLAGS.EXCL)) return E.EXIST;
            if ((oflags & OFLAGS.DIRECTORY) && !(node instanceof Dir)) return E.NOTDIR;
            if (node instanceof File && (oflags & OFLAGS.TRUNC)) {
                node.file = new Uint8Array(0);
                node.mtime = Date.now();
            }
        } else {
            if (!(oflags & OFLAGS.CREAT)) return E.NOENT;
            if (!r.dir) return E.NOENT;
            node = new File(new Uint8Array(0));
            r.dir.entries.set(r.name, node);
        }
        const opened = new Fd(node instanceof Dir ? "dir" : "file", node, r.path);
        opened.append = (fdflags & FDFLAGS.APPEND) !== 0;
        const n = this.nextFd++;
        this.fds.set(n, opened);
        this.setU32(openedPtr, n);
        return E.SUCCESS;
    }

    wasi_path_remove_directory(fd, pathPtr, pathLen) {
        const r = this.resolve(fd, pathPtr, pathLen);
        if (r.err) return r.err;
        if (!r.node) return E.NOENT;
        if (!(r.node instanceof Dir)) return E.NOTDIR;
        if (r.node.entries.size) return E.NOTEMPTY;
        r.dir.entries.delete(r.name);
        return E.SUCCESS;
    }

    wasi_path_unlink_file(fd, pathPtr, pathLen) {
        const r = this.resolve(fd, pathPtr, pathLen);
        if (r.err) return r.err;
        if (!r.node) return E.NOENT;
        if (r.node instanceof Dir) return E.ISDIR;
        r.dir.entries.delete(r.name);
        return E.SUCCESS;
    }

    wasi_path_rename(fd, oldPtr, oldLen, newFd, newPtr, newLen) {
        const from = this.resolve(fd, oldPtr, oldLen);
        if (from.err) return from.err;
        if (!from.node) return E.NOENT;
        const to = this.resolve(newFd, newPtr, newLen);
        if (to.err) return to.err;
        if (!to.dir) return E.NOENT;
        const existing = to.node;
        if (existing instanceof Dir && !(from.node instanceof Dir)) return E.ISDIR;
        if (existing instanceof Dir && existing.entries.size) return E.NOTEMPTY;
        from.dir.entries.delete(from.name);
        to.dir.entries.set(to.name, from.node);
        return E.SUCCESS;
    }
}

// Runs a command line through an instantiated goose.wasm: writes it into
// the module and calls goose_run, then goose_resume while the program is
// suspended, `onYield` between the pieces (false from it stops the run).
// Returns the exit status, or null when stopped. A command with --wasm
// compiles the program to a module instead of interpreting it
// (docs/design/wasm_backend.md §6): goose_run returns READY with the
// module's bytes in memory, and runCompiled below runs them.
export const SUSPENDED = -2, READY = -3, REPORT = -4;

// Writes a string into the module's argument buffer and returns its address.
function writeText(exports, s) {
    const bytes = encoder.encode(s);
    const buf = exports.goose_argbuf(bytes.length);
    new Uint8Array(exports.memory.buffer, buf, bytes.length + 1).set(bytes);
    return buf;
}

export function runCommand(instance, cmdline, onYield = () => true, compiled = {}) {
    const { goose_run, goose_resume } = instance.exports;
    let code = goose_run(writeText(instance.exports, cmdline));
    while (code === SUSPENDED) {
        if (!onYield()) return null;
        code = goose_resume();
    }
    if (code === READY) code = runCompiled(instance, compiled);
    return code;
}

// Runs the module goose_run left in memory: instantiates it on goose.wasm's
// exports, which it imports as the module "goose", runs its _start, and
// asks goose_wasm_finish what ended it, with the engine's message where
// the run ended in a trap the host did not record; while the answer is
// REPORT, calls the overflow reporter the host names and asks again.
// `onReady(bytes)` sees the module's bytes before they are compiled, and
// `onStart()` is called before the program runs.
export function runCompiled(instance, { onReady, onStart } = {}) {
    const ex = instance.exports;
    const bytes = new Uint8Array(ex.memory.buffer, ex.goose_module_ptr(), ex.goose_module_len()).slice();
    if (onReady) onReady(bytes);
    const imports = { goose: { memory: ex.memory } };
    for (const name of ["native", "access_slow", "static_handle", "func_handle", "info", "stop", "fault"])
        imports.goose[name] = ex["goose_" + name];
    const prog = new WebAssembly.Instance(new WebAssembly.Module(bytes), imports);
    // Every stop is a trap after the host records the reason, so an
    // `unreachable` says nothing the host does not know; any other trap is
    // the engine's own, reported by its message.
    const run = f => {
        try {
            f();
            return 0;
        } catch (e) {
            const own = e instanceof WebAssembly.RuntimeError && /unreachable/.test(e.message);
            return own ? 0 : writeText(ex, `${e.name}: ${e.message}`);
        }
    };
    if (onStart) onStart();
    let code = ex.goose_wasm_finish(run(() => prog.exports._start()));
    while (code === REPORT) {
        const name = decoder.decode(new Uint8Array(ex.memory.buffer, ex.goose_wasm_report_name(), 64)).split("\0")[0];
        const reporter = prog.exports[name];
        code = ex.goose_wasm_finish(reporter ? run(() => reporter(ex.goose_wasm_report_arg())) : 0);
    }
    return code;
}
