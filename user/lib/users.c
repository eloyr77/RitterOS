/*
 * Users and passwords. /system/config/users (everyone may read it):
 * name:uid:gid:home per line. /system/config/passwords (root only): name,
 * PBKDF2 iterations, salt and hash (hex), name:iterations:salt:hash.
 */
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "ritteros.h"

#define LINE_LENGTH 256

static char file[8192];
static char name[USER_NAME_MAX + 1], home[128];
static struct passwd entry = { name, 0, 0, home };

/* The whole of a small file into file[], NUL-terminated; its length or -1. */
static long load(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    long n = 0, r;
    while (n < (long)sizeof(file) - 1 && (r = read(fd, file + n, sizeof(file) - 1 - (size_t)n)) > 0)
        n += r;
    close(fd);
    file[n] = 0;
    return n;
}

static unsigned long number(const char *s, const char **end)
{
    unsigned long v = 0;
    while (*s >= '0' && *s <= '9')
        v = v * 10 + (unsigned long)(*s++ - '0');
    *end = s;
    return v;
}

/* The users file's lines one by one, as an entry; want_name or want_uid
 * (when want_name is 0) picks one. */
static struct passwd *search(const char *want_name, uid_t want_uid)
{
    if (load(USERS_FILE) < 0)
        return 0;
    for (char *line = file; *line;) {
        char *next = strchr(line, '\n');
        if (next)
            *next++ = 0;
        else
            next = line + strlen(line);
        char *colon = strchr(line, ':');
        if (colon && (size_t)(colon - line) <= USER_NAME_MAX) {
            const char *p = colon + 1;
            unsigned long uid = number(p, &p), gid = 0;
            if (*p == ':')
                gid = number(p + 1, &p);
            if (*p == ':' &&
                (want_name ? (size_t)(colon - line) == strlen(want_name) &&
                                 strncmp(line, want_name, (size_t)(colon - line)) == 0
                           : uid == want_uid)) {
                memcpy(name, line, (size_t)(colon - line));
                name[colon - line] = 0;
                snprintf(home, sizeof(home), "%s", p + 1);
                entry.pw_uid = (uid_t)uid;
                entry.pw_gid = (gid_t)gid;
                return &entry;
            }
        }
        line = next;
    }
    return 0;
}

/* root exists without a line in the file (a fresh system has none). */
static struct passwd *root(void)
{
    snprintf(name, sizeof(name), "root");
    snprintf(home, sizeof(home), "/home/root");
    entry.pw_uid = entry.pw_gid = 0;
    return &entry;
}

struct passwd *getpwnam(const char *want)
{
    struct passwd *p = search(want, 0);
    return p ? p : strcmp(want, "root") == 0 ? root() : 0;
}

struct passwd *getpwuid(uid_t uid)
{
    struct passwd *p = search(0, uid);
    return p ? p : uid == 0 ? root() : 0;
}

bool user_name_valid(const char *s)
{
    size_t n = strlen(s);
    if (n == 0 || n > USER_NAME_MAX || !((s[0] >= 'a' && s[0] <= 'z') || s[0] == '_'))
        return false;
    for (size_t i = 0; i < n; i++)
        if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= '0' && s[i] <= '9') || s[i] == '_' ||
              s[i] == '-'))
            return false;
    return true;
}

static void hex(const uint8_t *in, size_t n, char *out)
{
    for (size_t i = 0; i < n; i++)
        snprintf(out + 2 * i, 3, "%02x", in[i]);
}

/* Replace (or add) name's line in path by `line` (0: remove it). */
int user_file_set(const char *path, const char *user, const char *line, unsigned mode)
{
    long n = load(path);
    if (n < 0 && errno != ENOENT)
        return -1;
    if (n < 0)
        file[0] = 0;
    static char out[sizeof(file) + LINE_LENGTH];
    size_t used = 0, len = strlen(user);
    for (char *p = file; *p;) {
        char *next = strchr(p, '\n');
        size_t length = next ? (size_t)(next - p) : strlen(p);
        if (!(length > len && p[len] == ':' && strncmp(p, user, len) == 0)) {
            memcpy(out + used, p, length);
            used += length;
            out[used++] = '\n';
        }
        p += length + (next ? 1 : 0);
    }
    if (line) {
        size_t l = strlen(line);
        if (used + l + 1 > sizeof(out)) {
            errno = ENOSPC;
            return -1;
        }
        memcpy(out + used, line, l);
        used += l;
        out[used++] = '\n';
    }
    struct stat st;
    if (stat(CONFIG_DIR, &st) < 0)
        mkdir(CONFIG_DIR, 0755);                    /* a disk from before step 32 */
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return -1;
    bool ok = write(fd, out, used) == (ssize_t)used;
    if (close(fd) < 0 || !ok)
        return -1;
    return chmod(path, mode);
}

int password_set(const char *user, const char *password)
{
    uint8_t salt[PASSWORD_SALT], hash[SHA256_SIZE];
    if (ritteros_random(salt, sizeof(salt)) < 0)
        return -1;
    pbkdf2_sha256(password, strlen(password), salt, sizeof(salt), PASSWORD_ITERATIONS, hash,
                  sizeof(hash));
    char line[LINE_LENGTH], salt_hex[2 * PASSWORD_SALT + 1], hash_hex[2 * SHA256_SIZE + 1];
    hex(salt, sizeof(salt), salt_hex);
    hex(hash, sizeof(hash), hash_hex);
    snprintf(line, sizeof(line), "%s:%u:%s:%s", user, (unsigned)PASSWORD_ITERATIONS, salt_hex,
             hash_hex);
    memset(hash, 0, sizeof(hash));
    return user_file_set(PASSWORDS_FILE, user, line, 0600);
}

static int unhex(const char *s, uint8_t *out, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        int v = 0;
        for (int k = 0; k < 2; k++) {
            char c = s[2 * i + k];
            int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
            if (d < 0)
                return -1;
            v = v * 16 + d;
        }
        out[i] = (uint8_t)v;
    }
    return 0;
}

bool password_exists(const char *user)
{
    if (load(PASSWORDS_FILE) < 0)
        return false;
    size_t len = strlen(user);
    for (const char *p = file; *p;) {
        if (strncmp(p, user, len) == 0 && p[len] == ':')
            return true;
        const char *next = strchr(p, '\n');
        p = next ? next + 1 : p + strlen(p);
    }
    return false;
}

int password_check(const char *user, const char *password)
{
    if (load(PASSWORDS_FILE) < 0)
        return -1;
    size_t len = strlen(user);
    for (char *p = file; *p;) {
        char *next = strchr(p, '\n');
        if (next)
            *next++ = 0;
        else
            next = p + strlen(p);
        if (strncmp(p, user, len) == 0 && p[len] == ':') {
            const char *q = p + len + 1;
            unsigned long iterations = number(q, &q);
            uint8_t salt[PASSWORD_SALT], want[SHA256_SIZE], got[SHA256_SIZE];
            if (*q != ':' || unhex(q + 1, salt, sizeof(salt)) ||
                q[1 + 2 * PASSWORD_SALT] != ':' ||
                unhex(q + 2 + 2 * PASSWORD_SALT, want, sizeof(want)) || !iterations)
                return 0;                           /* malformed: no login */
            pbkdf2_sha256(password, strlen(password), salt, sizeof(salt),
                          (uint32_t)iterations, got, sizeof(got));
            uint8_t diff = 0;                       /* compare all bytes, always */
            for (int i = 0; i < SHA256_SIZE; i++)
                diff |= (uint8_t)(got[i] ^ want[i]);
            return diff == 0;
        }
        p = next;
    }
    /* No password (or no such user): no login, but only after as long as a
     * real check takes, so the answer's time does not tell which names
     * exist. */
    uint8_t salt[PASSWORD_SALT] = {0}, hash[SHA256_SIZE];
    pbkdf2_sha256(password, strlen(password), salt, sizeof(salt), PASSWORD_ITERATIONS, hash,
                  sizeof(hash));
    return 0;
}

int ritteros_session_stamp(int op)
{
    int64_t r = syscall6(SYS_SESSION_STAMP, op, 0, 0, 0, 0, 0);
    if (r < 0) {
        errno = (int)-r;
        return -1;
    }
    return (int)r;
}
