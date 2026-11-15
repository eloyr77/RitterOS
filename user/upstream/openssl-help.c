/*
 * (46b) The openssl program's PROGRAM_HELP line, which RitterOS's shell
 * (`help`) and man's index read from every program in /system/<arch>/bin.
 * OpenSSL's own sources have none: this object is linked into the program
 * only (user/upstream/openssl.conf's bin_lflags).
 */
#include "ritteros.h"

PROGRAM_HELP("openssl COMMAND [OPTION...]",
             "OpenSSL's tool: TLS connections, keys, certificates, digests and ciphers");
