# hello-cmake: a RitterOS program built by CMake (43d): a C++ program, a
# shared library of its own in C, and a RitterOS library found through
# pkg-config. (43e) Installed as an app's bundle: hello-cmake.program/ with
# hello-cmake and lib/libgreet.so.1 (the program looks in $ORIGIN/lib first).
# Built on this computer:
#   cmake -S . -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=RITTEROS/sdk/ritteros.cmake
#   cmake --build build
cmake_minimum_required(VERSION 3.20)
project(hello_cmake C CXX)

find_package(PkgConfig REQUIRED)
pkg_check_modules(IMAGE REQUIRED IMPORTED_TARGET libimage)

add_library(greet SHARED greet.c)
set_target_properties(greet PROPERTIES SOVERSION 1)

add_executable(hello-cmake main.cpp)
target_link_libraries(hello-cmake PRIVATE greet PkgConfig::IMAGE)
target_compile_features(hello-cmake PRIVATE cxx_std_23)
