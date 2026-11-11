/*
 * (37e-1b, the user's rule) A program writes the settings file it reads
 * when it is missing, with its default text (common/settings_defaults.h):
 * a new system gets it the first time it is looked for, and a deleted one
 * comes back. Only root may write /system/config: anyone else goes on with
 * the defaults the file would hold.
 */
#ifndef SETTINGS_FILE_H
#define SETTINGS_FILE_H

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../../common/settings_defaults.h"

/* path written with text if it does not exist (its folder made too, as
 * wifi/ for wifi/radio); true when it was written. */
static inline bool settings_make(const char *path, const char *text)
{
    struct stat st;
    if (stat(path, &st) == 0 || errno != ENOENT)
        return false;
    char dir[128];
    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash && slash != dir) {
        *slash = 0;
        if (stat(dir, &st) < 0 && mkdir(dir, 0755) < 0)
            return false;
    }
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0)
        return false;
    size_t n = strlen(text);
    bool ok = write(fd, text, n) == (ssize_t)n;
    if (close(fd) < 0)
        ok = false;
    return ok;
}

#endif
