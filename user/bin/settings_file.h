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
#include <stdlib.h>
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

/* (69c) path's default text (common/settings_defaults.h), a user's files
 * by their name in ~/.config; "" for any other file. */
static inline const char *settings_default_text(const char *path)
{
    static const struct { const char *path, *text; } known[] = {
        { SETTINGS_CPU, SETTINGS_CPU_TEXT }, { SETTINGS_POWER, SETTINGS_POWER_TEXT },
        { SETTINGS_DISPLAYS, SETTINGS_DISPLAYS_TEXT }, { SETTINGS_SWAP, SETTINGS_SWAP_TEXT },
        { SETTINGS_WIFI_RADIO, SETTINGS_WIFI_RADIO_TEXT }, { SETTINGS_ROUTING, SETTINGS_ROUTING_TEXT },
        { SETTINGS_VIRTUAL, SETTINGS_VIRTUAL_TEXT }, { SETTINGS_TIME, SETTINGS_TIME_TEXT },
        { SETTINGS_SOUND, SETTINGS_SOUND_TEXT }, { SETTINGS_SERVICES, SETTINGS_SERVICES_TEXT },
        { SETTINGS_SUDO, SETTINGS_SUDO_TEXT },
    };
    for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++)
        if (!strcmp(path, known[i].path))
            return known[i].text;
    size_t n = strlen(path), w = strlen("/" SETTINGS_USER_WINDOWS), t = strlen("/" SETTINGS_USER_FILETYPES);
    if (n >= w && !strcmp(path + n - w, "/" SETTINGS_USER_WINDOWS))
        return SETTINGS_USER_WINDOWS_TEXT;
    if (n >= t && !strcmp(path + n - t, "/" SETTINGS_USER_FILETYPES))
        return SETTINGS_USER_FILETYPES_TEXT;
    size_t r = strlen("/" SETTINGS_USER_KEYRING);
    if (n >= r && !strcmp(path + n - r, "/" SETTINGS_USER_KEYRING))
        return SETTINGS_USER_KEYRING_TEXT;
    return "";
}

/* (69c) `key = value` in place of key's line of the file at path (added at
 * its end when it has none; later lines of the same key go), or key's line
 * taken out (value 0): the other lines and the comments stay (the user's
 * rule: a program changes only its line). A missing file starts as its
 * default text. 0, or -1 with errno. One writer for every program that
 * changes a setting (soundd's volume, net's virtual cards, the settings
 * app). */
static inline int settings_set(const char *path, const char *key, const char *value)
{
    struct stat st;
    size_t size = stat(path, &st) == 0 && st.st_size > 0 ? (size_t)st.st_size : 0;
    const char *dflt = size ? "" : settings_default_text(path);
    size_t room = size + strlen(dflt) + strlen(key) + (value ? strlen(value) : 0) + 8;
    char *old = malloc(size + strlen(dflt) + 1), *content = malloc(room);
    if (!old || !content) {
        free(old), free(content);
        errno = ENOMEM;
        return -1;
    }
    ssize_t got = 0;
    int fd = open(path, O_RDONLY);
    if (fd >= 0) {
        got = size ? read(fd, old, size) : 0;
        close(fd);
    }
    if (got <= 0)
        got = snprintf(old, strlen(dflt) + 1, "%s", dflt);
    old[got] = 0;
    size_t n = 0, klen = strlen(key);
    bool replaced = false;
    for (char *line = old; *line;) {
        char *end = strchr(line, '\n');
        size_t len = end ? (size_t)(end - line) + 1 : strlen(line);
        const char *k = line + strspn(line, " \t");
        bool is_key = !strncmp(k, key, klen) && k[klen] && strchr(" \t=", k[klen]);
        if (is_key && !replaced && value) {
            n += (size_t)snprintf(content + n, room - n, "%s = %s\n", key, value);
        } else if (!is_key && n + len + 1 < room) {
            memcpy(content + n, line, len);
            n += len;
            if (!end)
                content[n++] = '\n';
        }
        replaced |= is_key;
        line += len;
    }
    if (!replaced && value)
        n += (size_t)snprintf(content + n, room - n, "%s = %s\n", key, value);
    free(old);
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    bool ok = fd >= 0 && write(fd, content, n) == (ssize_t)n;
    int saved = errno;
    if (fd >= 0 && close(fd) < 0 && ok)
        ok = false, saved = errno;
    free(content);
    errno = saved;
    return ok ? 0 : -1;
}

#endif
