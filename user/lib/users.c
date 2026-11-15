/*
 * Users and passwords. /system/config/users (everyone may read it):
 * name:uid:gid:home per line. /system/config/passwords (root only): name,
 * PBKDF2 iterations, salt and hash (hex), name:iterations:salt:hash.
 */
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "ritteros.h"

#define LINE_LENGTH 256

static char file[8192];

/* (43a) One user as the users file gives it. */
struct user_line {
    char name[USER_NAME_MAX + 1], home[128];
    uid_t uid;
    gid_t gid;
};

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

/* The users: root first (a fresh system has no line for it; a line of
 * root's is skipped then), then each line of the file in turn. index is
 * which one (0 root); false past the last. (The file is read again for
 * each: it is small, and a change counts at once.) */
static bool user_at(int index, struct user_line *u)
{
    if (index == 0) {
        snprintf(u->name, sizeof(u->name), "root");
        snprintf(u->home, sizeof(u->home), "/home/root");
        u->uid = u->gid = 0;
        return true;
    }
    if (load(USERS_FILE) < 0)
        return false;
    int k = 0;
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
            if (*p == ':' && !(uid == 0 && (size_t)(colon - line) == 4 && !strncmp(line, "root", 4)) &&
                ++k == index) {
                memcpy(u->name, line, (size_t)(colon - line));
                u->name[colon - line] = 0;
                snprintf(u->home, sizeof(u->home), "%s", p + 1);
                u->uid = (uid_t)uid;
                u->gid = (gid_t)gid;
                return true;
            }
        }
        line = next;
    }
    return false;
}

/* The user of that name (want_name) or uid; false if none. */
static bool find_user(const char *want_name, uid_t want_uid, struct user_line *u)
{
    for (int i = 0; user_at(i, u); i++)
        if (want_name ? strcmp(u->name, want_name) == 0 : u->uid == want_uid)
            return true;
    return false;
}

/* u into pwd, its strings in buf: 0, or ERANGE. */
static int fill_passwd(const struct user_line *u, struct passwd *pwd, char *buf, size_t size)
{
    size_t name = strlen(u->name) + 1, home = strlen(u->home) + 1;
    static const char x[] = "x", shell[] = "/bin/sh";
    if (name * 2 + home + sizeof(x) + sizeof(shell) > size)
        return ERANGE;
    char *p = buf;
    pwd->pw_name = memcpy(p, u->name, name), p += name;
    pwd->pw_gecos = memcpy(p, u->name, name), p += name;
    pwd->pw_dir = memcpy(p, u->home, home), p += home;
    pwd->pw_passwd = memcpy(p, x, sizeof(x)), p += sizeof(x);
    pwd->pw_shell = memcpy(p, shell, sizeof(shell));
    pwd->pw_uid = u->uid;
    pwd->pw_gid = u->gid;
    return 0;
}

static struct passwd shared;
static char shared_text[USER_NAME_MAX * 2 + 128 + 32];

static struct passwd *shared_entry(const struct user_line *u)
{
    fill_passwd(u, &shared, shared_text, sizeof(shared_text));
    return &shared;
}

struct passwd *getpwnam(const char *want)
{
    struct user_line u;
    return find_user(want, 0, &u) ? shared_entry(&u) : 0;
}

struct passwd *getpwuid(uid_t uid)
{
    struct user_line u;
    return find_user(0, uid, &u) ? shared_entry(&u) : 0;
}

static int lookup_r(const char *name, uid_t uid, struct passwd *pwd, char *buf, size_t size,
                    struct passwd **result)
{
    struct user_line u;
    *result = 0;
    if (!find_user(name, uid, &u))
        return 0;
    int r = fill_passwd(&u, pwd, buf, size);
    if (!r)
        *result = pwd;
    return r;
}

int getpwnam_r(const char *name, struct passwd *pwd, char *buf, size_t size, struct passwd **result)
{
    return lookup_r(name, 0, pwd, buf, size, result);
}

int getpwuid_r(uid_t uid, struct passwd *pwd, char *buf, size_t size, struct passwd **result)
{
    return lookup_r(0, uid, pwd, buf, size, result);
}

static int next_user;

struct passwd *getpwent(void)
{
    struct user_line u;
    return user_at(next_user, &u) ? (next_user++, shared_entry(&u)) : 0;
}

void setpwent(void) { next_user = 0; }
void endpwent(void) { next_user = 0; }

/* (43a) The struct passwd of programs built before 43a, and their
 * getpwnam and getpwuid, kept under those names. */
struct passwd_v1 {
    char *pw_name;
    uid_t pw_uid;
    gid_t pw_gid;
    char *pw_dir;
};

static struct passwd_v1 *v1_of(struct passwd *p)
{
    static struct passwd_v1 old;
    if (!p)
        return 0;
    old.pw_name = p->pw_name;
    old.pw_uid = p->pw_uid;
    old.pw_gid = p->pw_gid;
    old.pw_dir = p->pw_dir;
    return &old;
}

struct passwd_v1 *getpwnam_v1(const char *name) __asm__("getpwnam");
struct passwd_v1 *getpwuid_v1(uid_t uid) __asm__("getpwuid");
struct passwd_v1 *getpwnam_v1(const char *name) { return v1_of(getpwnam(name)); }
struct passwd_v1 *getpwuid_v1(uid_t uid) { return v1_of(getpwuid(uid)); }

/* Groups (43a): one per user, named after the user whose uid it is. */
static bool group_at(int index, struct user_line *u)
{
    return user_at(index, u);
}

static int fill_group(const struct user_line *u, struct group *grp, char *buf, size_t size)
{
    size_t name = strlen(u->name) + 1;
    size_t at = (name + 2 + sizeof(char *) - 1) & ~(sizeof(char *) - 1);
    if (at + 2 * sizeof(char *) + name > size)
        return ERANGE;
    grp->gr_name = memcpy(buf, u->name, name);
    grp->gr_passwd = memcpy(buf + name, "x", 2);
    grp->gr_gid = (gid_t)u->uid;
    char **members = (char **)(void *)(buf + at);
    members[0] = memcpy(buf + at + 2 * sizeof(char *), u->name, name);
    members[1] = 0;
    grp->gr_mem = members;
    return 0;
}

static bool find_group(const char *want_name, gid_t want_gid, struct user_line *u)
{
    for (int i = 0; group_at(i, u); i++)
        if (want_name ? strcmp(u->name, want_name) == 0 : u->uid == want_gid)
            return true;
    return false;
}

static struct group shared_group;
static char shared_group_text[2 * USER_NAME_MAX + 64];

static struct group *shared_group_entry(const struct user_line *u)
{
    fill_group(u, &shared_group, shared_group_text, sizeof(shared_group_text));
    return &shared_group;
}

struct group *getgrnam(const char *name)
{
    struct user_line u;
    return find_group(name, 0, &u) ? shared_group_entry(&u) : 0;
}

struct group *getgrgid(gid_t gid)
{
    struct user_line u;
    return find_group(0, gid, &u) ? shared_group_entry(&u) : 0;
}

static int group_r(const char *name, gid_t gid, struct group *grp, char *buf, size_t size,
                   struct group **result)
{
    struct user_line u;
    *result = 0;
    if (!find_group(name, gid, &u))
        return 0;
    int r = fill_group(&u, grp, buf, size);
    if (!r)
        *result = grp;
    return r;
}

int getgrnam_r(const char *name, struct group *grp, char *buf, size_t size, struct group **result)
{
    return group_r(name, 0, grp, buf, size, result);
}

int getgrgid_r(gid_t gid, struct group *grp, char *buf, size_t size, struct group **result)
{
    return group_r(0, gid, grp, buf, size, result);
}

static int next_group;

struct group *getgrent(void)
{
    struct user_line u;
    return group_at(next_group, &u) ? (next_group++, shared_group_entry(&u)) : 0;
}

void setgrent(void) { next_group = 0; }
void endgrent(void) { next_group = 0; }

int getgrouplist(const char *user, gid_t group, gid_t *groups, int *count)
{
    (void)user;
    if (*count < 1) {
        *count = 1;
        return -1;
    }
    groups[0] = group;
    *count = 1;
    return 1;
}

int setgroups(size_t count, const gid_t *groups)
{
    (void)count, (void)groups;
    if (geteuid() != 0) {
        errno = EPERM;
        return -1;
    }
    return 0;
}

int initgroups(const char *user, gid_t group)
{
    return setgroups(1, &group) < 0 ? (void)user, -1 : 0;
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
