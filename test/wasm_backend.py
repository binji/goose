#!/usr/bin/env python3
"""Runs the interpreter's C fixtures through the WebAssembly backend
(docs/design/wasm_backend.md §11).

    python test/wasm_backend.py [--exe goose.wasm] [--bless] [names...]

Each fixture in test/interp/run/ is compiled to a module by goose.wasm
under node (scripts/wasm_run.mjs, `--run-c file.c --wasm`) and run on it,
as test/interp_run.cpp runs the same fixtures through the VM, with the
same arguments: a fresh empty directory as argv[1], `name.stdin` as its
input where there is one, and INTERP_RUN_VAR set. Both streams and the
exit status are compared with `name.expected`, except that the VM's call
stack under a fault's message is not expected of compiled code, so the
`  in ...` lines are left out of the comparison. Exits 0 when every
fixture matches, 1 on a failure, and 3 where node or goose.wasm is absent.
"""

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
FIXTURES = HERE / "interp" / "run"


def strip_stack(text):
    return "\n".join(line for line in text.split("\n") if not line.startswith("  in ") and not line.startswith("  ... "))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--exe", default=str(ROOT / "build" / "wasm" / "goose.wasm"), help="the compiler built to Wasm")
    ap.add_argument("--bless", action="store_true", help="rewrite the expected files from what the backend printed")
    ap.add_argument("names", nargs="*", help="fixtures to run (default: all)")
    args = ap.parse_args()
    node = shutil.which("node")
    if not node:
        print("skip: node is not installed, which runs the module")
        return 3
    if not Path(args.exe).exists():
        print(f"skip: no {args.exe} (test/wasm_build.py builds it)")
        return 3
    # The error_ fixtures stop in the C front end, before the backend, which
    # test/interp_run.cpp covers.
    names = args.names or sorted(p.stem for p in FIXTURES.glob("*.c") if not p.stem.startswith("error_"))
    tmp = ROOT / "build" / "wasm-backend-test"
    failures = 0
    for name in names:
        src = FIXTURES / f"{name}.c"
        work = tmp / name
        shutil.rmtree(work, ignore_errors=True)
        work.mkdir(parents=True)
        stdin_file = FIXTURES / f"{name}.stdin"
        stdin = stdin_file.read_bytes() if stdin_file.exists() else b""
        # The engine's stack is raised for the fixtures that recurse deep,
        # as the VM's frame segment lets them.
        cmd = [node, "--no-warnings", "--stack-size=4000", str(ROOT / "scripts" / "wasm_run.mjs"), args.exe, str(ROOT),
               "--run-c", "/" + src.relative_to(ROOT).as_posix(), "--wasm", "--",
               "/" + work.relative_to(ROOT).as_posix()]
        env = dict(os.environ, INTERP_RUN_VAR="forty-two")
        # The two streams into one, in order, as the unit test captures them.
        r = subprocess.run(cmd, input=stdin, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env)
        got = r.stdout.decode("utf-8", "replace")
        if r.returncode != 0:
            got += f"exit status {r.returncode}\n"
        expected = FIXTURES / f"{name}.expected"
        if args.bless:
            expected.write_text(got, encoding="utf-8")
            print(f"blessed {name}")
            continue
        want = expected.read_text(encoding="utf-8") if expected.exists() else ""
        if strip_stack(got) != strip_stack(want):
            failures += 1
            print(f"FAIL wasm-backend {name}\n--- got:\n{got}--- want:\n{want}")
        else:
            print(f"ok   wasm-backend {name}")
    print(f"{len(names) - failures} of {len(names)} fixtures match")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
