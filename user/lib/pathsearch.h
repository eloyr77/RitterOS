/*
 * Internal to the library (55c): what execvp and posix_spawnp share.
 * `path_search` tries NAME in each folder of PATH (without PATH: the
 * system's bin, then apps), calling try(path, ctx) until one returns 0;
 * an error other than ENOENT/ENOTDIR is remembered and returned if no
 * folder works. `is_script` tells a file the system will not start that
 * is no binary (no ELF header, no zero byte in 512 bytes): POSIX runs it
 * through sh.
 */
#ifndef RITTEROS_PATHSEARCH_H
#define RITTEROS_PATHSEARCH_H

#include <stdbool.h>

#if defined(__x86_64__)
#define SYSTEM_DIR "/system/x86_64"
#else
#define SYSTEM_DIR "/system/aarch64"
#endif

int __ritteros_path_search(const char *name, int (*try)(const char *path, void *ctx), void *ctx);
bool __ritteros_is_script(const char *path);

#endif
