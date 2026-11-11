/*
 * find [DIR] [-name PATTERN] [-attr KEY[=VALUE]]...: search DIR (default:
 * the current directory) and everything below it, printing each path that
 * meets all conditions: a name matching PATTERN (* any characters, ? one),
 * an attribute KEY, or KEY with exactly VALUE. Without conditions it lists
 * everything.
 */
#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "ritteros.h"

PROGRAM_HELP("find [DIR] [-name PATTERN] [-attr KEY[=VALUE]]...",
             "search for files by name and attributes");

#define MAX_CONDITIONS 8
#define PATH_LENGTH 512

static const char *names[MAX_CONDITIONS], *attrs[MAX_CONDITIONS];
static int name_count, attr_count, status;
static char value[ATTR_VALUE_MAX];

static bool match(const char *pattern, const char *name)
{
    if (*pattern == 0)
        return *name == 0;
    if (*pattern == '*')
        return match(pattern + 1, name) || (*name && match(pattern, name + 1));
    if (*name == 0)
        return false;
    return (*pattern == '?' || *pattern == *name) && match(pattern + 1, name + 1);
}

/* An attribute condition: "KEY" (present) or "KEY=VALUE" (equal). */
static bool has_attr(const char *path, const char *condition)
{
    char key[ATTR_KEY_MAX + 2];
    const char *equals = strchr(condition, '=');
    size_t n = equals ? (size_t)(equals - condition) : strlen(condition);
    if (n >= sizeof(key))
        return false;
    memcpy(key, condition, n);
    key[n] = 0;
    int64_t length = ritteros_attr(ATTR_GET, path, key, value, sizeof(value));
    if (length < 0)
        return false;
    return !equals || ((size_t)length == strlen(equals + 1) &&
                       memcmp(value, equals + 1, (size_t)length) == 0);
}

static void consider(const char *path, const char *name)
{
    for (int i = 0; i < name_count; i++)
        if (!match(names[i], name))
            return;
    for (int i = 0; i < attr_count; i++)
        if (!has_attr(path, attrs[i]))
            return;
    printf("%s\n", path);
}

static void walk(char *path, size_t length)
{
    DIR *dir = opendir(path);
    if (!dir) {
        dprintf(STDERR_FILENO, "find: %s: %s\n", path, strerror(errno));
        status = 1;
        return;
    }
    struct dirent *e;
    while ((e = readdir(dir))) {
        size_t n = strlen(e->d_name);
        bool slash = length > 0 && path[length - 1] != '/';
        if (length + slash + n + 1 > PATH_LENGTH) {
            dprintf(STDERR_FILENO, "find: %s/%s: path too long\n", path, e->d_name);
            status = 1;
            continue;
        }
        if (slash)
            path[length] = '/';
        memcpy(path + length + slash, e->d_name, n + 1);
        consider(path, e->d_name);
        if (e->d_type == DT_DIR)
            walk(path, length + slash + n);
        path[length] = 0;
    }
    closedir(dir);
}

int main(int argc, char **argv)
{
    static char path[PATH_LENGTH];
    const char *start = ".";
    for (int i = 1; i < argc; i++) {
        bool is_name = strcmp(argv[i], "-name") == 0, is_attr = strcmp(argv[i], "-attr") == 0;
        if ((is_name || is_attr) && i + 1 < argc &&
            (is_name ? name_count : attr_count) < MAX_CONDITIONS) {
            if (is_name)
                names[name_count++] = argv[++i];
            else
                attrs[attr_count++] = argv[++i];
        } else if (i == 1 && argv[i][0] != '-') {
            start = argv[i];
        } else {
            dprintf(STDERR_FILENO, "usage: find [DIR] [-name PATTERN] [-attr KEY[=VALUE]]...\n");
            return 2;
        }
    }
    snprintf(path, sizeof(path), "%s", start);
    walk(path, strlen(path));
    return status;
}
