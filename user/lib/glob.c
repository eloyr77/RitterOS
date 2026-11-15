/*
 * (43a) glob: the pattern walked a part at a time; a part without a
 * wildcard is only looked at (it may be a name no folder lists), one with
 * a wildcard is matched against the folder's entries. The paths are
 * collected, then sorted (strcmp: the C locale's order).
 */
#include <dirent.h>
#include <errno.h>
#include <fnmatch.h>
#include <glob.h>
#include <pwd.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct found {
    char **paths;
    size_t count, room;
    bool no_space;
};

static void add(struct found *f, const char *path, bool mark)
{
    if (f->no_space)
        return;
    if (f->count == f->room) {
        size_t want = f->room ? f->room * 2 : 16;
        char **bigger = realloc(f->paths, want * sizeof(char *));
        if (!bigger) {
            f->no_space = true;
            return;
        }
        f->paths = bigger, f->room = want;
    }
    size_t n = strlen(path);
    char *copy = malloc(n + 2);
    if (!copy) {
        f->no_space = true;
        return;
    }
    memcpy(copy, path, n + 1);
    struct stat st;
    if (mark && n && path[n - 1] != '/' && stat(path, &st) == 0 && S_ISDIR(st.st_mode))
        copy[n] = '/', copy[n + 1] = 0;
    f->paths[f->count++] = copy;
}

static bool has_wildcard(const char *s, size_t n, bool escapes)
{
    for (size_t i = 0; i < n; i++) {
        if (escapes && s[i] == '\\' && i + 1 < n) {
            i++;
            continue;
        }
        if (s[i] == '*' || s[i] == '?' || s[i] == '[')
            return true;
    }
    return false;
}

/* The part's text without its backslashes (a part with no wildcard). */
static void unescape(char *out, const char *s, size_t n, bool escapes)
{
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        if (escapes && s[i] == '\\' && i + 1 < n)
            i++;
        out[k++] = s[i];
    }
    out[k] = 0;
}

/* dir (the path so far, "" at the start of a relative pattern) followed
 * by the parts of rest; false when the walk was stopped. */
static bool walk(char *dir, size_t dlen, const char *rest, int flags,
                 int (*errfunc)(const char *, int), struct found *f)
{
    while (*rest == '/')
        rest++;
    if (!*rest) {
        add(f, dlen ? dir : ".", flags & GLOB_MARK);
        return true;
    }
    size_t plen = strcspn(rest, "/");
    const char *after = rest + plen;
    bool escapes = !(flags & GLOB_NOESCAPE);
    char part[256 + 1];
    if (plen > 256)
        return true;
    if (!has_wildcard(rest, plen, escapes)) {
        char name[257];
        unescape(name, rest, plen, escapes);
        size_t n = dlen;
        if (n + strlen(name) + 2 > 4096)
            return true;
        if (n && dir[n - 1] != '/')
            dir[n++] = '/';
        strcpy(dir + n, name);
        struct stat st;
        if (lstat(dir, &st) == 0) {
            if (!*after)
                add(f, dir, flags & GLOB_MARK);
            else if (stat(dir, &st) == 0 && S_ISDIR(st.st_mode))
                if (!walk(dir, n + strlen(name), after, flags, errfunc, f))
                    return false;
        }
        dir[dlen] = 0;
        return true;
    }
    memcpy(part, rest, plen);
    part[plen] = 0;
    DIR *d = opendir(dlen ? dir : ".");
    if (!d) {
        if ((errfunc && errfunc(dlen ? dir : ".", errno)) || (flags & GLOB_ERR))
            return false;
        return true;
    }
    int fnflags = (escapes ? 0 : FNM_NOESCAPE) | ((flags & GLOB_PERIOD) ? 0 : FNM_PERIOD);
    bool ok = true;
    struct dirent *e;
    while (ok && (e = readdir(d))) {
        if ((!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) && part[0] != '.')
            continue;
        if (fnmatch(part, e->d_name, fnflags))
            continue;
        size_t n = dlen;
        if (n + strlen(e->d_name) + 2 > 4096)
            continue;
        if (n && dir[n - 1] != '/')
            dir[n++] = '/';
        strcpy(dir + n, e->d_name);
        if (!*after) {
            add(f, dir, flags & GLOB_MARK);
        } else {
            struct stat st;
            if (stat(dir, &st) == 0 && S_ISDIR(st.st_mode))
                ok = walk(dir, n + strlen(e->d_name), after, flags, errfunc, f);
        }
        dir[dlen] = 0;
    }
    closedir(d);
    return ok;
}

static int by_name(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

int glob(const char *pattern, int flags, int (*errfunc)(const char *path, int error), glob_t *pglob)
{
    if (!(flags & GLOB_APPEND)) {
        pglob->gl_pathc = 0;
        pglob->gl_pathv = 0;
        if (!(flags & GLOB_DOOFFS))
            pglob->gl_offs = 0;
    }
    char dir[4096 + 2] = "";
    size_t dlen = 0;
    const char *rest = pattern;
    if ((flags & GLOB_TILDE) && pattern[0] == '~') {
        size_t n = strcspn(pattern + 1, "/");
        const char *home = 0;
        if (n == 0) {
            home = getenv("HOME");
            if (!home) {
                struct passwd *pw = getpwuid(getuid());
                home = pw ? pw->pw_dir : 0;
            }
        } else if (n < 64) {
            char user[64];
            memcpy(user, pattern + 1, n);
            user[n] = 0;
            struct passwd *pw = getpwnam(user);
            home = pw ? pw->pw_dir : 0;
        }
        if (home && strlen(home) < 4096) {
            strcpy(dir, home);
            dlen = strlen(dir);
            rest = pattern + 1 + n;
        }
    } else if (pattern[0] == '/') {
        strcpy(dir, "/");
        dlen = 1;
    }
    struct found f = { 0 };
    bool ok = walk(dir, dlen, rest, flags, errfunc, &f);
    if (f.no_space) {
        for (size_t i = 0; i < f.count; i++)
            free(f.paths[i]);
        free(f.paths);
        return GLOB_NOSPACE;
    }
    if (!ok) {
        for (size_t i = 0; i < f.count; i++)
            free(f.paths[i]);
        free(f.paths);
        return GLOB_ABORTED;
    }
    if (f.count == 0) {
        if (!(flags & GLOB_NOCHECK))
            return GLOB_NOMATCH;
        add(&f, pattern, false);
        if (f.no_space)
            return GLOB_NOSPACE;
    }
    if (!(flags & GLOB_NOSORT))
        qsort(f.paths, f.count, sizeof(char *), by_name);
    size_t offs = (flags & GLOB_DOOFFS) ? pglob->gl_offs : 0;
    size_t old = pglob->gl_pathc;
    char **v = realloc(pglob->gl_pathv, (offs + old + f.count + 1) * sizeof(char *));
    if (!v) {
        for (size_t i = 0; i < f.count; i++)
            free(f.paths[i]);
        free(f.paths);
        return GLOB_NOSPACE;
    }
    if (!pglob->gl_pathv)
        for (size_t i = 0; i < offs; i++)
            v[i] = 0;
    memcpy(v + offs + old, f.paths, f.count * sizeof(char *));
    v[offs + old + f.count] = 0;
    free(f.paths);
    pglob->gl_pathv = v;
    pglob->gl_pathc = old + f.count;
    return 0;
}

void globfree(glob_t *pglob)
{
    if (!pglob->gl_pathv)
        return;
    for (size_t i = 0; i < pglob->gl_pathc; i++)
        free(pglob->gl_pathv[pglob->gl_offs + i]);
    free(pglob->gl_pathv);
    pglob->gl_pathv = 0;
    pglob->gl_pathc = 0;
}
