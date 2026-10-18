#!/usr/bin/env python3
"""Builds the compiler to WebAssembly and runs a program through it
(docs/design/interpreter.md section 8, step 11 of section 14).

    python test/wasm_build.py [--build-dir DIR] [--exe goose.wasm] [--fetch]

With wasi-sdk at $WASI_SDK_PATH (or a --build-dir already configured with
cmake/wasm32-wasi.cmake, or --fetch, which lets the toolchain file fetch
the sdk into build/wasi-sdk), configures build/wasm with the toolchain file,
builds goose.wasm, and runs the hello-world fixture and the sieve sample
under node's WASI through scripts/wasm_run.mjs, comparing their output with
the blessed files. Exits 0 when they match, 1 on a failure, and 3 where
wasi-sdk or node is absent, which the test runner reports as a skip.
"""

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent


def run(argv, **kw):
    return subprocess.run(argv, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                          errors="replace", **kw)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build-dir", default=str(ROOT / "build" / "wasm"),
                    help="where goose.wasm is configured and built (default build/wasm)")
    ap.add_argument("--exe", help="a goose.wasm built already, to run without building")
    ap.add_argument("--fetch", action="store_true",
                    help="without WASI_SDK_PATH, configure anyway: the toolchain file fetches wasi-sdk")
    args = ap.parse_args()
    node = shutil.which("node")
    if not node:
        print("skip: node is not installed, which runs the module")
        return 3
    exe = args.exe
    if not exe:
        sdk = os.environ.get("WASI_SDK_PATH")
        builddir = Path(args.build_dir)
        if not sdk and not args.fetch and not (builddir / "CMakeCache.txt").exists():
            print("skip: WASI_SDK_PATH is not set and no build is configured (--fetch fetches wasi-sdk)")
            return 3
        if not (builddir / "CMakeCache.txt").exists():
            r = run(["cmake", "-S", str(ROOT), "-B", str(builddir), "-G", "Ninja",
                     "-DCMAKE_BUILD_TYPE=Release",
                     f"-DCMAKE_TOOLCHAIN_FILE={ROOT / 'cmake' / 'wasm32-wasi.cmake'}"])
            if r.returncode != 0:
                print(r.stdout)
                print("FAIL wasm configure")
                return 1
        r = run(["cmake", "--build", str(builddir), "--target", "goose"])
        if r.returncode != 0:
            print(r.stdout[-4000:])
            print("FAIL wasm build")
            return 1
        exe = str(builddir / "goose.wasm")
    failures = 0
    # The directory preopened as /: the repository, so the standard library
    # is found beside the sources as it is natively (../stdlib of a test).
    checks = [
        ("hello_world", ["run", "/test/codegen/hello_world.goose", "--stdlib", "/stdlib"],
         ROOT / "test" / "expected" / "hello_world.out"),
        ("sieve", ["run", "/samples/07_sieve.goose", "-O2", "--stdlib", "/stdlib"],
         ROOT / "samples" / "expected" / "sieve.out"),
    ]
    for name, cmd, expected in checks:
        # The program's stdout is compared; the compiler's progress and a
        # sample's timing go to stderr.
        r = subprocess.run([node, "--no-warnings", str(ROOT / "scripts" / "wasm_run.mjs"), exe,
                            str(ROOT), *cmd],
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, errors="replace")
        want = expected.read_text(encoding="utf-8").rstrip("\n")
        got = r.stdout.rstrip("\n")
        if r.returncode != 0 or got != want:
            failures += 1
            print(f"--- got (exit {r.returncode}):\n{r.stdout}\n--- stderr:\n{r.stderr}\n--- want:\n{want}")
            print(f"FAIL wasm {name}")
        else:
            print(f"ok   wasm {name}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
