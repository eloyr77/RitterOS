/*
 * (43e) Where a library is looked for, ld.so's rule (ldd includes it too, so
 * both find the same file): each folder of the RUNPATH of the object that
 * needs it, then each of the program's (an app's libraries find each other
 * as macOS's @rpath does, without a RUNPATH of their own), then
 * /system/<arch>/lib. A RUNPATH is folders with ':' between; `$ORIGIN` (or
 * `${ORIGIN}`) is the folder of the object it is written in, so an app's
 * bundle keeps its libraries in APP.program/lib with `$ORIGIN/lib`.
 */
#ifndef RITTEROS_LDSO_RUNPATH_H
#define RITTEROS_LDSO_RUNPATH_H

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

/* The next folder of a RUNPATH (*list moves past it) into out, without a
 * '/' at its end; false at the list's end. Skipped: empty and relative
 * folders, other $ names ($LIB, $PLATFORM), and $ORIGIN when it is unknown
 * (origin 0 or "") or the program is secure (a setuid program run by
 * another user, AT_SECURE: else anyone could link it into a folder of
 * their own beside a library of their own, as Linux's loader refuses). */
static bool runpath_next(const char **list, const char *origin, bool secure, char *out, size_t size)
{
    while (**list) {
        const char *entry = *list, *end = strchr(entry, ':');
        if (!end)
            end = entry + strlen(entry);
        *list = *end ? end + 1 : end;
        size_t n = 0;
        bool ok = true;
        for (const char *c = entry; c < end && ok;) {
            const char *piece = c;
            size_t length = 1;
            if (*c == '$') {
                size_t name = (size_t)(end - c) >= 7 && !memcmp(c, "$ORIGIN", 7) ? 7
                            : (size_t)(end - c) >= 9 && !memcmp(c, "${ORIGIN}", 9) ? 9 : 0;
                char next = c + name < end ? c[name] : 0;
                if (!name || (name == 7 && (next == '_' || (next >= '0' && next <= '9') ||
                                            ((next | 0x20) >= 'a' && (next | 0x20) <= 'z'))) ||
                    secure || !origin || !origin[0]) {
                    ok = false;                 /* (another name, or no origin to use) */
                    break;
                }
                piece = origin;
                length = strlen(origin);
                c += name;
            } else {
                c++;
            }
            if (n + length >= size)
                ok = false;
            else
                memcpy(out + n, piece, length), n += length;
        }
        while (ok && n > 1 && out[n - 1] == '/')
            n--;
        out[ok ? n : 0] = 0;
        if (ok && n && out[0] == '/')
            return true;
    }
    return false;
}

/* The folder of path into out (no '/' at its end; "" without one). */
static void runpath_origin(const char *path, char *out, size_t size)
{
    const char *slash = strrchr(path, '/');
    size_t n = slash ? (size_t)(slash - path) : 0;
    if (slash == path)
        n = 1;                                  /* (the root's "/") */
    if (n >= size)
        n = 0;
    memcpy(out, path, n);
    out[n] = 0;
}

#endif
