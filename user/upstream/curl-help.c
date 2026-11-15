/*
 * (46d-2) The curl program's PROGRAM_HELP line, which RitterOS's shell
 * (`help`) and man's index read from every program in /system/<arch>/bin.
 * curl's own sources have none: this object is linked into the program
 * (the Makefile gives it to curl's CMake as CMAKE_EXE_LINKER_FLAGS).
 */
#include "ritteros.h"

PROGRAM_HELP("curl [OPTION...] URL...",
             "transfer data from or to a URL: HTTP/1.1 and HTTP/2, HTTPS, WebSockets, proxies");
