/*
 * ar (63): archives of object files, as the build makes its static
 * libraries (libritteros.a and the like) and ld searches them. The GNU
 * format, written as llvm-ar writes it by default (deterministic: dates,
 * owners 0, mode 644): "!<arch>", a "/" symbol index (big-endian count,
 * the members' offsets, their defined global symbols' names), a "//" table
 * for names of 16 characters or more, then the members, each padded to an
 * even size.
 *
 * ar rcs ARCHIVE FILE...   add or replace FILEs (r), quietly creating the
 *                          archive (c), with the symbol index (s: always)
 * ar t ARCHIVE             list the members
 * ar x ARCHIVE [MEMBER...] extract members (all of them by default)
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if __has_include("ritteros.h")
#include "ritteros.h"
PROGRAM_HELP("ar rcs ARCHIVE FILE... | ar t ARCHIVE | ar x ARCHIVE [MEMBER...]",
             "make and read archives of object files (static libraries)");
#endif

struct member {
    char *name;                                 /* the file's last path part */
    unsigned char *data;
    size_t size;
};

static struct member *members;
static int nmembers;

static _Noreturn void fail(const char *fmt, const char *what)
{
    fprintf(stderr, "ar: ");
    fprintf(stderr, fmt, what);
    fprintf(stderr, "\n");
    exit(1);
}

static unsigned char *read_whole(const char *path, size_t *size, int must)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        if (must)
            fail("cannot read %s", path);
        return 0;
    }
    size_t cap = 1 << 16, len = 0;
    unsigned char *d = malloc(cap);
    for (;;) {
        if (len == cap)
            d = realloc(d, cap *= 2);
        if (!d)
            fail("out of memory reading %s", path);
        size_t n = fread(d + len, 1, cap - len, f);
        if (!n)
            break;
        len += n;
    }
    fclose(f);
    *size = len;
    return d;
}

static const char *base_name(const char *path)
{
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

static uint64_t get(const unsigned char *p, int n)
{
    uint64_t v = 0;
    for (int i = n - 1; i >= 0; i--)
        v = v << 8 | p[i];
    return v;
}

/* the members of an existing archive */
static void read_archive(const char *path)
{
    size_t size;
    unsigned char *d = read_whole(path, &size, 0);
    if (!d)
        return;
    if (size < 8 || memcmp(d, "!<arch>\n", 8) != 0)
        fail("%s: not an archive", path);
    const char *long_names = 0;
    size_t long_size = 0, at = 8;
    while (at + 60 <= size) {
        const unsigned char *h = d + at;
        char field[11];
        memcpy(field, h + 48, 10);
        field[10] = 0;
        size_t msize = (size_t)strtoull(field, 0, 10);
        if (at + 60 + msize > size)
            fail("%s: a member past the archive's end", path);
        unsigned char *data = d + at + 60;
        char name[256];
        name[0] = 0;
        if (h[0] == '/' && (h[1] == ' ' || memcmp(h, "/SYM64/", 7) == 0)) {
            /* the symbol index: made again */
        } else if (h[0] == '/' && h[1] == '/') {
            long_names = (const char *)data;
            long_size = msize;
        } else {
            size_t k = 0;
            if (h[0] == '/') {
                size_t off = (size_t)strtoull((const char *)h + 1, 0, 10);
                while (off + k < long_size && long_names[off + k] != '/' && long_names[off + k] != '\n' && k < 255) {
                    name[k] = long_names[off + k];
                    k++;
                }
            } else
                while (k < 16 && h[k] != '/' && h[k] != ' ') {
                    name[k] = (char)h[k];
                    k++;
                }
            name[k] = 0;
            members = realloc(members, sizeof(*members) * (size_t)(nmembers + 1));
            members[nmembers].name = strdup(name);
            members[nmembers].data = data;
            members[nmembers].size = msize;
            nmembers++;
        }
        at += 60 + msize;
        at += at & 1;
    }
}

/* --- the symbol index ------------------------------------------------------------ */

struct symbols {
    char *names;
    size_t len, cap;
    uint32_t *owner;                            /* each symbol's member */
    int count, room;
};

/* the defined global (and weak, and common) symbols of an ELF64 object */
static void object_symbols(int m, struct symbols *s)
{
    const unsigned char *d = members[m].data;
    size_t size = members[m].size;
    if (size < 64 || memcmp(d, "\177ELF", 4) != 0 || d[4] != 2)
        return;                                 /* (not a 64-bit ELF object: no symbols) */
    uint64_t shoff = get(d + 40, 8);
    int shnum = (int)get(d + 60, 2);
    if (shoff + 64 * (uint64_t)shnum > size)
        return;
    for (int i = 0; i < shnum; i++) {
        const unsigned char *h = d + shoff + 64 * (uint64_t)i;
        if (get(h + 4, 4) != 2)                 /* SHT_SYMTAB */
            continue;
        uint64_t off = get(h + 24, 8), sz = get(h + 32, 8);
        int link = (int)get(h + 40, 4);
        if (link >= shnum || off + sz > size)
            return;
        const unsigned char *sh = d + shoff + 64 * (uint64_t)link;
        uint64_t stroff = get(sh + 24, 8), strsz = get(sh + 32, 8);
        if (stroff + strsz > size)
            return;
        for (uint64_t k = 1; k < sz / 24; k++) {
            const unsigned char *e = d + off + 24 * k;
            int bind = e[4] >> 4, type = e[4] & 15;
            int shndx = (int)get(e + 6, 2);
            if ((bind != 1 && bind != 2 && bind != 10) || type == 3 || type == 4)
                continue;                       /* (locals, sections, files) */
            if (shndx == 0)
                continue;                       /* (undefined) */
            uint64_t name = get(e, 4);
            if (name >= strsz)
                continue;
            const char *n = (const char *)d + stroff + name;
            size_t len = strnlen(n, (size_t)(strsz - name));
            if (!len)
                continue;
            if (s->len + len + 1 > s->cap) {
                s->cap = (s->len + len + 1) * 2;
                s->names = realloc(s->names, s->cap);
            }
            memcpy(s->names + s->len, n, len);
            s->len += len;
            s->names[s->len++] = 0;
            if (s->count == s->room) {
                s->room = s->room ? 2 * s->room : 256;
                s->owner = realloc(s->owner, sizeof(uint32_t) * (size_t)s->room);
            }
            s->owner[s->count++] = (uint32_t)m;
        }
        return;
    }
}

static void put_header(FILE *f, const char *name, size_t size, int plain)
{
    char h[61];
    if (plain)                                  /* (the index and the long names' table) */
        snprintf(h, sizeof(h), "%-16s%-12s%-6s%-6s%-8s%-10zu`\n", name, plain == 1 ? "0" : "",
                 plain == 1 ? "0" : "", plain == 1 ? "0" : "", plain == 1 ? "0" : "", size);
    else
        snprintf(h, sizeof(h), "%-16s%-12s%-6s%-6s%-8s%-10zu`\n", name, "0", "0", "0", "644", size);
    fwrite(h, 1, 60, f);
}

static void put32be(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)(v >> 24);
    p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);
    p[3] = (unsigned char)v;
}

static void write_archive(const char *path)
{
    /* the long names' table and each member's name field */
    char *table = 0;
    size_t tlen = 0;
    char (*fields)[17] = calloc((size_t)nmembers + 1, sizeof(*fields));
    for (int m = 0; m < nmembers; m++) {
        size_t n = strlen(members[m].name);
        if (n < 16)
            snprintf(fields[m], 17, "%s/", members[m].name);
        else {
            snprintf(fields[m], 17, "/%zu", tlen);
            table = realloc(table, tlen + n + 3);
            memcpy(table + tlen, members[m].name, n);
            tlen += n;
            table[tlen++] = '/';
            table[tlen++] = '\n';
        }
    }
    if (tlen & 1) {
        table = realloc(table, tlen + 1);
        table[tlen++] = '\n';
    }
    /* the symbols, and so the index's size: then every offset is known */
    struct symbols s = { 0 };
    for (int m = 0; m < nmembers; m++)
        object_symbols(m, &s);
    size_t index_size = 4 + 4 * (size_t)s.count + s.len;
    size_t index_padded = index_size + (index_size & 1);
    size_t at = 8 + 60 + index_padded;
    if (tlen)
        at += 60 + tlen;
    size_t *offsets = malloc(sizeof(size_t) * (size_t)(nmembers + 1));
    for (int m = 0; m < nmembers; m++) {
        offsets[m] = at;
        at += 60 + members[m].size;
        at += at & 1;
    }
    FILE *f = fopen(path, "wb");
    if (!f)
        fail("cannot write %s", path);
    fwrite("!<arch>\n", 1, 8, f);
    put_header(f, "/", index_padded, 1);
    unsigned char *index = calloc(1, index_padded + 1);
    put32be(index, (uint32_t)s.count);
    for (int k = 0; k < s.count; k++)
        put32be(index + 4 + 4 * k, (uint32_t)offsets[s.owner[k]]);
    memcpy(index + 4 + 4 * (size_t)s.count, s.names, s.len);
    fwrite(index, 1, index_padded, f);
    if (tlen) {
        put_header(f, "//", tlen, 2);
        fwrite(table, 1, tlen, f);
    }
    for (int m = 0; m < nmembers; m++) {
        put_header(f, fields[m], members[m].size, 0);
        fwrite(members[m].data, 1, members[m].size, f);
        if (members[m].size & 1)
            fputc('\n', f);
    }
    if (fclose(f) != 0)
        fail("cannot write %s", path);
}

static void usage(void)
{
    fprintf(stderr, "usage: ar rcs ARCHIVE FILE...\n       ar t ARCHIVE\n       ar x ARCHIVE [MEMBER...]\n");
    exit(2);
}

int main(int argc, char **argv)
{
    if (argc < 3)
        usage();
    const char *ops = argv[1][0] == '-' ? argv[1] + 1 : argv[1];
    const char *archive = argv[2];
    int replace = 0, list = 0, extract = 0;
    for (const char *o = ops; *o; o++)
        switch (*o) {
        case 'r': replace = 1; break;
        case 't': list = 1; break;
        case 'x': extract = 1; break;
        case 'c': case 's': case 'D': case 'u': break;   /* (create quietly; the index is always made) */
        default: usage();
        }
    if (replace + list + extract != 1)
        usage();
    read_archive(archive);
    if (list) {
        for (int m = 0; m < nmembers; m++)
            printf("%s\n", members[m].name);
        return 0;
    }
    if (extract) {
        for (int m = 0; m < nmembers; m++) {
            int wanted = argc == 3;
            for (int k = 3; k < argc && !wanted; k++)
                wanted = strcmp(argv[k], members[m].name) == 0;
            if (!wanted)
                continue;
            FILE *f = fopen(members[m].name, "wb");
            if (!f || fwrite(members[m].data, 1, members[m].size, f) != members[m].size || fclose(f) != 0)
                fail("cannot write %s", members[m].name);
        }
        return 0;
    }
    /* r: each file replaces the member of its name, or comes at the end */
    for (int k = 3; k < argc; k++) {
        size_t size;
        unsigned char *data = read_whole(argv[k], &size, 1);
        const char *name = base_name(argv[k]);
        int m = 0;
        while (m < nmembers && strcmp(members[m].name, name) != 0)
            m++;
        if (m == nmembers) {
            members = realloc(members, sizeof(*members) * (size_t)(nmembers + 1));
            members[nmembers].name = strdup(name);
            nmembers++;
        }
        members[m].data = data;
        members[m].size = size;
    }
    write_archive(archive);
    return 0;
}
