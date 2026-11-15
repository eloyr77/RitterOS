# netlibs: a RitterOS program built by CMake (46e) against the system
# libraries from upstream, found as ported projects find them:
# find_package(OpenSSL), (CURL), (ZLIB) and (LibPSL) (RitterOS's
# sdk/cmake/FindLibPSL.cmake), and pkg-config (sdk/pkgconfig) for brotli
# and nghttp2. A command-line program (/system/ARCH/bin/netlibs).
# Built on this computer:
#   cmake -S . -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=RITTEROS/sdk/ritteros.cmake
#   cmake --build build
# (RitterOS's `make sdk` first: the libraries, their headers and link names.)
cmake_minimum_required(VERSION 3.20)
project(netlibs C)

find_package(OpenSSL 3 REQUIRED)
find_package(CURL REQUIRED)
find_package(ZLIB REQUIRED)
find_package(LibPSL REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(BROTLI REQUIRED IMPORTED_TARGET libbrotlidec libbrotlienc)
pkg_check_modules(NGHTTP2 REQUIRED IMPORTED_TARGET libnghttp2)

add_executable(netlibs main.c)
target_link_libraries(netlibs PRIVATE OpenSSL::SSL OpenSSL::Crypto CURL::libcurl ZLIB::ZLIB LibPSL::LibPSL
                      PkgConfig::BROTLI PkgConfig::NGHTTP2)
