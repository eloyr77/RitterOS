/*
 * tar c|x|t [v] [-f ARCHIVE] [-C DIR] [FILE...] (63h): archives of files
 * and folders, as on Unix. c makes one of FILEs (folders with everything
 * in them), x takes one apart (all of it, or the FILEs named), t lists it;
 * v also names each file (tv: as ls -l does). The archive is ARCHIVE, or
 * the standard input or output (also as -f -), so a folder goes to another
 * machine in one ssh call: tar cf - DIR | ssh HOST 'tar xf - -C /home/root'.
 * -C DIR works in DIR. The options may come as one word too, without the
 * dash ("cvf ARCHIVE").
 *
 * The format is POSIX's ustar: 512-byte blocks, a header block for each
 * entry (its name split into prefix and name when longer than 100 bytes,
 * the rest in a pax extended header), the content padded to whole blocks,
 * two zero blocks at the end, all in records of 20 blocks. Read are also
 * the pax headers other systems write (x: this entry's path, linkpath and
 * size; g: for all, skipped) and GNU's long names (L, K). Kept: files,
 * folders, symbolic links (hard links as files of their own), their
 * permission bits (and setuid when root extracts), owners' numbers and
 * names, times. Extracted files belong to who extracts them, and get their
 * times from the archive (63m; m: the time of extraction instead, as
 * POSIX's -m), folders' last, as filling them changed theirs. Names
 * lose a leading '/', and an entry with a ".." part, or one whose folders
 * on the way include a symbolic link (one the archive just made, say, to
 * /etc), is refused, so an archive cannot write outside the folder it is
 * taken apart in (as bsdtar).
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "ritteros.h"

PROGRAM_HELP("tar c|x|t [vmpo] [-f ARCHIVE] [-C DIR] [FILE...]",
             "make (c), take apart (x) or list (t) archives of files and folders");

#define BLOCK   512
#define RECORD  (20 * BLOCK)
#define PATH_MAX_TAR 4096

static bool verbose;
static bool keep_now;                   /* m: extracted files keep the time now (63m) */
static int fd = -1;                     /* the archive */
static FILE *talk;                      /* where -v lists (stderr when the archive is stdout) */
static int status;                      /* 1 after a problem with a file */

static void problem(const char *what, const char *why)
{
    fprintf(stderr, "tar: %s: %s\n", what, why);
    status = 1;
}

/* --- the archive's bytes, in records ------------------------------------- */

static unsigned char record[RECORD];
static size_t at, have;                 /* reading: the next byte, the bytes there */
static bool written;                    /* (writing: a record began) */

static bool read_block(unsigned char *b)
{
    while (have - at < BLOCK) {             /* (a pipe gives what it has: collect a block) */
        if (at)
            memmove(record, record + at, have - at), have -= at, at = 0;
        ssize_t n = read(fd, record + have, sizeof(record) - have);
        if (n <= 0)
            return false;
        have += (size_t)n;
    }
    memcpy(b, record + at, BLOCK);
    at += BLOCK;
    return true;
}

static void write_all(const unsigned char *b, size_t n)
{
    while (n) {
        ssize_t w = write(fd, b, n);
        if (w <= 0) {
            fprintf(stderr, "tar: cannot write the archive: %s\n", strerror(errno));
            exit(1);
        }
        b += w, n -= (size_t)w;
    }
}

static void write_block(const unsigned char *b)
{
    memcpy(record + at, b, BLOCK);
    at += BLOCK;
    written = true;
    if (at == RECORD) {
        write_all(record, RECORD);
        at = 0;
    }
}

/* --- headers -------------------------------------------------------------- */

static void octal(unsigned char *field, int width, uint64_t v)
{
    char text[32];
    snprintf(text, sizeof(text), "%0*llo", width - 1, (unsigned long long)v);
    memcpy(field, text, (size_t)width);             /* (width - 1 digits and a NUL) */
}

static uint64_t parse_octal(const unsigned char *field, int width)
{
    if (field[0] & 0x80) {                          /* (base 256, as GNU writes big ones) */
        uint64_t v = field[0] & 0x3F;
        for (int i = 1; i < width; i++)
            v = v << 8 | field[i];
        return v;
    }
    uint64_t v = 0;
    int i = 0;
    while (i < width && (field[i] == ' ' || field[i] == 0))
        i++;
    for (; i < width && field[i] >= '0' && field[i] <= '7'; i++)
        v = v * 8 + (uint64_t)(field[i] - '0');
    return v;
}

static unsigned checksum(const unsigned char *h)
{
    unsigned sum = 0;
    for (int i = 0; i < BLOCK; i++)
        sum += i >= 148 && i < 156 ? ' ' : h[i];    /* (its own field counted as spaces) */
    return sum;
}

struct entry {
    char path[PATH_MAX_TAR], link[PATH_MAX_TAR];
    char type;                          /* '0' file, '5' folder, '2' symbolic link */
    unsigned mode, uid, gid;
    uint64_t size, mtime;
    char uname[33], gname[33];
};

static void put_header(const struct entry *e, const char *name, const char *prefix,
                       const char *link, char type, uint64_t size)
{
    unsigned char h[BLOCK] = { 0 };
    memcpy(h, name, strnlen(name, 100));
    octal(h + 100, 8, e->mode & 07777);
    octal(h + 108, 8, e->uid);
    octal(h + 116, 8, e->gid);
    octal(h + 124, 12, size);
    octal(h + 136, 12, e->mtime);
    h[156] = (unsigned char)type;
    memcpy(h + 157, link, strnlen(link, 100));
    memcpy(h + 257, "ustar", 6);                    /* magic "ustar\0", version "00" */
    memcpy(h + 263, "00", 2);
    memcpy(h + 265, e->uname, strnlen(e->uname, 31));
    memcpy(h + 297, e->gname, strnlen(e->gname, 31));
    memcpy(h + 345, prefix, strnlen(prefix, 155));
    char sum[8];
    snprintf(sum, sizeof(sum), "%06o", checksum(h));
    memcpy(h + 148, sum, 7);                        /* (six digits, NUL, then the space) */
    h[155] = ' ';
    write_block(h);
}

/* A pax record "LEN key=value\n" (LEN counts itself). */
static size_t pax_record(char *out, size_t room, const char *key, const char *value)
{
    size_t body = strlen(key) + strlen(value) + 3, len = body + 1;
    while (snprintf(0, 0, "%zu", len) + body != len)
        len++;
    size_t n = (size_t)snprintf(out, room, "%zu %s=%s\n", len, key, value);
    return n < room ? n : 0;
}

static void write_entry_header(const struct entry *e)
{
    const char *path = e->path;
    size_t n = strlen(path);
    char name[101] = "", prefix[156] = "";
    bool fits = n <= 100;
    if (fits) {
        memcpy(name, path, n);
    } else {                                        /* prefix/name at a '/' */
        for (size_t i = n > 101 ? n - 101 : 0; i < n && i <= 155; i++)
            if (path[i] == '/' && n - i - 1 <= 100 && n - i - 1 > 0) {
                memcpy(prefix, path, i);
                memcpy(name, path + i + 1, n - i - 1);
                fits = true;
                break;
            }
    }
    bool link_fits = strlen(e->link) <= 100;
    if (!fits || !link_fits) {                      /* a pax header says what ustar cannot */
        char data[2 * PATH_MAX_TAR + 64];
        size_t d = 0;
        if (!fits)
            d += pax_record(data + d, sizeof(data) - d, "path", path);
        if (!link_fits)
            d += pax_record(data + d, sizeof(data) - d, "linkpath", e->link);
        struct entry x = *e;
        x.mode = 0644;
        char xname[101];
        snprintf(xname, sizeof(xname), "PaxHeaders/%.80s", strrchr(path, '/') ? strrchr(path, '/') + 1 : path);
        put_header(&x, xname, "", "", 'x', d);
        for (size_t o = 0; o < d; o += BLOCK) {
            unsigned char b[BLOCK] = { 0 };
            memcpy(b, data + o, d - o < BLOCK ? d - o : BLOCK);
            write_block(b);
        }
        if (!fits)
            snprintf(name, sizeof(name), "%.100s", path + (n > 100 ? n - 100 : 0));
    }
    put_header(e, name, prefix, link_fits ? e->link : "", e->type, e->type == '0' ? e->size : 0);
}

/* --- c: making an archive -------------------------------------------------- */

static void owner_names(struct entry *e)
{
    struct passwd *pw = getpwuid(e->uid);
    snprintf(e->uname, sizeof(e->uname), "%s", pw ? pw->pw_name : e->uid == 0 ? "root" : "");
    snprintf(e->gname, sizeof(e->gname), "%s", e->gid == 0 ? "root" : e->uname);
}

static void listed(const struct entry *e);

static void add(const char *path)
{
    struct stat st;
    if (lstat(path, &st) < 0) {
        problem(path, strerror(errno));
        return;
    }
    struct entry e;
    memset(&e, 0, sizeof(e));
    const char *name = path;
    while (name[0] == '/' && name[1])               /* (a name without its leading '/') */
        name++;
    snprintf(e.path, sizeof(e.path), "%s", name[0] == '/' ? "." : name);
    e.mode = st.st_mode & 07777;
    e.uid = st.st_uid, e.gid = st.st_gid;
    e.mtime = st.st_mtime > 0 ? (uint64_t)st.st_mtime : 0;
    owner_names(&e);
    if (S_ISLNK(st.st_mode)) {
        ssize_t n = readlink(path, e.link, sizeof(e.link) - 1);
        if (n < 0) {
            problem(path, strerror(errno));
            return;
        }
        e.link[n] = 0;
        e.type = '2';
        write_entry_header(&e);
        listed(&e);
        return;
    }
    if (S_ISDIR(st.st_mode)) {
        e.type = '5';
        size_t n = strlen(e.path);
        if (n + 2 < sizeof(e.path) && (n == 0 || e.path[n - 1] != '/'))
            strcat(e.path, "/");                    /* (a folder's name ends in '/') */
        write_entry_header(&e);
        listed(&e);
        /* Its names first, the folder closed before going into them (a
         * program may have only a few folders open at once). */
        DIR *d = opendir(path);
        if (!d) {
            problem(path, strerror(errno));
            return;
        }
        char **children = 0;
        size_t count = 0, room = 0;
        struct dirent *de;
        while ((de = readdir(d))) {
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
                continue;
            if (count == room) {
                room = room ? 2 * room : 16;
                char **grown = realloc(children, room * sizeof(*children));
                if (!grown)
                    break;
                children = grown;
            }
            char child[PATH_MAX_TAR];
            if (snprintf(child, sizeof(child), "%s%s%s", path, path[strlen(path) - 1] == '/' ? "" : "/",
                         de->d_name) >= (int)sizeof(child)) {
                problem(de->d_name, "the path is too long");
                continue;
            }
            children[count] = strdup(child);
            if (children[count])
                count++;
        }
        closedir(d);
        for (size_t k = 0; k < count; k++) {
            add(children[k]);
            free(children[k]);
        }
        free(children);
        return;
    }
    if (!S_ISREG(st.st_mode)) {
        problem(path, "not a file, folder or link: left out");
        return;
    }
    int in = open(path, O_RDONLY);
    if (in < 0) {
        problem(path, strerror(errno));
        return;
    }
    e.type = '0';
    e.size = (uint64_t)st.st_size;
    write_entry_header(&e);
    uint64_t left = e.size;
    unsigned char b[BLOCK];
    while (left) {                              /* (exactly the size the header says) */
        memset(b, 0, sizeof(b));
        size_t want = left < BLOCK ? (size_t)left : BLOCK, got = 0;
        while (got < want) {
            ssize_t n = read(in, b + got, want - got);
            if (n <= 0)
                break;
            got += (size_t)n;
        }
        if (got < want)
            problem(path, "it became shorter while being read (the rest is zeros)");
        write_block(b);
        left -= want;
    }
    close(in);
    listed(&e);
}

static void finish_archive(void)
{
    unsigned char zero[BLOCK] = { 0 };
    write_block(zero);
    write_block(zero);
    if (at) {                                   /* (the last record filled with zeros) */
        memset(record + at, 0, RECORD - at);
        write_all(record, RECORD);
        at = 0;
    }
}

/* --- reading entries ------------------------------------------------------- */

/* The next entry's header (pax and GNU name records taken in); false at
 * the end. */
static bool next_entry(struct entry *e)
{
    char long_path[PATH_MAX_TAR] = "", long_link[PATH_MAX_TAR] = "";
    int64_t pax_size = -1;
    for (;;) {
        unsigned char h[BLOCK];
        if (!read_block(h))
            return false;
        bool zero = true;
        for (int i = 0; i < BLOCK && zero; i++)
            zero = h[i] == 0;
        if (zero)
            return false;                       /* (the end: the second zero block is not needed) */
        if ((unsigned)parse_octal(h + 148, 8) != checksum(h)) {
            fprintf(stderr, "tar: not a tar archive (or a damaged one): a header's checksum is wrong\n");
            exit(1);
        }
        memset(e, 0, sizeof(*e));
        e->type = h[156] ? (char)h[156] : '0';
        e->size = parse_octal(h + 124, 12);
        e->mode = (unsigned)parse_octal(h + 100, 8);
        e->uid = (unsigned)parse_octal(h + 108, 8);
        e->gid = (unsigned)parse_octal(h + 116, 8);
        e->mtime = parse_octal(h + 136, 12);
        memcpy(e->uname, h + 265, 32);
        memcpy(e->gname, h + 297, 32);
        if (e->type == 'x' || e->type == 'g' || e->type == 'L' || e->type == 'K') {
            /* (the data: pax records, or GNU's long name or link) */
            if (e->size > 1024 * 1024) {
                fprintf(stderr, "tar: an extended header of %llu bytes: too long\n",
                        (unsigned long long)e->size);
                exit(1);
            }
            char *data = calloc(1, (size_t)e->size + BLOCK + 1);
            if (!data)
                exit(1);
            for (uint64_t o = 0; o < e->size; o += BLOCK)
                if (!read_block((unsigned char *)data + o)) {
                    fprintf(stderr, "tar: the archive ends inside an entry\n");
                    exit(1);
                }
            data[e->size] = 0;
            if (e->type == 'L')
                snprintf(long_path, sizeof(long_path), "%s", data);
            else if (e->type == 'K')
                snprintf(long_link, sizeof(long_link), "%s", data);
            else if (e->type == 'x')
                for (char *p = data; p < data + e->size;) {     /* "LEN key=value\n" */
                    char *space = strchr(p, ' ');
                    long len = atol(p);
                    if (!space || len <= 0 || p + len > data + e->size)
                        break;
                    char *key = space + 1, *eq = memchr(key, '=', (size_t)(p + len - key));
                    if (eq) {
                        *eq = 0;
                        char *value = eq + 1;
                        p[len - 1] = 0;                         /* (its '\n') */
                        if (!strcmp(key, "path"))
                            snprintf(long_path, sizeof(long_path), "%s", value);
                        else if (!strcmp(key, "linkpath"))
                            snprintf(long_link, sizeof(long_link), "%s", value);
                        else if (!strcmp(key, "size"))
                            pax_size = atoll(value);
                    }
                    p += len;
                }
            free(data);
            continue;                           /* (then the entry they are about) */
        }
        char name[101] = "", prefix[156] = "", link[101] = "";
        memcpy(name, h, 100);
        memcpy(link, h + 157, 100);
        if (!memcmp(h + 257, "ustar", 5))
            memcpy(prefix, h + 345, 155);
        if (long_path[0])
            snprintf(e->path, sizeof(e->path), "%s", long_path);
        else if (prefix[0])
            snprintf(e->path, sizeof(e->path), "%s/%s", prefix, name);
        else
            snprintf(e->path, sizeof(e->path), "%s", name);
        snprintf(e->link, sizeof(e->link), "%s", long_link[0] ? long_link : link);
        if (pax_size >= 0)
            e->size = (uint64_t)pax_size;
        if (e->type == '7')
            e->type = '0';                      /* (a "contiguous" file: a file) */
        if (e->type == '0' && e->path[0] && e->path[strlen(e->path) - 1] == '/')
            e->type = '5';                      /* (old archives: a folder by its '/') */
        return true;
    }
}

/* Skip (or with out >= 0, write there) an entry's content. */
static bool content(const struct entry *e, int out, const char *what)
{
    /* Links, folders and devices have no content, whatever the size says. */
    uint64_t left = e->type == '0' ? e->size : 0;
    unsigned char b[BLOCK];
    bool ok = true;
    while (left) {
        if (!read_block(b)) {
            fprintf(stderr, "tar: the archive ends inside %s\n", what);
            exit(1);
        }
        size_t n = left < BLOCK ? (size_t)left : BLOCK;
        if (out >= 0 && ok) {
            const unsigned char *p = b;
            size_t m = n;
            while (m) {
                ssize_t w = write(out, p, m);
                if (w <= 0) {
                    problem(what, strerror(errno));
                    ok = false;
                    break;
                }
                p += w, m -= (size_t)w;
            }
        }
        left -= n;
    }
    return ok;
}

/* --- t and v: what is in it ------------------------------------------------ */

static void listed(const struct entry *e)
{
    if (!verbose)
        return;
    fprintf(talk, "%s\n", e->path);
}

static void long_line(const struct entry *e)
{
    char m[11] = "----------";
    m[0] = e->type == '5' ? 'd' : e->type == '2' ? 'l' : '-';
    const char *rwx = "rwxrwxrwx";
    for (int i = 0; i < 9; i++)
        if (e->mode & (0400u >> i))
            m[1 + i] = rwx[i];
    if (e->mode & 04000)
        m[3] = m[3] == 'x' ? 's' : 'S';
    time_t t = (time_t)e->mtime;
    struct tm *tm = gmtime(&t);
    char when[32] = "";
    if (tm)
        strftime(when, sizeof(when), "%Y-%m-%d %H:%M", tm);
    char who[80];
    snprintf(who, sizeof(who), "%s/%s", e->uname[0] ? e->uname : "?", e->gname[0] ? e->gname : "?");
    if (!e->uname[0])
        snprintf(who, sizeof(who), "%u/%u", e->uid, e->gid);
    printf("%s %-12s %10llu %s %s%s%s\n", m, who, (unsigned long long)(e->type == '0' ? e->size : 0),
           when, e->path, e->type == '2' ? " -> " : "", e->type == '2' ? e->link : "");
}

/* --- x: taking it apart ---------------------------------------------------- */

static bool wanted(const char *path, char **names, int count)
{
    if (!count)
        return true;
    for (int i = 0; i < count; i++) {
        size_t n = strlen(names[i]);
        while (n > 1 && names[i][n - 1] == '/')
            n--;
        if (!strncmp(path, names[i], n) && (path[n] == 0 || path[n] == '/'))
            return true;                        /* (that name, or something inside it) */
    }
    return false;
}

/* The path to write, or 0 if it would leave the folder (a ".." part). */
static const char *safe_path(char *path)
{
    while (path[0] == '/')
        path++;                                 /* (never from the root: here) */
    for (const char *p = path; *p;) {
        size_t n = strcspn(p, "/");
        if (n == 2 && p[0] == '.' && p[1] == '.')
            return 0;
        p += n;
        while (*p == '/')
            p++;
    }
    return path[0] ? path : 0;
}

/* Whether a folder on the way to path is a symbolic link (where writing
 * would follow it out of here). */
static bool through_link(const char *path)
{
    char p[PATH_MAX_TAR];
    snprintf(p, sizeof(p), "%s", path);
    for (char *s = strchr(p + 1, '/'); s; s = strchr(s + 1, '/')) {
        *s = 0;
        struct stat st;
        bool link = lstat(p, &st) == 0 && S_ISLNK(st.st_mode);
        *s = '/';
        if (link)
            return true;
    }
    return false;
}

/* Make the folders above path (as mkdir -p). */
static void parents(const char *path)
{
    char p[PATH_MAX_TAR];
    snprintf(p, sizeof(p), "%s", path);
    for (char *s = strchr(p + 1, '/'); s; s = strchr(s + 1, '/')) {
        *s = 0;
        mkdir(p, 0755);                         /* (there already: that is fine) */
        *s = '/';
    }
}

/* (63m) The time the archive gives a file (a link itself, not what it
 * names). Folders get theirs at the end (later_times): filling them
 * changes it. */
static void set_time(const char *path, const struct entry *e)
{
    if (keep_now)
        return;
    struct timespec when[2] = { { (time_t)e->mtime, 0 }, { (time_t)e->mtime, 0 } };
    if (utimensat(AT_FDCWD, path, when, AT_SYMLINK_NOFOLLOW) < 0 && errno != EPERM)
        problem(path, strerror(errno));
}

static struct folder_time { char *path; uint64_t mtime; } *folder_times;
static size_t folder_count, folder_room;

static void later_time(const char *path, uint64_t mtime)
{
    if (keep_now)
        return;
    if (folder_count == folder_room) {
        folder_room = folder_room ? 2 * folder_room : 32;
        struct folder_time *grown = realloc(folder_times, folder_room * sizeof(*grown));
        if (!grown)
            return;
        folder_times = grown;
    }
    folder_times[folder_count].path = strdup(path);
    if (folder_times[folder_count].path)
        folder_times[folder_count++].mtime = mtime;
}

static void folder_times_now(void)
{
    for (size_t i = folder_count; i-- > 0;) {   /* (inner ones first: they come later) */
        struct entry e;
        e.mtime = folder_times[i].mtime;
        set_time(folder_times[i].path, &e);
        free(folder_times[i].path);
    }
    folder_count = 0;
}

static void extract(struct entry *e)
{
    const char *path = safe_path(e->path);
    if (!path) {
        if (strcmp(e->path, "./") && strcmp(e->path, "."))
            problem(e->path, "a path that would leave this folder: left out");
        content(e, -1, e->path);
        return;
    }
    unsigned mode = e->mode & (getuid() == 0 ? 04777 : 0777);
    if (through_link(path)) {
        problem(path, "it would go through a symbolic link: left out");
        content(e, -1, path);
        return;
    }
    parents(path);
    if (e->type == '5') {
        char dir[PATH_MAX_TAR];
        snprintf(dir, sizeof(dir), "%s", path);
        size_t n = strlen(dir);
        while (n > 1 && dir[n - 1] == '/')
            dir[--n] = 0;
        struct stat st;
        if (mkdir(dir, mode ? mode : 0755) < 0 && !(stat(dir, &st) == 0 && S_ISDIR(st.st_mode)))
            problem(dir, strerror(errno));
        else {
            chmod(dir, mode ? mode : 0755);
            later_time(dir, e->mtime);
        }
        content(e, -1, path);
        listed(e);
        return;
    }
    if (e->type == '2') {
        struct stat st;
        if (lstat(path, &st) == 0 && !S_ISDIR(st.st_mode))
            unlink(path);                       /* (as tar does: the archive's wins) */
        if (symlink(e->link, path) < 0)
            problem(path, strerror(errno));
        else
            set_time(path, e);
        content(e, -1, path);
        listed(e);
        return;
    }
    if (e->type == '1') {                       /* a hard link: a copy of the file it names */
        const char *source = safe_path(e->link);
        int in = source ? open(source, O_RDONLY) : -1, out = -1;
        if (in >= 0)
            out = open(path, O_WRONLY | O_CREAT | O_TRUNC, mode ? mode : 0644);
        if (in < 0 || out < 0)
            problem(path, in < 0 ? "the file it is a link to is not there" : strerror(errno));
        unsigned char b[4096];
        ssize_t n;
        while (in >= 0 && out >= 0 && (n = read(in, b, sizeof(b))) > 0)
            if (write(out, b, (size_t)n) != n) {
                problem(path, strerror(errno));
                break;
            }
        if (in >= 0)
            close(in);
        if (out >= 0 && close(out) == 0)
            set_time(path, e);
        content(e, -1, path);
        listed(e);
        return;
    }
    if (e->type != '0') {
        problem(e->path, "not a file, folder or link (a device?): left out");
        content(e, -1, e->path);
        return;
    }
    struct stat st;
    if (lstat(path, &st) == 0 && S_ISLNK(st.st_mode))
        unlink(path);                           /* (never write through a link) */
    int out = open(path, O_WRONLY | O_CREAT | O_TRUNC, mode ? mode : 0644);
    if (out < 0) {
        problem(path, strerror(errno));
        content(e, -1, path);
        return;
    }
    content(e, out, path);
    if (close(out) < 0)
        problem(path, strerror(errno));
    chmod(path, mode ? mode : 0644);            /* (the umask had its say at open) */
    set_time(path, e);
    listed(e);
}

/* --- the command line ------------------------------------------------------- */

static void usage(void)
{
    fprintf(stderr, "usage: tar c|x|t [vm] [-f ARCHIVE] [-C DIR] [FILE...]\n");
    exit(2);
}

int main(int argc, char **argv)
{
    char what = 0;
    const char *archive = "-", *dir = 0;
    char **names = calloc((size_t)argc + 1, sizeof(char *));
    int count = 0;
    /* The first word may be old-style keys without a dash ("cvf A"); then
     * f takes the next word. */
    int i = 1;
    bool want_f = false;
    if (i < argc && argv[i][0] != '-') {
        for (const char *k = argv[i]; *k; k++) {
            if (*k == 'c' || *k == 'x' || *k == 't') {
                if (what)
                    usage();
                what = *k;
            } else if (*k == 'v')
                verbose = true;
            else if (*k == 'f')
                want_f = true;
            else if (*k == 'm')
                keep_now = true;
            else if (*k == 'p' || *k == 'o')
                ;                               /* (as other tars: permissions are kept anyway) */
            else
                usage();
        }
        i++;
        if (want_f) {
            if (i >= argc)
                usage();
            archive = argv[i++];
        }
    }
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--")) {
            for (i++; i < argc; i++)
                names[count++] = argv[i];
            break;
        }
        if (a[0] != '-' || !a[1]) {
            names[count++] = argv[i];
            continue;
        }
        for (const char *k = a + 1; *k; k++) {
            if (*k == 'c' || *k == 'x' || *k == 't') {
                if (what && what != *k)
                    usage();
                what = *k;
            } else if (*k == 'v') {
                verbose = true;
            } else if (*k == 'm') {
                keep_now = true;
            } else if (*k == 'p' || *k == 'o') {
                ;
            } else if (*k == 'f' || *k == 'C') {
                const char *value = k[1] ? k + 1 : i + 1 < argc ? argv[++i] : 0;
                if (!value)
                    usage();
                if (*k == 'f')
                    archive = value;
                else
                    dir = value;
                break;
            } else {
                usage();
            }
        }
    }
    if (!what)
        usage();
    if (what == 'c' && !count) {
        fprintf(stderr, "tar: c needs the files to put in\n");
        return 2;
    }
    bool to_stdout = what == 'c' && !strcmp(archive, "-");
    talk = to_stdout ? stderr : stdout;
    if (!strcmp(archive, "-"))
        fd = what == 'c' ? STDOUT_FILENO : STDIN_FILENO;
    else
        fd = what == 'c' ? open(archive, O_WRONLY | O_CREAT | O_TRUNC, 0644) : open(archive, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "tar: %s: %s\n", archive, strerror(errno));
        return 1;
    }
    if (dir && chdir(dir) < 0) {                /* (after opening: a relative ARCHIVE is from here) */
        fprintf(stderr, "tar: %s: %s\n", dir, strerror(errno));
        return 1;
    }
    if (what == 'c') {
        for (int k = 0; k < count; k++)
            add(names[k]);
        finish_archive();
    } else {
        struct entry e;
        while (next_entry(&e)) {
            if (!wanted(e.path, names, count)) {
                content(&e, -1, e.path);
                continue;
            }
            if (what == 't') {
                if (verbose)
                    long_line(&e);
                else
                    printf("%s\n", e.path);
                content(&e, -1, e.path);
            } else {
                extract(&e);
            }
        }
        folder_times_now();
        unsigned char rest[BLOCK];              /* (what follows the end: read, so a writer */
        while (read_block(rest))                /*  into a pipe is not cut off) */
            ;
    }
    if (fd > STDERR_FILENO && close(fd) < 0) {
        fprintf(stderr, "tar: %s: %s\n", archive, strerror(errno));
        return 1;
    }
    return status;
}
