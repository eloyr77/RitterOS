# hello-meson: a RitterOS program built by Meson (43d): a C++ program, a
# shared library of its own in C, and a RitterOS library found through
# pkg-config. (43e) Installed as an app's bundle: hello-meson.program/ with
# hello-meson and lib/libgreet.so.1 (the program looks in $ORIGIN/lib first).
# Built on this computer:
#   meson setup build --cross-file RITTEROS/sdk/ritteros.meson
#   meson compile -C build
project('hello-meson', 'c', 'cpp', default_options: ['cpp_std=c++23'])

image = dependency('libimage')
greet = shared_library('greet', 'greet.c', soversion: '1')
executable('hello-meson', 'main.cpp', link_with: greet, dependencies: image)
