# The toolchain for building the compiler to WebAssembly (docs/design/
# interpreter.md §8): clang from wasi-sdk, its sysroot, and the flags the
# compiler's own code needs. Configure with
#
#     cmake -S . -B build/wasm -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/wasm32-wasi.cmake
#
# with wasi-sdk at WASI_SDK_PREFIX (a cache variable) or $WASI_SDK_PATH. The
# normal CMakeLists.txt then builds `goose` as a reactor module, goose.wasm,
# with GOOSE_NO_JIT set, since TinyCC does not target Wasm, and the
# interpreter in its place.

if(NOT WASI_SDK_PREFIX)
    if(DEFINED ENV{WASI_SDK_PATH})
        set(WASI_SDK_PREFIX "$ENV{WASI_SDK_PATH}")
    else()
        message(FATAL_ERROR "set WASI_SDK_PREFIX (or WASI_SDK_PATH in the environment) to a wasi-sdk")
    endif()
endif()
set(WASI_SDK_PREFIX "${WASI_SDK_PREFIX}" CACHE PATH "Where wasi-sdk is")

set(CMAKE_SYSTEM_NAME WASI)
set(CMAKE_SYSTEM_VERSION 1)
set(CMAKE_SYSTEM_PROCESSOR wasm32)
set(CMAKE_C_COMPILER "${WASI_SDK_PREFIX}/bin/clang")
set(CMAKE_CXX_COMPILER "${WASI_SDK_PREFIX}/bin/clang++")
set(CMAKE_AR "${WASI_SDK_PREFIX}/bin/llvm-ar")
set(CMAKE_RANLIB "${WASI_SDK_PREFIX}/bin/llvm-ranlib")
# wasip1 is the WASI the browsers' shims and node implement; the bare
# wasm32-wasi spelling is what older releases called it.
set(CMAKE_C_COMPILER_TARGET wasm32-wasip1)
set(CMAKE_CXX_COMPILER_TARGET wasm32-wasip1)
set(CMAKE_SYSROOT "${WASI_SDK_PREFIX}/share/wasi-sysroot")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
# A static library of Wasm objects, nothing to run or probe.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# C++ exceptions over the exception-handling proposal, which the compiler's
# error reporting throws across its phases. wasi-sdk 34's libc++abi and
# libunwind for exceptions (lib/wasm32-wasip1/eh/) are built with the
# proposal's final form (exnref), where clang 23 still emits the legacy one
# by default, and a module may not mix them: hence the -mllvm flag, and the
# unwinder linked by name, which the driver leaves out.
set(CMAKE_CXX_FLAGS_INIT "-fwasm-exceptions -mllvm -wasm-use-legacy-eh=false")
# The shader compiler (cute_spirv) recovers from errors with setjmp and
# longjmp, which Wasm does over the same proposal: enabled for C, and its
# runtime linked.
set(CMAKE_C_FLAGS_INIT "-mexception-handling -mllvm -wasm-enable-sjlj -mllvm -wasm-use-legacy-eh=false")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-fwasm-exceptions -mllvm -wasm-use-legacy-eh=false -lunwind -lsetjmp")
# TinyCC does not target Wasm; the interpreter runs programs.
set(GOOSE_NO_JIT ON CACHE BOOL "Build without TinyCC; goose run uses the interpreter" FORCE)
