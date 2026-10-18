# The toolchain for building the compiler to WebAssembly (docs/design/
# interpreter.md §8): clang from wasi-sdk, its sysroot, and the flags the
# compiler's own code needs. Configure with
#
#     cmake -S . -B build/wasm -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/wasm32-wasi.cmake
#
# with wasi-sdk at WASI_SDK_PREFIX (a cache variable) or $WASI_SDK_PATH, or,
# with neither set, the release below fetched for the host into
# build/wasi-sdk, once, for every build directory to share. The normal
# CMakeLists.txt then builds `goose` as a reactor module, goose.wasm, with
# GOOSE_NO_JIT set, since TinyCC does not target Wasm, and the interpreter
# in its place.

set(GOOSE_WASI_SDK_VERSION 34)

# The release's tarball for the host, checked against the digest GitHub
# lists for it, unpacked beside the others and named by the result.
function(goose_fetch_wasi_sdk outvar)
    set(ver "${GOOSE_WASI_SDK_VERSION}")
    if(CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
        set(arch x86_64)
    elseif(CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
        set(arch arm64)
    else()
        message(FATAL_ERROR "no wasi-sdk ${ver} release for a ${CMAKE_HOST_SYSTEM_PROCESSOR} host: "
                            "set WASI_SDK_PATH to one built for it")
    endif()
    if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
        set(os linux)
    elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
        set(os macos)
    elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows")
        set(os windows)
    else()
        message(FATAL_ERROR "no wasi-sdk ${ver} release for ${CMAKE_HOST_SYSTEM_NAME}: "
                            "set WASI_SDK_PATH to one built for it")
    endif()
    set(sha256_x86_64-linux b761e3a0721dbae9c09a0059e5fdb2bf917d1b4a8a7b430fb3b5aafb0984b2c4)
    set(sha256_arm64-linux f7e243dff54d60bcc576e94d6166b69f410f2500ae4a9ceef34315be10e77971)
    set(sha256_x86_64-macos 87d27fa8adc68dee59bfbf2e22a6d34ef717c34d6bf1d8af2a56fc929d9ce0eb)
    set(sha256_arm64-macos 9c59398106b417f8f14913380fdf0097a8cc0ff4af9eb3ce0065a859e88d49e9)
    set(sha256_x86_64-windows cccb5c323a9b34f0349a9b09e8804a0a7632c68c3310f4b5f437ed57d7e71d8f)
    set(sha256_arm64-windows 45e1c71f3e965621e7b98ebe1d37b0e4b1f77f3e8072113ffb4534e67b1a4b7c)
    set(name "wasi-sdk-${ver}.0-${arch}-${os}")
    get_filename_component(root "${CMAKE_CURRENT_LIST_DIR}/../build/wasi-sdk" ABSOLUTE)
    set(dir "${root}/${name}")
    if(NOT EXISTS "${dir}/bin")
        set(url "https://github.com/WebAssembly/wasi-sdk/releases/download/wasi-sdk-${ver}/${name}.tar.gz")
        message(STATUS "wasi-sdk: fetching ${url}")
        file(DOWNLOAD "${url}" "${root}/${name}.tar.gz" STATUS status
             EXPECTED_HASH "SHA256=${sha256_${arch}-${os}}")
        list(GET status 0 code)
        if(NOT code EQUAL 0)
            list(GET status 1 msg)
            file(REMOVE "${root}/${name}.tar.gz")
            message(FATAL_ERROR "wasi-sdk: cannot fetch ${url}: ${msg}")
        endif()
        # Unpacked aside and renamed into place, so an interrupted run
        # leaves nothing that looks finished.
        file(REMOVE_RECURSE "${root}/unpack")
        file(ARCHIVE_EXTRACT INPUT "${root}/${name}.tar.gz" DESTINATION "${root}/unpack")
        file(RENAME "${root}/unpack/${name}" "${dir}")
        file(REMOVE_RECURSE "${root}/unpack")
        file(REMOVE "${root}/${name}.tar.gz")
    endif()
    set(${outvar} "${dir}" PARENT_SCOPE)
endfunction()

if(NOT WASI_SDK_PREFIX)
    if(DEFINED ENV{WASI_SDK_PATH})
        set(WASI_SDK_PREFIX "$ENV{WASI_SDK_PATH}")
    else()
        goose_fetch_wasi_sdk(WASI_SDK_PREFIX)
        # The try_compile projects read this file again, without the cache.
        set(ENV{WASI_SDK_PATH} "${WASI_SDK_PREFIX}")
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
