/* Kernel command line: space-separated words, either "key" or "key=value". */
#ifndef RITTEROS_CMDLINE_H
#define RITTEROS_CMDLINE_H

#include <stdbool.h>
#include <stddef.h>

/*
 * Find key in the command line. Copies its value (empty for a bare key) into
 * value, truncated to size - 1 characters, and returns true; false if absent.
 */
bool cmdline_get(const char *cmdline, const char *key, char *value, size_t size);

#endif
