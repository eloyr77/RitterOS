# (71b, the browser agent's request 4) CMake's LINKER: options as they are:
# sdk/ritteros.cmake links with lld itself, not through clang, so the
# compiler module's -Xlinker wrapper (set after the platform file: this
# file comes after it) stopped lld ("unknown argument '-Xlinker'"). Each
# option is passed alone: LINKER:-z,defs gives -z defs.
set(CMAKE_C_LINKER_WRAPPER_FLAG "")
unset(CMAKE_C_LINKER_WRAPPER_FLAG_SEP)
