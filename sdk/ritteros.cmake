# ritteros.cmake: CMake's toolchain file for RitterOS programs (43d), the
# same build ritteros.mk makes: zig's clang for C and C++ (RitterOS's
# headers, RitterOS's libc++ for C++), zig's lld linking with RitterOS's
# start code and shared libraries (C++: libc++, libc++abi, libunwind too).
#
#   cmake -S SRC -B build -G Ninja \
#         -DCMAKE_TOOLCHAIN_FILE=RITTEROS/sdk/ritteros.cmake [-DRITTEROS_ARCH=aarch64]
#   cmake --build build
#
# RITTEROS_ARCH: x86_64 (the default) or aarch64. RitterOS's libraries come
# from its checkout (this file's ../): run `make sdk` there first (ritteros.mk
# does it itself; CMake does not). Shared libraries (add_library SHARED) link
# against libritteros.so.1; pkg-config (find_package(PkgConfig)) finds
# RitterOS's libraries' .pc files (sdk/pkgconfig). Programs are built on this
# computer only: RitterOS has no CMake. See docs/porting.md.

set(RITTEROS_ARCH "x86_64" CACHE STRING "RitterOS's architecture: x86_64 or aarch64")
if(NOT RITTEROS_ARCH MATCHES "^(x86_64|aarch64)$")
    message(FATAL_ERROR "RITTEROS_ARCH must be x86_64 or aarch64, not '${RITTEROS_ARCH}'")
endif()
# (CMake's checks build test projects with this file again: they see the same.)
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES RITTEROS_ARCH)

get_filename_component(RITTEROS_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
find_program(RITTEROS_ZIG zig PATHS "${RITTEROS_DIR}/tools/zig" NO_DEFAULT_PATH)
find_program(RITTEROS_ZIG zig)
if(NOT RITTEROS_ZIG)
    message(FATAL_ERROR "no zig: run RitterOS's scripts/setup-toolchain.sh, or put Zig 0.17.0 in PATH")
endif()
# Zig's own headers: `zig env` names its lib folder, relative to the folder
# it runs in when it can (run in zig's own folder here).
get_filename_component(_zig_dir "${RITTEROS_ZIG}" DIRECTORY)
execute_process(COMMAND "${RITTEROS_ZIG}" env WORKING_DIRECTORY "${_zig_dir}" OUTPUT_VARIABLE _zig_env)
string(REGEX MATCH "\\.lib_dir = \"([^\"]*)\"" _ "${_zig_env}")
get_filename_component(RITTEROS_ZIG_LIB "${CMAKE_MATCH_1}" ABSOLUTE BASE_DIR "${_zig_dir}")

set(CMAKE_SYSTEM_NAME RitterOS)                 # (sdk/cmake/Platform/RitterOS.cmake)
set(CMAKE_SYSTEM_PROCESSOR ${RITTEROS_ARCH})
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake")
# (71b) One program, sdk/zig-clang (zig's clang): projects that run the
# compiler themselves drop a compiler's argument (libpng's -E).
set(CMAKE_C_COMPILER "${RITTEROS_DIR}/sdk/zig-clang")
set(CMAKE_CXX_COMPILER "${RITTEROS_DIR}/sdk/zig-clang")   # (no clang++: clang takes .cpp as C++)
set(CMAKE_C_COMPILER_TARGET ${RITTEROS_ARCH}-unknown-none-elf)
set(CMAKE_CXX_COMPILER_TARGET ${RITTEROS_ARCH}-unknown-none-elf)
set(CMAKE_ASM_COMPILER "${RITTEROS_DIR}/sdk/zig-clang")
set(CMAKE_ASM_COMPILER_TARGET ${RITTEROS_ARCH}-unknown-none-elf)

set(_lib "${RITTEROS_DIR}/build/system/${RITTEROS_ARCH}/lib")
set(RITTEROS_START "${RITTEROS_DIR}/build/${RITTEROS_ARCH}/user/user/arch/${RITTEROS_ARCH}/start.S.o")
set(RITTEROS_LIB_PIC "${RITTEROS_DIR}/build/${RITTEROS_ARCH}/user/libritteros-pic.a")
# (46d-1) CMake's checks build programs and link them, with RitterOS's start
# code and libraries, so a check a link answers (a function or a library
# exists, a file without main) answers as on RitterOS; before, they only
# compiled, and every such check said yes. RitterOS's files must be built.
foreach(_f "${RITTEROS_START}" "${RITTEROS_LIB_PIC}" "${_lib}/libritteros.so.1")
    if(NOT EXISTS "${_f}")
        message(FATAL_ERROR "${_f} is missing: run `make sdk` in ${RITTEROS_DIR} first")
    endif()
endforeach()
set(RITTEROS_SHARED_LIBS "${_lib}/libritteros.so.1 ${_lib}/libwindow.so.1 ${_lib}/libimage.so.1 ${_lib}/libaudio.so.1 ${_lib}/libvideo.so.1 ${_lib}/libdraw.so.1 ${_lib}/libwidget.so.1 ${_lib}/libtls.so.1 ${_lib}/libkeyring.so.1")
set(RITTEROS_CXX_LIBS "${_lib}/libc++.so.1 ${_lib}/libc++abi.so.1 ${_lib}/libunwind.so.1")

set(CMAKE_C_FLAGS_INIT "-ffreestanding -nostdinc -isystem ${RITTEROS_ZIG_LIB}/include -isystem ${RITTEROS_DIR}/user/include -isystem ${RITTEROS_DIR}/build/upstream/include -fno-stack-protector -fno-asynchronous-unwind-tables")
set(CMAKE_CXX_FLAGS_INIT "-nostdinc -nostdinc++ -include ${RITTEROS_DIR}/user/cxx/__config_site -isystem ${RITTEROS_ZIG_LIB}/libcxx/include -isystem ${RITTEROS_ZIG_LIB}/libcxxabi/include -isystem ${RITTEROS_DIR}/user/include -isystem ${RITTEROS_DIR}/build/upstream/include -isystem ${RITTEROS_ZIG_LIB}/include -fno-stack-protector -funwind-tables")
set(CMAKE_ASM_FLAGS_INIT "-nostdinc -isystem ${RITTEROS_ZIG_LIB}/include -isystem ${RITTEROS_DIR}/user/include")
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Programs: position-independent, loaded by ld.so (ritteros.mk's LINK =
# shared), (43e) looking for libraries in $ORIGIN/lib first (an app's own,
# in its bundle's lib); C++ ones with the frames' index and the C++
# libraries.
# (71b) lld through sdk/ritteros-ld --plain: -pthread (in static libraries'
# pkg-config files, request 6) dropped; -lm, -lpthread, -ldl, -lrt are
# make sdk's empty archives (all in libritteros).
set(_ld "${RITTEROS_DIR}/sdk/ritteros-ld --plain -L ${RITTEROS_DIR}/build/sdk/${RITTEROS_ARCH}/lib")
set(_exe "-T ${RITTEROS_DIR}/user/user.ld -nostdlib -z max-page-size=4096 -pie -dynamic-linker /system/${RITTEROS_ARCH}/lib/ld.so -z now -z relro -z nocopyreloc --hash-style=sysv")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-rpath '$ORIGIN/lib'")   # (as a flag: each generator quotes it its way)
set(CMAKE_C_LINK_EXECUTABLE "${_ld} ${_exe} <LINK_FLAGS> -o <TARGET> ${RITTEROS_START} <OBJECTS> <LINK_LIBRARIES> --as-needed ${RITTEROS_SHARED_LIBS} --no-as-needed ${RITTEROS_LIB_PIC}")
set(CMAKE_CXX_LINK_EXECUTABLE "${_ld} ${_exe} --eh-frame-hdr <LINK_FLAGS> -o <TARGET> ${RITTEROS_START} <OBJECTS> <LINK_LIBRARIES> --as-needed ${RITTEROS_CXX_LIBS} ${RITTEROS_SHARED_LIBS} --no-as-needed ${RITTEROS_LIB_PIC}")
# Shared libraries, as RitterOS's own: every symbol resolved at the link.
set(_so "-shared -z max-page-size=4096 -z now -z relro -z defs --hash-style=sysv --eh-frame-hdr")
set(CMAKE_C_CREATE_SHARED_LIBRARY "${_ld} ${_so} <LINK_FLAGS> -soname <TARGET_SONAME> -o <TARGET> <OBJECTS> <LINK_LIBRARIES> --as-needed ${RITTEROS_SHARED_LIBS} --no-as-needed ${RITTEROS_LIB_PIC}")
set(CMAKE_CXX_CREATE_SHARED_LIBRARY "${_ld} ${_so} <LINK_FLAGS> -soname <TARGET_SONAME> -o <TARGET> <OBJECTS> <LINK_LIBRARIES> --as-needed ${RITTEROS_CXX_LIBS} ${RITTEROS_SHARED_LIBS} --no-as-needed ${RITTEROS_LIB_PIC}")
set(CMAKE_C_CREATE_SHARED_MODULE "${CMAKE_C_CREATE_SHARED_LIBRARY}")
set(CMAKE_CXX_CREATE_SHARED_MODULE "${CMAKE_CXX_CREATE_SHARED_LIBRARY}")
set(CMAKE_AR "${RITTEROS_ZIG}" CACHE FILEPATH "")
# (69m) The archive made anew: `ar rcs` adds to one already there, so an
# object whose source left the library stayed in it (and a linker could take
# it over the new code: the browser agent's canvas encoders). Meson's own
# rule removes it first too.
set(CMAKE_C_CREATE_STATIC_LIBRARY "${CMAKE_COMMAND} -E rm -f <TARGET>" "${RITTEROS_ZIG} ar rcs <TARGET> <OBJECTS>")
set(CMAKE_CXX_CREATE_STATIC_LIBRARY "${CMAKE_COMMAND} -E rm -f <TARGET>" "${RITTEROS_ZIG} ar rcs <TARGET> <OBJECTS>")

# Only RitterOS's files, never this computer's: (46e) the system libraries
# from upstream found by their link names (build/sdk/ARCH/lib: libssl.so for
# libssl.so.3 ...) and their headers (build/upstream/include), so
# find_package(OpenSSL), (CURL), (ZLIB) and this folder's cmake/
# FindLibPSL.cmake find them; the headers are also on every compile line,
# as /system/include is on the machine.
set(CMAKE_FIND_ROOT_PATH "${RITTEROS_DIR}/user" "${RITTEROS_DIR}/build/sdk/${RITTEROS_ARCH}"
    "${RITTEROS_DIR}/build/upstream" "${RITTEROS_DIR}/build/system/${RITTEROS_ARCH}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
# pkg-config: RitterOS's .pc files only, for this architecture.
set(ENV{PKG_CONFIG_LIBDIR} "${RITTEROS_DIR}/sdk/pkgconfig")
set(ENV{PKG_CONFIG_PATH} "")
set(PKG_CONFIG_ARGN "--define-variable=arch=${RITTEROS_ARCH}")
# (the pinned pkgconf's own program, not its Python wrapper, which talks)
file(GLOB _pkgconf "${RITTEROS_DIR}/tools/venv/lib/python*/site-packages/pkgconf/.bin/pkgconf")
if(_pkgconf)
    list(GET _pkgconf 0 PKG_CONFIG_EXECUTABLE)
else()
    find_program(PKG_CONFIG_EXECUTABLE NAMES pkgconf pkg-config)
endif()
