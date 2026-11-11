/*
 * ld (60f): RitterOS's linker. It links ELF objects, archives of them and
 * shared libraries into a program for RitterOS: position-independent, with
 * the layout of user/user.ld (built in: code, read-only data, the
 * relocated-then-read-only part and the data each on pages of their own,
 * from 0x400000), the loader /system/<arch>/lib/ld.so, every symbol bound
 * at the start (the PLT entries only jump through their GOT slots) and no
 * copy relocations. Like LLVM's lld, an archive is searched for any symbol
 * still undefined, wherever it stood on the command line.
 *
 * ld [-o FILE] [-L DIR] [-lNAME] [--as-needed] [-dynamic-linker PATH] FILE...
 */
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

/* (built on the Mac too, for the tests: there without RitterOS's header) */
#if __has_include("ritteros.h")
#include "ritteros.h"
PROGRAM_HELP("ld [-o FILE] [OPTION]... FILE...",
             "link programs");
#endif

#define PAGE 4096
#define BASE 0x400000                           /* (programs; libraries and ld.so at 0) */

enum { EM_X86_64 = 62, EM_AARCH64 = 183 };

static int machine;
static const char *output = "a.out";
static const char *interp;
static bool as_needed;
/* (62d) what is made: a dynamic program (PIE, the default), a static
 * program (-static), a shared library (-shared) or a self-relocating
 * static PIE like ld.so (-pie --no-dynamic-linker) */
enum { MODE_PIE, MODE_STATIC, MODE_SHARED, MODE_LDSO };
static int mode = MODE_PIE;
static uint64_t base = BASE;
static const char *soname;
static bool relro = true;
static char **exports;                          /* a version script's global names (0: all) */
static int nexports;
static bool have_version_script;

/* --- small things -------------------------------------------------------- */

static _Noreturn void fail(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "ld: ");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(1);
}

static void *alloc(size_t n)
{
    void *p = calloc(1, n ? n : 1);
    if (!p)
        fail("out of memory");
    return p;
}

static char *copy_string(const char *s)
{
    size_t n = strlen(s);
    char *p = alloc(n + 1);
    memcpy(p, s, n);
    return p;
}

static uint64_t get(const unsigned char *p, int n)
{
    uint64_t v = 0;
    for (int i = 0; i < n; i++)
        v |= (uint64_t)p[i] << (8 * i);
    return v;
}

static void put(unsigned char *p, uint64_t v, int n)
{
    for (int i = 0; i < n; i++)
        p[i] = (unsigned char)(v >> (8 * i));
}

static uint64_t align_up(uint64_t n, uint64_t a)
{
    return a ? (n + a - 1) / a * a : n;
}

static unsigned char *read_file(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    size_t cap = 65536, len = 0;
    unsigned char *buf = alloc(cap);
    size_t n;
    while ((n = fread(buf + len, 1, cap - len, f)) > 0) {
        len += n;
        if (len == cap) {
            cap *= 2;
            buf = realloc(buf, cap);
            if (!buf)
                fail("out of memory");
        }
    }
    fclose(f);
    *size = len;
    return buf;
}

/* --- ELF ----------------------------------------------------------------- */

#define SHT_PROGBITS 1
#define SHT_SYMTAB 2
#define SHT_STRTAB 3
#define SHT_RELA 4
#define SHT_HASH 5
#define SHT_DYNAMIC 6
#define SHT_NOTE 7
#define SHT_NOBITS 8
#define SHT_DYNSYM 11
#define SHT_INIT_ARRAY 14
#define SHT_FINI_ARRAY 15
#define SHT_GROUP 17
#define SHF_WRITE 1
#define SHF_ALLOC 2
#define SHF_EXECINSTR 4
#define SHF_TLS 0x400
#define SHN_UNDEF 0
#define SHN_ABS 0xFFF1
#define SHN_COMMON 0xFFF2
#define STB_LOCAL 0
#define STB_GLOBAL 1
#define STB_WEAK 2
#define STT_NOTYPE 0
#define STT_OBJECT 1
#define STT_FUNC 2
#define STT_SECTION 3
#define STT_TLS 6

struct section {
    struct object *obj;
    char *name;
    int type;
    uint64_t flags;
    uint64_t size, align;
    unsigned char *data;                        /* (0 for NOBITS) */
    unsigned char *rela;                        /* its relocations (RELA entries) */
    uint64_t nrela;
    struct output *out;                         /* where it goes; 0: dropped */
    bool placed;                                /* (62f) taken by a linker script's rule */
    uint64_t offset;                            /* in the output section */
};

struct symbol;

struct object {
    char *name;
    unsigned char *file;
    size_t size;
    struct section *sections;
    int nsections;
    struct symbol **symbols;                    /* by index; locals get their own */
    int nsymbols;
    bool loaded;
};

/* a global symbol, or a local one of an object (its own entry) */
struct symbol {
    struct symbol *next;                        /* (hash chain) */
    char *name;
    struct object *obj;                         /* defining object, or 0 */
    struct section *section;                    /* 0: absolute or undefined */
    uint64_t value;
    uint64_t size;
    int binding, type;
    bool defined, is_absolute, is_common;
    struct shlib *shlib;                        /* defined by a shared library */
    bool referenced;
    int got;                                    /* GOT entry index + 1 */
    int plt;                                    /* PLT entry index + 1 */
    int tls_pair;                               /* (62b) its TLS GOT pair's index + 1 */
    bool hidden;                                /* (62d) STV_HIDDEN or INTERNAL: never exported */
    bool exported;                              /* (62d) in a library's .dynsym as a definition */
    bool synthetic;                             /* (62d) defined by ld (__ehdr_start, _DYNAMIC): value */
    int dynsym;                                 /* index in .dynsym */
    bool lazy_member;                           /* (an archive member defines it) */
    struct member *member;
};

struct shlib {
    char *path, *soname;
    bool needed;
    struct shlib *next;
};

struct member {
    char *archive;
    unsigned char *data;
    size_t size;
    char *name;
    bool loaded;
};

static struct symbol *table[16384];
static struct object **objects;
static int nobjects;
static struct shlib *shlibs, *shlibs_tail;

static unsigned hash_name(const char *s)
{
    unsigned h = 2166136261u;
    for (; *s; s++)
        h = (h ^ (unsigned char)*s) * 16777619u;
    return h & 16383;
}

static struct symbol *global(const char *name)
{
    unsigned h = hash_name(name);
    for (struct symbol *s = table[h]; s; s = s->next)
        if (strcmp(s->name, name) == 0)
            return s;
    struct symbol *s = alloc(sizeof(*s));
    s->name = copy_string(name);
    s->next = table[h];
    table[h] = s;
    return s;
}

static struct symbol *find_global(const char *name)
{
    for (struct symbol *s = table[hash_name(name)]; s; s = s->next)
        if (strcmp(s->name, name) == 0)
            return s;
    return 0;
}

/* --- reading objects ----------------------------------------------------- */

static void check_header(const unsigned char *d, size_t size, const char *name, int want_type)
{
    if (size < 64 || memcmp(d, "\177ELF", 4) != 0 || d[4] != 2 || d[5] != 1)
        fail("%s: not a 64-bit little-endian ELF file", name);
    int type = (int)get(d + 16, 2), mach = (int)get(d + 18, 2);
    if (type != want_type)
        fail("%s: not an %s", name, want_type == 1 ? "object file" : "shared library");
    if (machine && mach != machine)
        fail("%s: built for another processor", name);
    machine = mach;
}

static void define(struct symbol *g, struct object *o, struct section *sec, uint64_t value, uint64_t size, int bind,
                   int type, bool absolute)
{
    if (g->defined && !g->shlib) {
        if (bind == STB_WEAK)
            return;
        if (g->binding != STB_WEAK)
            fail("duplicate symbol: %s (in %s and %s)", g->name, g->obj ? g->obj->name : "?", o->name);
    }
    g->obj = o;
    g->section = sec;
    g->value = value;
    g->size = size;
    g->binding = bind;
    g->type = type;
    g->defined = true;
    g->is_absolute = absolute;
    g->is_common = false;
    g->shlib = 0;
}

static void load_object(const char *name, unsigned char *d, size_t size)
{
    check_header(d, size, name, 1);
    struct object *o = alloc(sizeof(*o));
    o->name = copy_string(name);
    o->file = d;
    o->size = size;
    uint64_t shoff = get(d + 40, 8);
    int shnum = (int)get(d + 60, 2), shstrndx = (int)get(d + 62, 2);
    if (shoff + (uint64_t)shnum * 64 > size)
        fail("%s: section headers out of the file", name);
    o->nsections = shnum;
    o->sections = alloc(sizeof(struct section) * (size_t)shnum);
    const unsigned char *shstr = d + get(d + shoff + 64 * (uint64_t)shstrndx + 24, 8);
    int symtab = -1;
    for (int i = 0; i < shnum; i++) {
        const unsigned char *h = d + shoff + 64 * (uint64_t)i;
        struct section *s = &o->sections[i];
        s->obj = o;
        s->name = (char *)shstr + get(h, 4);
        s->type = (int)get(h + 4, 4);
        s->flags = get(h + 8, 8);
        uint64_t off = get(h + 24, 8);
        s->size = get(h + 32, 8);
        s->align = get(h + 48, 8);
        if (s->type != SHT_NOBITS) {
            if (off + s->size > size)
                fail("%s: section %s out of the file", name, s->name);
            s->data = d + off;
        }
        if (s->type == SHT_SYMTAB)
            symtab = i;
    }
    /* relocations onto their sections */
    for (int i = 0; i < shnum; i++) {
        const unsigned char *h = d + shoff + 64 * (uint64_t)i;
        if (o->sections[i].type != SHT_RELA)
            continue;
        int target = (int)get(h + 44, 4);
        if (target <= 0 || target >= shnum)
            fail("%s: a relocation section for no section", name);
        o->sections[target].rela = o->sections[i].data;
        o->sections[target].nrela = o->sections[i].size / 24;
    }
    if (symtab < 0)
        fail("%s: no symbol table", name);
    const unsigned char *sh = d + shoff + 64 * (uint64_t)symtab;
    const unsigned char *syms = o->sections[symtab].data;
    int nsyms = (int)(o->sections[symtab].size / 24);
    int strndx = (int)get(sh + 40, 4);
    const char *strtab = (const char *)o->sections[strndx].data;
    o->nsymbols = nsyms;
    o->symbols = alloc(sizeof(struct symbol *) * (size_t)nsyms);
    for (int i = 1; i < nsyms; i++) {
        const unsigned char *s = syms + 24 * (size_t)i;
        const char *sname = strtab + get(s, 4);
        int info = s[4], bind = info >> 4, type = info & 15, visibility = s[5] & 3;
        int shndx = (int)get(s + 6, 2);
        uint64_t value = get(s + 8, 8), ssize = get(s + 16, 8);
        struct section *sec = shndx > 0 && shndx < shnum ? &o->sections[shndx] : 0;
        if (bind == STB_LOCAL) {
            struct symbol *l = alloc(sizeof(*l));
            l->name = (char *)sname;
            l->obj = o;
            l->section = sec;
            l->value = value;
            l->type = type;
            l->binding = STB_LOCAL;
            l->defined = true;
            l->is_absolute = shndx == SHN_ABS;
            o->symbols[i] = l;
            continue;
        }
        struct symbol *g = global(sname);
        o->symbols[i] = g;
        if (visibility == 1 || visibility == 2)
            g->hidden = true;
        if (shndx == SHN_UNDEF) {
            if (bind == STB_GLOBAL && g->binding == STB_WEAK && !g->defined)
                g->binding = STB_GLOBAL;        /* (a strong reference) */
            else if (!g->defined && !g->binding)
                g->binding = bind;
            g->referenced = true;
            continue;
        }
        if (shndx == SHN_COMMON) {
            /* a common symbol: a zero-filled object unless someone defines it */
            if (!g->defined || g->shlib || (g->is_common && ssize > g->size)) {
                g->obj = o;
                g->section = 0;
                g->size = ssize;
                g->value = value;               /* (its alignment) */
                g->defined = true;
                g->is_common = true;
                g->binding = bind;
                g->type = STT_OBJECT;
                g->shlib = 0;
            }
            continue;
        }
        define(g, o, sec, value, ssize, bind, type, shndx == SHN_ABS);
    }
    o->loaded = true;
    objects = realloc(objects, sizeof(*objects) * (size_t)(nobjects + 1));
    objects[nobjects++] = o;
}

/* --- archives ------------------------------------------------------------ */

static void load_archive(const char *path, unsigned char *d, size_t size)
{
    /* GNU format: "/" lists the symbols and their members' offsets, "//" the long names */
    size_t at = 8;
    const char *long_names = 0;
    size_t long_size = 0;
    struct { uint64_t offset; struct member *m; } *members = 0;
    int nmembers = 0;
    const unsigned char *index = 0;
    size_t index_size = 0;
    while (at + 60 <= size) {
        const unsigned char *h = d + at;
        char sizebuf[11];
        memcpy(sizebuf, h + 48, 10);
        sizebuf[10] = 0;
        size_t msize = (size_t)strtoull(sizebuf, 0, 10);
        const unsigned char *data = d + at + 60;
        if (at + 60 + msize > size)
            fail("%s: a member out of the archive", path);
        if (h[0] == '/' && h[1] == ' ') {
            index = data;
            index_size = msize;
        } else if (h[0] == '/' && h[1] == '/') {
            long_names = (const char *)data;
            long_size = msize;
        } else {
            char name[256];
            if (h[0] == '/') {
                size_t off = (size_t)strtoull((const char *)h + 1, 0, 10);
                size_t k = 0;
                while (off + k < long_size && long_names[off + k] != '/' && long_names[off + k] != '\n' && k < 255) {
                    name[k] = long_names[off + k];
                    k++;
                }
                name[k] = 0;
            } else {
                size_t k = 0;
                while (k < 16 && h[k] != '/' && h[k] != ' ' && k < 255) {
                    name[k] = (char)h[k];
                    k++;
                }
                name[k] = 0;
            }
            struct member *m = alloc(sizeof(*m));
            m->archive = (char *)path;
            m->data = (unsigned char *)data;
            m->size = msize;
            m->name = alloc(strlen(path) + strlen(name) + 3);
            snprintf(m->name, strlen(path) + strlen(name) + 3, "%s(%s)", path, name);
            members = realloc(members, sizeof(*members) * (size_t)(nmembers + 1));
            members[nmembers].offset = at;
            members[nmembers].m = m;
            nmembers++;
        }
        at += 60 + msize;
        at = (at + 1) & ~(size_t)1;
    }
    if (!index)
        fail("%s: an archive without a symbol index (make it with ar s)", path);
    /* big-endian count, offsets, then the names */
    uint32_t count = (uint32_t)index[0] << 24 | (uint32_t)index[1] << 16 | (uint32_t)index[2] << 8 | index[3];
    const char *names = (const char *)index + 4 + 4 * (size_t)count;
    for (uint32_t i = 0; i < count; i++) {
        const unsigned char *p = index + 4 + 4 * (size_t)i;
        uint64_t off = (uint64_t)p[0] << 24 | (uint64_t)p[1] << 16 | (uint64_t)p[2] << 8 | p[3];
        if ((const unsigned char *)names >= index + index_size)
            fail("%s: a bad symbol index", path);
        struct member *m = 0;
        for (int k = 0; k < nmembers; k++)
            if (members[k].offset == off)
                m = members[k].m;
        struct symbol *g = global(names);
        if (m && !g->defined && !g->member) {
            g->member = m;
            g->lazy_member = true;
        }
        names += strlen(names) + 1;
    }
    free(members);
}

/* --- shared libraries ---------------------------------------------------- */

static void load_shlib(const char *path, unsigned char *d, size_t size)
{
    check_header(d, size, path, 3);
    struct shlib *l = alloc(sizeof(*l));
    l->path = copy_string(path);
    uint64_t shoff = get(d + 40, 8);
    int shnum = (int)get(d + 60, 2);
    const unsigned char *dynsym = 0, *dynstr = 0, *dynamic = 0;
    uint64_t ndynsym = 0, ndynamic = 0;
    for (int i = 0; i < shnum; i++) {
        const unsigned char *h = d + shoff + 64 * (uint64_t)i;
        int type = (int)get(h + 4, 4);
        uint64_t off = get(h + 24, 8), sz = get(h + 32, 8);
        if (type == SHT_DYNSYM) {
            dynsym = d + off;
            ndynsym = sz / 24;
            int link = (int)get(h + 40, 4);
            dynstr = d + get(d + shoff + 64 * (uint64_t)link + 24, 8);
        } else if (type == SHT_DYNAMIC) {
            dynamic = d + off;
            ndynamic = sz / 16;
        }
    }
    if (!dynsym)
        fail("%s: no dynamic symbols", path);
    for (uint64_t i = 0; i < ndynamic; i++) {
        uint64_t tag = get(dynamic + 16 * i, 8), val = get(dynamic + 16 * i + 8, 8);
        if (tag == 14)                          /* DT_SONAME */
            l->soname = copy_string((const char *)dynstr + val);
    }
    if (!l->soname) {
        const char *slash = strrchr(path, '/');
        l->soname = copy_string(slash ? slash + 1 : path);
    }
    for (uint64_t i = 1; i < ndynsym; i++) {
        const unsigned char *s = dynsym + 24 * i;
        int bind = s[4] >> 4, type = s[4] & 15, shndx = (int)get(s + 6, 2);
        if (bind == STB_LOCAL || shndx == SHN_UNDEF)
            continue;
        struct symbol *g = global((const char *)dynstr + get(s, 4));
        if (g->defined)
            continue;
        g->defined = true;
        g->shlib = l;
        g->type = type;
        g->size = get(s + 16, 8);
        g->binding = g->binding ? g->binding : bind;
    }
    if (shlibs_tail)
        shlibs_tail->next = l;
    else
        shlibs = l;
    shlibs_tail = l;
}

static void load_file(const char *path)
{
    size_t size;
    unsigned char *d = read_file(path, &size);
    if (!d)
        fail("cannot read %s", path);
    if (size >= 8 && memcmp(d, "!<arch>\n", 8) == 0)
        load_archive(path, d, size);
    else if (size >= 18 && memcmp(d, "\177ELF", 4) == 0 && get(d + 16, 2) == 3)
        load_shlib(path, d, size);
    else
        load_object(path, d, size);
}

/* Archive members for every symbol still undefined, until none is left. */
static void load_members(void)
{
    for (bool again = true; again;) {
        again = false;
        for (int h = 0; h < 16384; h++)
            for (struct symbol *g = table[h]; g; g = g->next)
                if (g->referenced && (!g->defined || g->shlib) && g->member && !g->member->loaded &&
                    g->binding != STB_WEAK) {
                    /* (a member wins over a shared library, as lld) */
                    if (g->shlib)
                        continue;
                    g->member->loaded = true;
                    load_object(g->member->name, g->member->data, g->member->size);
                    again = true;
                }
    }
}

/* --- output sections ----------------------------------------------------- */

struct output {
    const char *name;
    int type;
    uint64_t flags;
    uint64_t align;
    uint64_t addr, offset, size;
    unsigned char *data;
    struct section **parts;
    int nparts;
    int index;                                  /* in the section headers */
};

enum {
    O_INTERP, O_HASH, O_DYNSYM, O_DYNSTR, O_RELA_DYN, O_RELA_PLT, O_TEXT, O_PLT, O_RODATA, O_TDATA, O_TBSS,
    O_DATA_REL_RO,
    O_DYNAMIC, O_GOT, O_GOT_PLT, O_INIT_ARRAY, O_FINI_ARRAY, O_DATA, O_BSS, O_HELP, O_SHSTRTAB, O_COUNT
};

static struct output outs[O_COUNT] = {
    [O_INTERP] = { .name = ".interp", .type = SHT_PROGBITS, .flags = SHF_ALLOC, .align = 1 },
    [O_HASH] = { .name = ".hash", .type = SHT_HASH, .flags = SHF_ALLOC, .align = 4 },
    [O_DYNSYM] = { .name = ".dynsym", .type = SHT_DYNSYM, .flags = SHF_ALLOC, .align = 8 },
    [O_DYNSTR] = { .name = ".dynstr", .type = SHT_STRTAB, .flags = SHF_ALLOC, .align = 1 },
    [O_RELA_DYN] = { .name = ".rela.dyn", .type = SHT_RELA, .flags = SHF_ALLOC, .align = 8 },
    [O_RELA_PLT] = { .name = ".rela.plt", .type = SHT_RELA, .flags = SHF_ALLOC | 0x40, .align = 8 },
    [O_TEXT] = { .name = ".text", .type = SHT_PROGBITS, .flags = SHF_ALLOC | SHF_EXECINSTR, .align = 16 },
    [O_PLT] = { .name = ".plt", .type = SHT_PROGBITS, .flags = SHF_ALLOC | SHF_EXECINSTR, .align = 16 },
    [O_RODATA] = { .name = ".rodata", .type = SHT_PROGBITS, .flags = SHF_ALLOC, .align = 1 },
    [O_TDATA] = { .name = ".tdata", .type = SHT_PROGBITS, .flags = SHF_ALLOC | SHF_WRITE | SHF_TLS, .align = 1 },
    [O_TBSS] = { .name = ".tbss", .type = SHT_NOBITS, .flags = SHF_ALLOC | SHF_WRITE | SHF_TLS, .align = 1 },
    [O_DATA_REL_RO] = { .name = ".data.rel.ro", .type = SHT_PROGBITS, .flags = SHF_ALLOC | SHF_WRITE, .align = 1 },
    [O_DYNAMIC] = { .name = ".dynamic", .type = SHT_DYNAMIC, .flags = SHF_ALLOC | SHF_WRITE, .align = 8 },
    [O_GOT] = { .name = ".got", .type = SHT_PROGBITS, .flags = SHF_ALLOC | SHF_WRITE, .align = 8 },
    [O_GOT_PLT] = { .name = ".got.plt", .type = SHT_PROGBITS, .flags = SHF_ALLOC | SHF_WRITE, .align = 8 },
    [O_INIT_ARRAY] = { .name = ".init_array", .type = SHT_INIT_ARRAY, .flags = SHF_ALLOC | SHF_WRITE, .align = 8 },
    [O_FINI_ARRAY] = { .name = ".fini_array", .type = SHT_FINI_ARRAY, .flags = SHF_ALLOC | SHF_WRITE, .align = 8 },
    [O_DATA] = { .name = ".data", .type = SHT_PROGBITS, .flags = SHF_ALLOC | SHF_WRITE, .align = 1 },
    [O_BSS] = { .name = ".bss", .type = SHT_NOBITS, .flags = SHF_ALLOC | SHF_WRITE, .align = 1 },
    [O_HELP] = { .name = ".ritteros.help", .type = SHT_PROGBITS, .flags = 0, .align = 1 },
    [O_SHSTRTAB] = { .name = ".shstrtab", .type = SHT_STRTAB, .flags = 0, .align = 1 },
};

static bool prefix(const char *s, const char *p)
{
    size_t n = strlen(p);
    return strncmp(s, p, n) == 0 && (s[n] == 0 || s[n] == '.');
}

/* Which output section an input section goes to (user/user.ld's rules); -1: dropped. */
static int output_of(struct section *s)
{
    const char *n = s->name;
    if (s->type == SHT_GROUP || s->type == SHT_SYMTAB || s->type == SHT_STRTAB || s->type == SHT_RELA)
        return -1;
    if (strcmp(n, ".ritteros.help") == 0)
        return O_HELP;
    if (!(s->flags & SHF_ALLOC))
        return -1;
    if (prefix(n, ".note") || prefix(n, ".eh_frame") || prefix(n, ".comment") || strcmp(n, ".llvm_addrsig") == 0)
        return -1;
    if (s->flags & SHF_TLS)
        return s->type == SHT_NOBITS ? O_TBSS : O_TDATA;   /* (62b) */
    if (prefix(n, ".text") || (s->flags & SHF_EXECINSTR))
        return O_TEXT;
    if (prefix(n, ".data.rel.ro"))
        return O_DATA_REL_RO;
    if (prefix(n, ".init_array"))
        return O_INIT_ARRAY;
    if (prefix(n, ".fini_array"))
        return O_FINI_ARRAY;
    if (s->type == SHT_NOBITS || prefix(n, ".bss"))
        return O_BSS;
    if (!(s->flags & SHF_WRITE))
        return O_RODATA;
    return O_DATA;
}

static void add_part(struct output *o, struct section *s)
{
    o->parts = realloc(o->parts, sizeof(*o->parts) * (size_t)(o->nparts + 1));
    o->parts[o->nparts++] = s;
    s->out = o;
    if (s->align > o->align)
        o->align = s->align;
}

/* --- relocations: what each needs ---------------------------------------- */

/* relocation types (x86-64, AArch64) */
#define X_64 1
#define X_PC32 2
#define X_PLT32 4
#define X_GLOB_DAT 6
#define X_JUMP_SLOT 7
#define X_RELATIVE 8
#define X_GOTPCREL 9
#define X_32 10
#define X_32S 11
#define X_PC64 24
#define X_GOTPCRELX 41
#define X_REX_GOTPCRELX 42
#define A_ABS64 257
#define A_ABS32 258
#define A_PREL64 260
#define A_PREL32 261
#define A_ADR_PREL_PG_HI21 275
#define A_ADD_ABS_LO12_NC 277
#define A_LDST8 278
#define A_TSTBR14 279
#define A_CONDBR19 280
#define A_JUMP26 282
#define A_CALL26 283
#define A_LDST16 284
#define A_LDST32 285
#define A_LDST64 286
#define A_LDST128 299
#define A_ADR_GOT_PAGE 311
#define A_LD64_GOT_LO12_NC 312
/* (62b) per-thread data */
#define X_DTPMOD64 16
#define X_DTPOFF64 17
#define X_TLSGD 19
#define X_TLSLD 20
#define X_DTPOFF32 21
#define A_TLSDESC_ADR_PAGE21 562
#define A_TLSDESC_LD64_LO12 563
#define A_TLSDESC_ADD_LO12 564
#define A_TLSDESC_CALL 569
#define A_TLSDESC 1031
#define A_GLOB_DAT 1025
#define A_JUMP_SLOT 1026
#define A_RELATIVE 1027

static struct symbol **got_list, **plt_list, **dyn_list;
static int ngot, nplt, ndyn;
/* (62b) TLS GOT pairs after the GOT's entries (x86: module and offset;
 * AArch64: a descriptor), and x86's local-dynamic module pair */
static struct symbol **pair_list;
static int npairs;
static bool module_pair;
static struct section *desc_stub;               /* (62d) AArch64 static: ldr x0, [x0, #8]; ret */
static uint64_t tls_start, tls_memsz, tls_align;
static uint64_t nabsolute;                      /* (64-bit addresses in loaded data: a dynamic relocation each) */
/* (62g) ld --efi: a UEFI application (PE32+), every 64-bit address in it
 * listed for the base relocations the firmware applies where it loads it */
static bool efi_out;
static uint64_t *abs_at;
static int nabs_at, capabs_at;

struct dynrel {
    uint64_t offset;
    int type;
    struct symbol *sym;                         /* 0: RELATIVE */
    int64_t addend;
};
static struct dynrel *dynrels;
static int ndynrels, capdynrels;

static void add_dynrel(uint64_t offset, int type, struct symbol *sym, int64_t addend)
{
    if (ndynrels == capdynrels) {
        capdynrels = capdynrels ? capdynrels * 2 : 256;
        dynrels = realloc(dynrels, sizeof(*dynrels) * (size_t)capdynrels);
    }
    dynrels[ndynrels].offset = offset;
    dynrels[ndynrels].type = type;
    dynrels[ndynrels].sym = sym;
    dynrels[ndynrels].addend = addend;
    ndynrels++;
}

static bool imported(struct symbol *s)
{
    return s && s->shlib;
}

static void need_dynsym(struct symbol *s)
{
    if (s->dynsym)
        return;
    dyn_list = realloc(dyn_list, sizeof(*dyn_list) * (size_t)(ndyn + 1));
    dyn_list[ndyn++] = s;
    s->dynsym = ndyn;                           /* (index 0 is the null symbol) */
    if (s->shlib)
        s->shlib->needed = true;
}

static void need_got(struct symbol *s)
{
    if (s->got)
        return;
    got_list = realloc(got_list, sizeof(*got_list) * (size_t)(ngot + 1));
    got_list[ngot++] = s;
    s->got = ngot;
    if (imported(s))
        need_dynsym(s);
}

static void need_pair(struct symbol *s)
{
    if (s->tls_pair)
        return;
    pair_list = realloc(pair_list, sizeof(*pair_list) * (size_t)(npairs + 1));
    pair_list[npairs++] = s;
    s->tls_pair = npairs;
    if (imported(s))
        need_dynsym(s);
}

static void need_plt(struct symbol *s)
{
    if (s->plt)
        return;
    plt_list = realloc(plt_list, sizeof(*plt_list) * (size_t)(nplt + 1));
    plt_list[nplt++] = s;
    s->plt = nplt;
    need_dynsym(s);
}

static bool is_got_reloc(int type)
{
    return type == X_GOTPCREL || type == X_GOTPCRELX || type == X_REX_GOTPCRELX || type == A_ADR_GOT_PAGE ||
           type == A_LD64_GOT_LO12_NC;
}

static bool is_call_reloc(int type)
{
    return type == X_PLT32 || type == A_CALL26 || type == A_JUMP26 || (type == X_PC32 && 0);
}

static struct symbol *reloc_symbol(struct section *s, uint64_t k, int *type, int64_t *addend, uint64_t *offset)
{
    const unsigned char *r = s->rela + 24 * k;
    *offset = get(r, 8);
    uint64_t info = get(r + 8, 8);
    *type = (int)(info & 0xFFFFFFFF);
    *addend = (int64_t)get(r + 16, 8);
    uint32_t index = (uint32_t)(info >> 32);
    if (!index)
        return 0;
    if ((int)index >= s->obj->nsymbols)
        fail("%s: a relocation's symbol out of range", s->obj->name);
    return s->obj->symbols[index];
}

static void scan_relocations(void)
{
    for (int i = 0; i < nobjects; i++) {
        struct object *o = objects[i];
        for (int j = 0; j < o->nsections; j++) {
            struct section *s = &o->sections[j];
            if (!s->out || !s->nrela)
                continue;
            for (uint64_t k = 0; k < s->nrela; k++) {
                int type;
                int64_t addend;
                uint64_t offset;
                struct symbol *sym = reloc_symbol(s, k, &type, &addend, &offset);
                if ((type == X_64 || type == A_ABS64) && (s->out->flags & SHF_ALLOC))
                    nabsolute++;
                if (!sym)
                    continue;
                if (sym->binding != STB_LOCAL && !sym->defined) {
                    if (sym->binding == STB_WEAK)
                        continue;               /* (an undefined weak symbol is 0) */
                    fail("undefined symbol: %s (referenced by %s)", sym->name, o->name);
                }
                if (type == X_TLSGD || type == A_TLSDESC_ADR_PAGE21 || type == A_TLSDESC_LD64_LO12 ||
                    type == A_TLSDESC_ADD_LO12)
                    need_pair(sym);
                else if (type == X_TLSLD)
                    module_pair = true;
                else if (is_got_reloc(type))
                    need_got(sym);
                else if (is_call_reloc(type) && imported(sym))
                    need_plt(sym);
                else if (imported(sym) && (type == X_64 || type == A_ABS64))
                    need_dynsym(sym);
                else if (imported(sym) && (type == X_PC32 || type == A_PREL32)) {
                    if (sym->type == STT_FUNC)
                        need_plt(sym);
                    else
                        fail("%s: a PC-relative reference to %s, which a library defines (data in a library "
                             "cannot be reached that way)", o->name, sym->name);
                }
            }
        }
    }
}

/* --- layout -------------------------------------------------------------- */

static struct { char *data; size_t len, cap; } dynstr, shstrtab;

static uint32_t strtab_add(char **data, size_t *len, size_t *cap, const char *s)
{
    size_t n = strlen(s) + 1;
    if (*len + n > *cap) {
        *cap = *cap ? *cap * 2 : 1024;
        while (*cap < *len + n)
            *cap *= 2;
        *data = realloc(*data, *cap);
    }
    memcpy(*data + *len, s, n);
    *len += n;
    return (uint32_t)(*len - n);
}

static uint32_t dynstr_add(const char *s)
{
    return strtab_add(&dynstr.data, &dynstr.len, &dynstr.cap, s);
}

static uint64_t symbol_address(struct symbol *s)
{
    if (!s || !s->defined)
        return 0;
    if (s->synthetic)
        return s->value;
    if (s->shlib)
        return s->plt ? outs[O_PLT].addr + 16 * (uint64_t)(s->plt - 1) : 0;
    if (s->is_absolute)
        return s->value;
    if (s->is_common)
        return s->value;                        /* (set when placed in .bss) */
    if (!s->section || !s->section->out)
        return 0;
    return s->section->out->addr + s->section->offset + s->value;
}

static uint64_t got_address(struct symbol *s)
{
    return outs[O_GOT].addr + 8 * (uint64_t)(s->got - 1);
}

static uint64_t pair_address(struct symbol *s)
{
    return outs[O_GOT].addr + 8 * (uint64_t)ngot + 16 * (uint64_t)(s->tls_pair - 1);
}

static uint64_t module_pair_address(void)
{
    return outs[O_GOT].addr + 8 * (uint64_t)ngot + 16 * (uint64_t)npairs;
}

/* a per-thread symbol's place in its module's block */
static uint64_t tls_offset(struct symbol *s)
{
    return symbol_address(s) - tls_start;
}

static uint64_t plt_slot(struct symbol *s)
{
    return outs[O_GOT_PLT].addr + 8 * (uint64_t)(s->plt - 1);
}

struct segment {
    int type;
    int flags;
    uint64_t offset, vaddr, filesz, memsz, align;
};

static struct segment segments[16];
static int nsegments;
static uint32_t elf_hash(const char *s)
{
    uint32_t h = 0;
    for (; *s; s++) {
        h = (h << 4) + (unsigned char)*s;
        uint32_t g = h & 0xF0000000u;
        if (g)
            h ^= g >> 24;
        h &= ~g;
    }
    return h;
}

/* --- the program --------------------------------------------------------- */

static void add_input(const char *arg, char **dirs, int ndirs)
{
    if (strncmp(arg, "-l", 2) == 0) {
        const char *name = arg + 2;
        for (int i = 0; i < ndirs; i++) {
            char path[1024];
            /* a shared library (libNAME.so or libNAME.so.1), else the archive */
            const char *forms[] = { "%s/lib%s.so", "%s/lib%s.so.1", "%s/lib%s.a" };
            for (int f = 0; f < 3; f++) {
                snprintf(path, sizeof(path), forms[f], dirs[i], name);
                FILE *fp = fopen(path, "rb");
                if (fp) {
                    fclose(fp);
                    load_file(path);
                    return;
                }
            }
        }
        fail("cannot find -l%s", name);
    }
    load_file(arg);
}

static void apply_relocations(unsigned char *image)
{
    for (int i = 0; i < nobjects; i++) {
        struct object *o = objects[i];
        for (int j = 0; j < o->nsections; j++) {
            struct section *s = &o->sections[j];
            if (!s->out || !s->nrela || s->type == SHT_NOBITS)
                continue;
            unsigned char *base = s->out->data + s->offset;
            uint64_t sec_addr = s->out->addr + s->offset;
            for (uint64_t k = 0; k < s->nrela; k++) {
                int type;
                int64_t A;
                uint64_t off;
                struct symbol *sym = reloc_symbol(s, k, &type, &A, &off);
                if (off + 4 > s->size && !(type == X_64 || type == A_ABS64))
                    fail("%s: a relocation out of its section", o->name);
                unsigned char *at = base + off;
                uint64_t P = sec_addr + off;
                uint64_t S;
                if (sym && sym->binding == STB_LOCAL && sym->type == STT_SECTION)
                    S = sym->section && sym->section->out ? sym->section->out->addr + sym->section->offset : 0;
                else
                    S = symbol_address(sym);
                bool weak_undef = sym && !sym->defined && sym->binding == STB_WEAK;
                bool alloc_target = (s->out->flags & SHF_ALLOC) != 0;
                switch (type) {
                case X_64: case A_ABS64:
                    if (imported(sym)) {
                        add_dynrel(P, type, sym, A);
                        put(at, 0, 8);
                    } else if (sym && (sym->is_absolute || weak_undef || !alloc_target)) {
                        put(at, S + (uint64_t)A, 8);
                    } else {
                        put(at, S + (uint64_t)A, 8);
                        if (efi_out) {
                            if (nabs_at == capabs_at) {
                                capabs_at = capabs_at ? 2 * capabs_at : 64;
                                abs_at = realloc(abs_at, sizeof(uint64_t) * (size_t)capabs_at);
                            }
                            abs_at[nabs_at++] = P;
                        }
                        if (mode != MODE_STATIC)    /* (62d: a static program's addresses are final) */
                            add_dynrel(P, machine == EM_X86_64 ? X_RELATIVE : A_RELATIVE, 0, (int64_t)(S + (uint64_t)A));
                    }
                    break;
                case X_PC32: case X_PLT32: case A_PREL32: {
                    uint64_t T = sym && sym->plt ? outs[O_PLT].addr + 16 * (uint64_t)(sym->plt - 1) : S;
                    int64_t v = (int64_t)(T + (uint64_t)A - P);
                    if (v < INT32_MIN || v > INT32_MAX)
                        fail("%s: a 32-bit PC-relative reference out of range", o->name);
                    put(at, (uint64_t)v, 4);
                    break;
                }
                case X_PC64: case A_PREL64:
                    put(at, S + (uint64_t)A - P, 8);
                    break;
                case X_GOTPCREL: case X_GOTPCRELX: case X_REX_GOTPCRELX: {
                    int64_t v = (int64_t)(got_address(sym) + (uint64_t)A - P);
                    put(at, (uint64_t)v, 4);
                    break;
                }
                case X_32: case X_32S: {
                    if (efi_out)
                        fail("%s: an absolute 32-bit reference to %s cannot be relocated in a UEFI image",
                             o->name, sym ? sym->name : "?");
                    if (mode != MODE_STATIC)
                        fail("%s: an absolute 32-bit reference to %s cannot be in a position-independent program "
                             "(build it with -fPIC)", o->name, sym ? sym->name : "?");
                    uint64_t v = S + (uint64_t)A;
                    if (type == X_32 ? v > 0xFFFFFFFFu : (int64_t)v != (int32_t)v)
                        fail("%s: a 32-bit address of %s out of range", o->name, sym ? sym->name : "?");
                    put(at, v, 4);
                    break;
                }
                case A_ADR_PREL_PG_HI21: case A_ADR_GOT_PAGE: {
                    uint64_t T = type == A_ADR_GOT_PAGE ? got_address(sym) : S + (uint64_t)A;
                    if (type == A_ADR_GOT_PAGE)
                        T += (uint64_t)A;
                    int64_t pages = (int64_t)((T & ~(uint64_t)0xFFF) - (P & ~(uint64_t)0xFFF)) >> 12;
                    if (pages < -(1 << 20) || pages >= (1 << 20))
                        fail("%s: adrp out of range", o->name);
                    uint32_t w = (uint32_t)get(at, 4);
                    w &= ~((3u << 29) | (0x7FFFFu << 5));
                    w |= ((uint32_t)pages & 3) << 29 | (((uint32_t)pages >> 2) & 0x7FFFF) << 5;
                    put(at, w, 4);
                    break;
                }
                case A_ADD_ABS_LO12_NC: case A_LDST8: case A_LDST16: case A_LDST32: case A_LDST64: case A_LDST128:
                case A_LD64_GOT_LO12_NC: {
                    uint64_t T = type == A_LD64_GOT_LO12_NC ? got_address(sym) + (uint64_t)A : S + (uint64_t)A;
                    int shift = type == A_LDST16 ? 1 : type == A_LDST32 ? 2 : type == A_LDST64 || type == A_LD64_GOT_LO12_NC ? 3
                                : type == A_LDST128 ? 4 : 0;
                    uint64_t lo = T & 0xFFF;
                    if (lo & ((1u << shift) - 1))
                        fail("%s: a misaligned low-12-bits reference to %s", o->name, sym ? sym->name : "?");
                    uint32_t w = (uint32_t)get(at, 4);
                    w &= ~(0xFFFu << 10);
                    w |= (uint32_t)(lo >> shift) << 10;
                    put(at, w, 4);
                    break;
                }
                case X_TLSGD: case X_TLSLD: {
                    uint64_t T = type == X_TLSGD ? pair_address(sym) : module_pair_address();
                    put(at, T + (uint64_t)A - P, 4);
                    break;
                }
                case X_DTPOFF32:
                    put(at, tls_offset(sym) + (uint64_t)A, 4);
                    break;
                case X_DTPOFF64:
                    put(at, tls_offset(sym) + (uint64_t)A, 8);
                    break;
                case A_TLSDESC_ADR_PAGE21: {
                    uint64_t T = pair_address(sym);
                    int64_t pages = (int64_t)((T & ~(uint64_t)0xFFF) - (P & ~(uint64_t)0xFFF)) >> 12;
                    uint32_t w = (uint32_t)get(at, 4);
                    w &= ~((3u << 29) | (0x7FFFFu << 5));
                    w |= ((uint32_t)pages & 3) << 29 | (((uint32_t)pages >> 2) & 0x7FFFF) << 5;
                    put(at, w, 4);
                    break;
                }
                case A_TLSDESC_LD64_LO12: case A_TLSDESC_ADD_LO12: {
                    uint64_t lo = pair_address(sym) & 0xFFF;
                    uint32_t w = (uint32_t)get(at, 4);
                    w &= ~(0xFFFu << 10);
                    w |= (uint32_t)(type == A_TLSDESC_LD64_LO12 ? lo >> 3 : lo) << 10;
                    put(at, w, 4);
                    break;
                }
                case A_TLSDESC_CALL:
                    break;
                case A_CALL26: case A_JUMP26: {
                    uint64_t T = sym && sym->plt ? outs[O_PLT].addr + 16 * (uint64_t)(sym->plt - 1) : S;
                    if (weak_undef)
                        T = P;                  /* (a call of nothing: to itself, as lld leaves it) */
                    int64_t v = (int64_t)(T + (uint64_t)A - P);
                    if (v < -(1 << 27) || v >= (1 << 27))
                        fail("%s: a branch out of range", o->name);
                    uint32_t w = (uint32_t)get(at, 4);
                    w = (w & 0xFC000000u) | ((uint32_t)(v >> 2) & 0x3FFFFFF);
                    put(at, w, 4);
                    break;
                }
                case A_CONDBR19: {
                    int64_t v = (int64_t)(S + (uint64_t)A - P);
                    uint32_t w = (uint32_t)get(at, 4);
                    w = (w & ~(0x7FFFFu << 5)) | (((uint32_t)(v >> 2) & 0x7FFFF) << 5);
                    put(at, w, 4);
                    break;
                }
                case A_TSTBR14: {
                    int64_t v = (int64_t)(S + (uint64_t)A - P);
                    uint32_t w = (uint32_t)get(at, 4);
                    w = (w & ~(0x3FFFu << 5)) | (((uint32_t)(v >> 2) & 0x3FFF) << 5);
                    put(at, w, 4);
                    break;
                }
                case 0:
                    break;
                default:
                    fail("%s: relocation type %d is not supported", o->name, type);
                }
            }
        }
    }
    (void)image;
}

/* (62d) a version script as scripts/libexports.c writes them:
 * { global: NAME; ... local: *; }, comments allowed */
static void read_version_script(const char *path)
{
    size_t size;
    char *text = (char *)read_file(path, &size);
    have_version_script = true;
    bool global_part = true;
    for (char *p = text; p < text + size;) {
        if (p[0] == '/' && p[1] == '*') {
            char *end = strstr(p + 2, "*/");
            p = end ? end + 2 : text + size;
            continue;
        }
        if (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == '{' || *p == '}' || *p == ';') {
            p++;
            continue;
        }
        char *start = p;
        while (p < text + size && !strchr(" \t\n\r{};", *p))
            p++;
        size_t n = (size_t)(p - start);
        if (n == 7 && strncmp(start, "global:", 7) == 0)
            global_part = true;
        else if (n == 6 && strncmp(start, "local:", 6) == 0)
            global_part = false;
        else if (global_part) {
            exports = realloc(exports, sizeof(char *) * (size_t)(nexports + 1));
            char *name = alloc(n + 1);
            memcpy(name, start, n);
            exports[nexports++] = name;
        } else if (!(n == 1 && *start == '*'))
            fail("%s: only 'local: *;' is supported", path);
    }
}

static bool exported_name(const char *name)
{
    if (!have_version_script)
        return true;
    for (int i = 0; i < nexports; i++)
        if (strcmp(exports[i], name) == 0)
            return true;
    return false;
}

/* --- linker scripts (62f) ------------------------------------------------ */

/* The subset a kernel's script uses (kernel/linker.ld): ENTRY(sym);
 * SECTIONS { ... } with `. = EXPR;`, `SYM = EXPR;`, output sections
 * `NAME [ADDR] [(NOLOAD)] : [ALIGN(N)] { ... }` holding input sections
 * `*(PATTERN ...)` (also in KEEP()), `*(COMMON)`, assignments and `. += N`,
 * and /DISCARD/. Expressions: numbers (K and M suffixes), `.`, symbols,
 * ALIGN(N), ALIGN(EXPR, N), + - * / % & | << >> ~ and parentheses. The
 * result is a static executable: one PT_LOAD for each run of sections with
 * the same permissions, and the symbol table. Input sections go to the
 * first rule in the script that names them (GNU's order); an allocated
 * section no rule names is an error rather than placed by guesswork. */

struct script_input {
    char **patterns;
    int npatterns;
    bool common;
    struct section **parts;
    int nparts;
};

enum { SI_ASSIGN, SI_INPUT };

struct script_item {
    int kind;
    char *sym;                                  /* SI_ASSIGN: the name, or "." */
    char op;                                    /* '=', '+' (+=) or '-' (-=) */
    const char *expr;                           /* the expression's text, read when laid out */
    struct script_input in;
};

struct script_section {
    char *name;                                 /* 0: an assignment between sections */
    struct script_item assign;
    const char *addr, *align;
    bool noload, discard;
    struct script_item *items;
    int nitems;
    struct output out;
};

static struct script_section *ss;
static int nss;
static const char *script_text, *script_name;
static uint64_t dot;

static _Noreturn void script_error(const char *p, const char *what)
{
    int line = 1;
    for (const char *q = script_text; q < p && *q; q++)
        line += *q == '\n';
    fail("%s:%d: %s", script_name, line, what);
}

static const char *script_skip(const char *p)
{
    for (;;) {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
            p++;
        if (p[0] == '/' && p[1] == '*') {
            const char *e = strstr(p + 2, "*/");
            if (!e)
                script_error(p, "a comment without its end");
            p = e + 2;
            continue;
        }
        return p;
    }
}

static bool word_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.' ||
           c == '$' || c == '*' || c == '?' || c == '/' || c == '-' || c == '[' || c == ']';
}

/* a name or a pattern */
static char *script_word(const char **pp)
{
    const char *p = script_skip(*pp), *start = p;
    while (word_char(*p))
        p++;
    if (p == start)
        script_error(p, "a name expected");
    *pp = p;
    char *w = alloc((size_t)(p - start) + 1);
    memcpy(w, start, (size_t)(p - start));
    return w;
}

static const char *script_expect(const char *p, char c)
{
    p = script_skip(p);
    if (*p != c) {
        char what[32];
        snprintf(what, sizeof(what), "'%c' expected", c);
        script_error(p, what);
    }
    return p + 1;
}

/* the text of an expression up to `end` (a ';', or ')' at depth 0) */
static const char *script_expr_text(const char **pp, char end)
{
    const char *p = script_skip(*pp), *start = p;
    int depth = 0;
    while (*p && !(depth == 0 && (*p == end || (end == ':' && *p == '(' && strncmp(script_skip(p + 1), "NOLOAD", 6) == 0)))) {
        if (*p == '(')
            depth++;
        else if (*p == ')')
            depth--;
        p++;
    }
    if (!*p)
        script_error(start, "an expression without its end");
    *pp = p;
    return start;
}

/* --- expressions, evaluated with the current `.` */

static uint64_t script_value(const char **pp);

static uint64_t script_primary(const char **pp)
{
    const char *p = script_skip(*pp);
    uint64_t v;
    if (*p == '(') {
        p++;
        v = script_value(&p);
        p = script_expect(p, ')');
    } else if (*p == '-' || *p == '~' || *p == '!') {
        char op = *p++;
        v = script_primary(&p);
        v = op == '-' ? 0 - v : op == '~' ? ~v : !v;
    } else if (*p >= '0' && *p <= '9') {
        char *end;
        v = strtoull(p, &end, 0);
        p = end;
        if (*p == 'K' || *p == 'k')
            v <<= 10, p++;
        else if (*p == 'M' || *p == 'm')
            v <<= 20, p++;
    } else if (*p == '.' && !word_char(p[1])) {
        v = dot;
        p++;
    } else {
        const char *start = p;
        while ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '_' ||
               *p == '.' || *p == '$')
            p++;
        if (p == start)
            script_error(p, "an expression expected");
        size_t n = (size_t)(p - start);
        if (n == 5 && strncmp(start, "ALIGN", 5) == 0) {
            p = script_expect(p, '(');
            uint64_t a = script_value(&p), from = dot;
            p = script_skip(p);
            if (*p == ',') {
                p++;
                from = a;
                a = script_value(&p);
            }
            p = script_expect(p, ')');
            if (!a || (a & (a - 1)))
                script_error(start, "ALIGN of something that is not a power of two");
            v = align_up(from, a);
        } else {
            char name[256];
            if (n >= sizeof(name))
                script_error(start, "a name too long");
            memcpy(name, start, n);
            name[n] = 0;
            struct symbol *g = find_global(name);
            if (!g || !g->defined)
                script_error(start, "a symbol that is not defined");
            v = symbol_address(g);
        }
    }
    *pp = p;
    return v;
}

static int script_precedence(const char *p, int *len)
{
    *len = 1;
    if ((p[0] == '<' && p[1] == '<') || (p[0] == '>' && p[1] == '>')) {
        *len = 2;
        return 3;
    }
    switch (*p) {
    case '|': return 1;
    case '&': return 2;
    case '+': case '-': return 4;
    case '*': case '/': case '%': return 5;
    }
    return 0;
}

static uint64_t script_binary(const char **pp, int min)
{
    uint64_t v = script_primary(pp);
    for (;;) {
        const char *p = script_skip(*pp);
        int len, prec = script_precedence(p, &len);
        if (!prec || prec < min)
            break;
        char op = *p;
        p += len;
        uint64_t r = script_binary(&p, prec + 1);
        switch (op) {
        case '|': v |= r; break;
        case '&': v &= r; break;
        case '<': v <<= r; break;
        case '>': v >>= r; break;
        case '+': v += r; break;
        case '-': v -= r; break;
        case '*': v *= r; break;
        case '/': if (!r) script_error(p, "division by zero"); v /= r; break;
        case '%': if (!r) script_error(p, "division by zero"); v %= r; break;
        }
        *pp = p;
    }
    return v;
}

static uint64_t script_value(const char **pp)
{
    return script_binary(pp, 1);
}

static uint64_t script_eval(const char *text)
{
    const char *p = text;
    return script_value(&p);
}

/* --- reading the script */

/* `NAME = EXPR;`, `NAME += EXPR;` (the name already read) */
static void script_assignment(const char **pp, char *name, struct script_item *it)
{
    const char *p = script_skip(*pp);
    it->kind = SI_ASSIGN;
    it->sym = name;
    it->op = '=';
    if ((p[0] == '+' || p[0] == '-') && p[1] == '=') {
        it->op = p[0];
        p++;
    }
    p = script_expect(p, '=');
    it->expr = script_expr_text(&p, ';');
    *pp = p + 1;
    if (strcmp(name, ".") != 0) {
        if (it->op != '=')
            script_error(p, "only . may be changed with += or -=");
        struct symbol *g = global(name);
        if (g->defined && !g->synthetic)
            script_error(p, "a symbol the objects define too");
        g->defined = true;                      /* (its value comes with the layout) */
        g->synthetic = true;
        g->binding = STB_GLOBAL;
    }
}

static bool next_is_assignment(const char *p)
{
    p = script_skip(p);
    return p[0] == '=' || ((p[0] == '+' || p[0] == '-') && p[1] == '=');
}

/* *(PATTERNS) or KEEP(*(PATTERNS)), after the file pattern was read */
static void script_input(const char **pp, char *file, struct script_item *it)
{
    const char *p = *pp;
    bool keep = strcmp(file, "KEEP") == 0;
    p = script_expect(p, '(');
    if (keep)
        file = script_word(&p), p = script_expect(p, '(');
    if (strcmp(file, "*") != 0)
        script_error(p, "only * is supported as a file pattern");
    it->kind = SI_INPUT;
    for (;;) {
        p = script_skip(p);
        if (*p == ')')
            break;
        char *w = script_word(&p);
        if (strcmp(w, "COMMON") == 0) {
            it->in.common = true;
            continue;
        }
        it->in.patterns = realloc(it->in.patterns, sizeof(char *) * (size_t)(it->in.npatterns + 1));
        it->in.patterns[it->in.npatterns++] = w;
    }
    p++;
    if (keep)
        p = script_expect(p, ')');
    *pp = p;
}

static void read_script_text(const char *path, const char *text);

static void read_script(const char *path)
{
    size_t size;
    unsigned char *d = read_file(path, &size);
    char *text = alloc(size + 1);
    memcpy(text, d, size);
    read_script_text(path, text);
}

static void read_script_text(const char *path, const char *text)
{
    script_text = text;
    script_name = path;
    const char *p = text;
    bool have_sections = false;
    for (;;) {
        p = script_skip(p);
        if (!*p)
            break;
        if (*p == ';') {
            p++;
            continue;
        }
        char *w = script_word(&p);
        if (strcmp(w, "ENTRY") == 0) {
            p = script_expect(p, '(');
            char *e = script_word(&p);
            p = script_expect(p, ')');
            if (strcmp(e, "_start") != 0)
                script_error(p, "the entry point must be _start");
            continue;
        }
        if (strcmp(w, "OUTPUT_FORMAT") == 0 || strcmp(w, "OUTPUT_ARCH") == 0) {
            script_expr_text(&p, ')');
            p++;
            continue;
        }
        if (strcmp(w, "SECTIONS") != 0)
            script_error(p, "unknown command");
        have_sections = true;
        p = script_expect(p, '{');
        for (;;) {
            p = script_skip(p);
            if (*p == '}') {
                p++;
                break;
            }
            if (*p == ';') {
                p++;
                continue;
            }
            ss = realloc(ss, sizeof(*ss) * (size_t)(nss + 1));
            struct script_section *sec = &ss[nss++];
            memset(sec, 0, sizeof(*sec));
            char *name = script_word(&p);
            if (next_is_assignment(p)) {
                script_assignment(&p, name, &sec->assign);
                continue;
            }
            sec->name = name;
            sec->discard = strcmp(name, "/DISCARD/") == 0;
            p = script_skip(p);
            if (*p != ':' && *p != '(') {
                sec->addr = script_expr_text(&p, ':');
                p = script_skip(p);
            }
            if (*p == '(') {
                char *k = script_word(&(const char *){ p + 1 });
                if (strcmp(k, "NOLOAD") != 0)
                    script_error(p, "only (NOLOAD) is supported here");
                p = strchr(p, ')') + 1;
                sec->noload = true;
            }
            p = script_expect(p, ':');
            p = script_skip(p);
            if (strncmp(p, "ALIGN", 5) == 0 && script_skip(p + 5)[0] == '(') {
                p = script_skip(p + 5) + 1;
                sec->align = script_expr_text(&p, ')');
                p++;
            }
            p = script_expect(p, '{');
            for (;;) {
                p = script_skip(p);
                if (*p == '}') {
                    p++;
                    break;
                }
                if (*p == ';') {
                    p++;
                    continue;
                }
                sec->items = realloc(sec->items, sizeof(*sec->items) * (size_t)(sec->nitems + 1));
                struct script_item *it = &sec->items[sec->nitems++];
                memset(it, 0, sizeof(*it));
                char *iw = script_word(&p);
                if (next_is_assignment(p))
                    script_assignment(&p, iw, it);
                else
                    script_input(&p, iw, it);
            }
            p = script_skip(p);
            if (*p == '>')
                script_error(p, "memory regions are not supported");
        }
    }
    if (!have_sections)
        fail("%s: no SECTIONS", path);
}

static bool glob(const char *pat, const char *s)
{
    if (!*pat)
        return !*s;
    if (*pat == '*')
        return glob(pat + 1, s) || (*s && glob(pat, s + 1));
    if (*s && (*pat == '?' || *pat == *s))
        return glob(pat + 1, s + 1);
    return false;
}

static bool is_input_section(struct section *s)
{
    return s->type != SHT_GROUP && s->type != SHT_SYMTAB && s->type != SHT_STRTAB && s->type != SHT_RELA &&
           s->type != 9 /* SHT_REL */ && s->type != 0 && s->name && *s->name;
}

/* every input section to the first rule naming it */
static void script_place(void)
{
    for (int k = 0; k < nss; k++) {
        struct script_section *sec = &ss[k];
        for (int m = 0; sec->name && m < sec->nitems; m++) {
            struct script_item *it = &sec->items[m];
            if (it->kind != SI_INPUT)
                continue;
            for (int i = 0; i < nobjects; i++)
                for (int j = 1; j < objects[i]->nsections; j++) {
                    struct section *s = &objects[i]->sections[j];
                    if (s->placed || !is_input_section(s))
                        continue;
                    bool match = false;
                    for (int q = 0; q < it->in.npatterns && !match; q++)
                        match = glob(it->in.patterns[q], s->name);
                    if (!match)
                        continue;
                    s->placed = true;
                    if (sec->discard)
                        continue;
                    it->in.parts = realloc(it->in.parts, sizeof(struct section *) * (size_t)(it->in.nparts + 1));
                    it->in.parts[it->in.nparts++] = s;
                    s->out = &sec->out;
                }
        }
    }
    for (int i = 0; i < nobjects; i++)
        for (int j = 1; j < objects[i]->nsections; j++) {
            struct section *s = &objects[i]->sections[j];
            if (!s->placed && is_input_section(s) && (s->flags & SHF_ALLOC))
                fail("%s: section %s is not placed by %s", objects[i]->name, s->name, script_name);
        }
}

static struct output *common_out;


static void script_assign(struct script_item *it)
{
    uint64_t v = script_eval(it->expr);
    if (strcmp(it->sym, ".") == 0) {
        uint64_t to = it->op == '+' ? dot + v : it->op == '-' ? dot - v : v;
        if (to < dot && !(it->op == '=' && dot == 0))
            script_error(it->expr, ". may not move backwards");
        dot = to;
    } else
        find_global(it->sym)->value = v;
}

/* addresses, in the script's order */
static void script_layout(void)
{
    dot = 0;
    for (int k = 0; k < nss; k++) {
        struct script_section *sec = &ss[k];
        if (!sec->name) {
            script_assign(&sec->assign);
            continue;
        }
        if (sec->discard)
            continue;
        struct output *o = &sec->out;
        o->name = sec->name;
        o->type = SHT_NOBITS;
        o->flags = SHF_ALLOC;
        o->align = 1;
        for (int m = 0; m < sec->nitems; m++)
            for (int q = 0; q < sec->items[m].in.nparts; q++) {
                struct section *s = sec->items[m].in.parts[q];
                o->flags |= s->flags & (SHF_WRITE | SHF_EXECINSTR);
                if (s->type != SHT_NOBITS)
                    o->type = s->type == SHT_INIT_ARRAY || s->type == SHT_FINI_ARRAY ? s->type : SHT_PROGBITS;
                if (s->align > o->align)
                    o->align = s->align;
            }
        if (sec->noload)
            o->type = SHT_NOBITS;
        if (sec->addr)
            dot = script_eval(sec->addr);
        if (sec->align)
            dot = align_up(dot, script_eval(sec->align));
        dot = align_up(dot, o->align);
        o->addr = dot;
        for (int m = 0; m < sec->nitems; m++) {
            struct script_item *it = &sec->items[m];
            if (it->kind == SI_ASSIGN) {
                script_assign(it);
                continue;
            }
            for (int q = 0; q < it->in.nparts; q++) {
                struct section *s = it->in.parts[q];
                dot = align_up(dot, s->align ? s->align : 1);
                s->offset = dot - o->addr;
                dot += s->size;
            }
            if (it->in.common) {
                common_out = o;
                for (int h = 0; h < 16384; h++)
                    for (struct symbol *g = table[h]; g; g = g->next)
                        if (g->is_common) {
                            uint64_t a = g->value ? g->value : 8;
                            dot = align_up(dot, a);
                            g->value = dot;
                            dot += g->size;
                            if (a > o->align)
                                o->align = a;
                        }
            }
        }
        o->size = dot - o->addr;
    }
    for (int h = 0; h < 16384; h++)
        for (struct symbol *g = table[h]; g; g = g->next)
            if (g->is_common && !common_out)
                fail("%s: common symbol %s, but no *(COMMON) in %s", g->obj ? g->obj->name : "?", g->name, script_name);
}

static void link_with_script(const char *path, struct symbol *entry)
{
    read_script(path);
    script_place();
    scan_relocations();
    if (ngot || nplt || npairs || module_pair)
        fail("%s: the objects need a GOT or PLT, which a linker script's layout does not have (compile with "
             "-fno-pic, without per-thread data)", path);
    script_layout();
    /* the outputs that take space, in the script's order */
    struct output **outputs = alloc(sizeof(struct output *) * (size_t)(nss + 1));
    int nout = 0;
    for (int k = 0; k < nss; k++)
        if (ss[k].name && !ss[k].discard && ss[k].out.size)
            outputs[nout++] = &ss[k].out;
    for (int k = 0; k < nout; k++) {
        struct output *o = outputs[k];
        if (o->type == SHT_NOBITS)
            continue;
        o->data = alloc((size_t)o->size + 16);
        for (int m = 0; m < nss; m++)
            for (int it = 0; &ss[m].out == o && it < ss[m].nitems; it++)
                for (int q = 0; q < ss[m].items[it].in.nparts; q++) {
                    struct section *s = ss[m].items[it].in.parts[q];
                    if (s->data && s->type != SHT_NOBITS)
                        memcpy(o->data + s->offset, s->data, (size_t)s->size);
                }
    }
    apply_relocations(0);

    /* segments: one PT_LOAD for each run of the same permissions */
    nsegments = 0;
    uint64_t seg_file_end[16];
    for (int k = 0; k < nout; k++) {
        struct output *o = outputs[k];
        int flags = 4 | (o->flags & SHF_WRITE ? 2 : 0) | (o->flags & SHF_EXECINSTR ? 1 : 0);
        struct segment *g = nsegments ? &segments[nsegments - 1] : 0;
        if (!g || g->flags != flags || o->addr < g->vaddr + g->memsz) {
            if (nsegments == 15)
                fail("%s: too many segments", path);
            g = &segments[nsegments++];
            *g = (struct segment){ 1, flags, 0, o->addr, 0, 0, PAGE };
            seg_file_end[nsegments - 1] = o->addr;
        }
        g->memsz = o->addr + o->size - g->vaddr;
        if (o->type != SHT_NOBITS)
            seg_file_end[nsegments - 1] = o->addr + o->size;
        g->filesz = seg_file_end[nsegments - 1] - g->vaddr;
    }
    int nload = nsegments;
    segments[nsegments++] = (struct segment){ 0x6474E551, 6, 0, 0, 0, 0, 0 };   /* PT_GNU_STACK */
    uint64_t pos = 64 + 56 * (uint64_t)nsegments;
    for (int i = 0; i < nload; i++) {
        segments[i].offset = align_up(pos, PAGE) + (segments[i].vaddr & (PAGE - 1));
        pos = segments[i].offset + segments[i].filesz;
    }
    for (int k = 0; k < nout; k++) {
        struct output *o = outputs[k];
        for (int i = 0; i < nload; i++)
            if (o->addr >= segments[i].vaddr && o->addr + o->size <= segments[i].vaddr + segments[i].memsz)
                o->offset = segments[i].offset + (o->addr - segments[i].vaddr);
        o->index = k + 1;
    }

    /* the symbol table: the objects' local symbols, then the global ones */
    struct { char *data; size_t len, cap; } strtab = { 0 }, symtab = { 0 };
    strtab_add(&strtab.data, &strtab.len, &strtab.cap, "");
    unsigned char sym[24] = { 0 };
#define ADD_SYM(name, info, shndx, value, size)                                                                  \
    do {                                                                                                         \
        memset(sym, 0, 24);                                                                                      \
        put(sym, strtab_add(&strtab.data, &strtab.len, &strtab.cap, (name)), 4);                                 \
        sym[4] = (unsigned char)(info);                                                                          \
        put(sym + 6, (shndx), 2);                                                                                \
        put(sym + 8, (value), 8);                                                                                \
        put(sym + 16, (size), 8);                                                                                \
        if (symtab.len + 24 > symtab.cap) {                                                                      \
            symtab.cap = symtab.cap ? symtab.cap * 2 : 4096;                                                     \
            symtab.data = realloc(symtab.data, symtab.cap);                                                      \
        }                                                                                                        \
        memcpy(symtab.data + symtab.len, sym, 24);                                                               \
        symtab.len += 24;                                                                                        \
    } while (0)
    symtab.cap = 4096;
    symtab.data = alloc(symtab.cap);
    symtab.len = 24;                            /* (entry 0) */
    for (int i = 0; i < nobjects; i++)
        for (int j = 1; j < objects[i]->nsymbols; j++) {
            struct symbol *s = objects[i]->symbols[j];
            if (!s || s->binding != STB_LOCAL || s->type == STT_SECTION || s->type == 4 /* STT_FILE */ ||
                !s->name || !*s->name || strncmp(s->name, ".L", 2) == 0 || !s->section || !s->section->out)
                continue;
            ADD_SYM(s->name, STB_LOCAL << 4 | s->type, (uint64_t)s->section->out->index, symbol_address(s), s->size);
        }
    uint64_t first_global = symtab.len / 24;
    for (int h = 0; h < 16384; h++)
        for (struct symbol *g = table[h]; g; g = g->next) {
            if (!g->defined || g->shlib)
                continue;
            uint64_t shndx = 0xFFF1;            /* SHN_ABS: ld's own and absolute ones */
            if (g->is_common)
                shndx = (uint64_t)common_out->index;
            else if (!g->synthetic && !g->is_absolute) {
                if (!g->section || !g->section->out)
                    continue;
                shndx = (uint64_t)g->section->out->index;
            }
            ADD_SYM(g->name, (g->binding == STB_WEAK ? STB_WEAK : STB_GLOBAL) << 4 | g->type, shndx,
                    symbol_address(g), g->size);
        }
#undef ADD_SYM

    /* the file: headers, the segments, the symbol table, the section headers */
    shstrtab.len = 0;
    strtab_add(&shstrtab.data, &shstrtab.len, &shstrtab.cap, "");
    uint32_t *names = alloc(sizeof(uint32_t) * (size_t)(nout + 1));
    for (int k = 0; k < nout; k++)
        names[k] = strtab_add(&shstrtab.data, &shstrtab.len, &shstrtab.cap, outputs[k]->name);
    uint32_t symtab_name = strtab_add(&shstrtab.data, &shstrtab.len, &shstrtab.cap, ".symtab");
    uint32_t strtab_name = strtab_add(&shstrtab.data, &shstrtab.len, &shstrtab.cap, ".strtab");
    uint32_t shstrtab_name = strtab_add(&shstrtab.data, &shstrtab.len, &shstrtab.cap, ".shstrtab");
    uint64_t symtab_off = align_up(pos, 8), strtab_off = symtab_off + symtab.len;
    uint64_t shstr_off = strtab_off + strtab.len, shoff = align_up(shstr_off + shstrtab.len, 8);
    int nsh = nout + 4;
    uint64_t total = shoff + 64 * (uint64_t)nsh;
    unsigned char *file = alloc((size_t)total);
    memcpy(file, "\177ELF\2\1\1", 7);
    put(file + 16, 2, 2);                       /* ET_EXEC */
    put(file + 18, (uint64_t)machine, 2);
    put(file + 20, 1, 4);
    put(file + 24, symbol_address(entry), 8);
    put(file + 32, 64, 8);
    put(file + 40, shoff, 8);
    put(file + 52, 64, 2);
    put(file + 54, 56, 2);
    put(file + 56, (uint64_t)nsegments, 2);
    put(file + 58, 64, 2);
    put(file + 60, (uint64_t)nsh, 2);
    put(file + 62, (uint64_t)(nsh - 1), 2);
    for (int i = 0; i < nsegments; i++) {
        unsigned char *ph = file + 64 + 56 * (size_t)i;
        put(ph, (uint64_t)segments[i].type, 4);
        put(ph + 4, (uint64_t)segments[i].flags, 4);
        put(ph + 8, segments[i].offset, 8);
        put(ph + 16, segments[i].vaddr, 8);
        put(ph + 24, segments[i].vaddr, 8);
        put(ph + 32, segments[i].filesz, 8);
        put(ph + 40, segments[i].memsz, 8);
        put(ph + 48, segments[i].align, 8);
    }
    for (int k = 0; k < nout; k++)
        if (outputs[k]->type != SHT_NOBITS)
            memcpy(file + outputs[k]->offset, outputs[k]->data, (size_t)outputs[k]->size);
    memcpy(file + symtab_off, symtab.data, symtab.len);
    memcpy(file + strtab_off, strtab.data, strtab.len);
    memcpy(file + shstr_off, shstrtab.data, shstrtab.len);
    for (int k = 0; k < nout; k++) {
        struct output *o = outputs[k];
        unsigned char *h = file + shoff + 64 * (size_t)(k + 1);
        put(h, names[k], 4);
        put(h + 4, (uint64_t)o->type, 4);
        put(h + 8, o->flags, 8);
        put(h + 16, o->addr, 8);
        put(h + 24, o->offset, 8);
        put(h + 32, o->size, 8);
        put(h + 48, o->align, 8);
    }
    unsigned char *h = file + shoff + 64 * (size_t)(nout + 1);
    put(h, symtab_name, 4);
    put(h + 4, SHT_SYMTAB, 4);
    put(h + 24, symtab_off, 8);
    put(h + 32, symtab.len, 8);
    put(h + 40, (uint64_t)(nout + 2), 4);       /* sh_link: .strtab */
    put(h + 44, first_global, 4);               /* sh_info: the first global symbol */
    put(h + 48, 8, 8);
    put(h + 56, 24, 8);
    h += 64;
    put(h, strtab_name, 4);
    put(h + 4, SHT_STRTAB, 4);
    put(h + 24, strtab_off, 8);
    put(h + 32, strtab.len, 8);
    put(h + 48, 1, 8);
    h += 64;
    put(h, shstrtab_name, 4);
    put(h + 4, SHT_STRTAB, 4);
    put(h + 24, shstr_off, 8);
    put(h + 32, shstrtab.len, 8);
    put(h + 48, 1, 8);
    unlink(output);
    int fd = open(output, O_WRONLY | O_CREAT | O_TRUNC, 0755);
    if (fd < 0)
        fail("cannot write %s", output);
    for (uint64_t at = 0; at < total;) {
        ssize_t w = write(fd, file + at, (size_t)(total - at));
        if (w <= 0)
            fail("cannot write %s", output);
        at += (uint64_t)w;
    }
    if (close(fd) != 0)
        fail("cannot write %s", output);
}

/* --- UEFI applications (62g) --------------------------------------------- */

/* ld --efi: the objects as a PE32+ image, laid out as lld-link lays out a
 * UEFI application: ImageBase 0x140000000, .text at RVA 0x1000, then
 * .rdata, .data (with .bss after its file bytes) and .reloc, each on its
 * own page; 512-byte file alignment; the entry point efi_main. The code is
 * PC-relative (cc -fno-pic), so only 64-bit addresses in data need base
 * relocations (DIR64), which the firmware applies wherever it loads us. */

#define EFI_BASE 0x140000000ULL

static const char efi_script[] =
    "SECTIONS {\n"
    "  . = 0x140001000;\n"
    "  .text : { *(.text .text.*) }\n"
    "  .rdata : ALIGN(4096) { *(.rodata .rodata.* .data.rel.ro .data.rel.ro.*) }\n"
    "  .data : ALIGN(4096) { *(.data .data.*) *(.bss .bss.*) *(COMMON) }\n"
    "  /DISCARD/ : { *(.comment) *(.note .note.*) *(.eh_frame .eh_frame.*) *(.llvm_addrsig) }\n"
    "}\n";

static int compare_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

static void link_efi(void)
{
    struct symbol *entry = find_global("efi_main");
    if (!entry || !entry->defined)
        fail("--efi: no efi_main (the UEFI application's entry point)");
    read_script_text("(--efi)", efi_script);
    script_place();
    scan_relocations();
    if (ngot || nplt || npairs || module_pair)
        fail("--efi: the objects need a GOT or PLT (compile with -fno-pic, without per-thread data)");
    script_layout();
    struct output *outs3[3];
    int nout = 0;
    for (int k = 0; k < nss; k++)
        if (ss[k].name && !ss[k].discard)
            outs3[nout++] = &ss[k].out;
    /* contents, and how much of each the file holds (.bss is not in it) */
    uint64_t file_size[3] = { 0 };
    for (int k = 0; k < nout; k++) {
        struct output *o = outs3[k];
        o->data = alloc((size_t)o->size + 16);
        for (int m = 0; m < nss; m++)
            for (int it = 0; &ss[m].out == o && it < ss[m].nitems; it++)
                for (int q = 0; q < ss[m].items[it].in.nparts; q++) {
                    struct section *s = ss[m].items[it].in.parts[q];
                    if (s->type == SHT_NOBITS)
                        continue;
                    if (s->data)
                        memcpy(o->data + s->offset, s->data, (size_t)s->size);
                    if (s->offset + s->size > file_size[k])
                        file_size[k] = s->offset + s->size;
                }
    }
    efi_out = true;
    apply_relocations(0);
    if (outs3[0]->size == 0)
        fail("--efi: no code");

    /* .reloc: a block per 4 KiB page, DIR64 entries, each block 4-byte aligned */
    qsort(abs_at, (size_t)nabs_at, sizeof(uint64_t), compare_u64);
    unsigned char *reloc = alloc(16 + 12 * (size_t)nabs_at);
    uint64_t rsize = 0;
    for (int i = 0; i < nabs_at;) {
        uint64_t page = (abs_at[i] - EFI_BASE) & ~(uint64_t)0xFFF;
        uint64_t start = rsize;
        rsize += 8;
        int n = 0;
        for (; i < nabs_at && ((abs_at[i] - EFI_BASE) & ~(uint64_t)0xFFF) == page; i++, n++) {
            put(reloc + rsize, 10u << 12 | ((abs_at[i] - EFI_BASE) & 0xFFF), 2);   /* IMAGE_REL_BASED_DIR64 */
            rsize += 2;
        }
        if (n & 1) {
            put(reloc + rsize, 0, 2);           /* (IMAGE_REL_BASED_ABSOLUTE: padding) */
            rsize += 2;
        }
        put(reloc + start, page, 4);
        put(reloc + start + 4, rsize - start, 4);
    }
    if (!rsize) {
        /* (an empty block: some loaders want the directory to be there) */
        put(reloc, 0, 4);
        put(reloc + 4, 8, 4);
        rsize = 8;
    }

    /* the file: headers, then each section's bytes */
    static const char *names[4] = { ".text", ".rdata", ".data", ".reloc" };
    static const uint32_t kinds[4] = { 0x60000020, 0x40000040, 0xC0000040, 0x42000040 };
    uint64_t rva[4], vsize[4], raw[4], rawp[4];
    const unsigned char *bytes[4];
    for (int k = 0; k < 3; k++) {
        rva[k] = outs3[k]->addr - EFI_BASE;
        vsize[k] = outs3[k]->size;
        raw[k] = align_up(file_size[k], 0x200);
        bytes[k] = outs3[k]->data;
    }
    rva[3] = align_up(rva[2] + (vsize[2] ? vsize[2] : 1), 0x1000);
    vsize[3] = rsize;
    raw[3] = align_up(rsize, 0x200);
    bytes[3] = reloc;
    int nsec = 4;
    uint64_t headers = align_up(0x40 + 4 + 20 + 240 + 40 * (uint64_t)nsec, 0x200), at = headers;
    for (int k = 0; k < nsec; k++) {
        rawp[k] = raw[k] ? at : 0;
        at += raw[k];
    }
    uint64_t total = at, image = align_up(rva[3] + vsize[3], 0x1000);
    unsigned char *file = alloc((size_t)total);
    file[0] = 'M';
    file[1] = 'Z';
    put(file + 0x3C, 0x40, 4);                  /* e_lfanew */
    unsigned char *pe = file + 0x40;
    memcpy(pe, "PE\0\0", 4);
    unsigned char *coff = pe + 4;
    put(coff, machine == EM_X86_64 ? 0x8664 : 0xAA64, 2);
    put(coff + 2, (uint64_t)nsec, 2);
    put(coff + 16, 240, 2);                     /* SizeOfOptionalHeader */
    put(coff + 18, 0x22, 2);                    /* EXECUTABLE_IMAGE | LARGE_ADDRESS_AWARE */
    unsigned char *opt = coff + 20;
    put(opt, 0x20B, 2);                         /* PE32+ */
    put(opt + 4, raw[0], 4);                    /* SizeOfCode */
    put(opt + 8, raw[1] + raw[2] + raw[3], 4);  /* SizeOfInitializedData */
    put(opt + 16, symbol_address(entry) - EFI_BASE, 4);
    put(opt + 20, rva[0], 4);                   /* BaseOfCode */
    put(opt + 24, EFI_BASE, 8);
    put(opt + 32, 0x1000, 4);                   /* SectionAlignment */
    put(opt + 36, 0x200, 4);                    /* FileAlignment */
    put(opt + 40, 6, 2);                        /* (operating system and subsystem 6.0, as lld) */
    put(opt + 48, 6, 2);
    put(opt + 56, image, 4);                    /* SizeOfImage */
    put(opt + 60, headers, 4);                  /* SizeOfHeaders */
    put(opt + 68, 10, 2);                       /* IMAGE_SUBSYSTEM_EFI_APPLICATION */
    put(opt + 70, 0x8160, 2);                   /* (DllCharacteristics as lld writes them) */
    put(opt + 72, 0x100000, 8);                 /* stack and heap, reserved and committed */
    put(opt + 80, 0x1000, 8);
    put(opt + 88, 0x100000, 8);
    put(opt + 96, 0x1000, 8);
    put(opt + 108, 16, 4);                      /* NumberOfRvaAndSizes */
    put(opt + 112 + 8 * 5, rva[3], 4);          /* the base relocation table */
    put(opt + 112 + 8 * 5 + 4, rsize, 4);
    unsigned char *sh = opt + 240;
    for (int k = 0; k < nsec; k++, sh += 40) {
        memcpy(sh, names[k], strlen(names[k]));
        put(sh + 8, vsize[k], 4);
        put(sh + 12, rva[k], 4);
        put(sh + 16, raw[k], 4);
        put(sh + 20, rawp[k], 4);
        put(sh + 36, kinds[k], 4);
        if (raw[k])
            memcpy(file + rawp[k], bytes[k], (size_t)(k == 3 ? rsize : file_size[k]));
    }
    unlink(output);
    int fd = open(output, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        fail("cannot write %s", output);
    for (uint64_t w = 0; w < total;) {
        ssize_t n = write(fd, file + w, (size_t)(total - w));
        if (n <= 0)
            fail("cannot write %s", output);
        w += (uint64_t)n;
    }
    if (close(fd) != 0)
        fail("cannot write %s", output);
}

int main(int argc, char **argv)
{
    char *dirs[64];
    int ndirs = 0;
    char **inputs = alloc(sizeof(char *) * (size_t)(argc + 1));
    int ninputs = 0;
    bool *needed_mode = alloc(sizeof(bool) * (size_t)(argc + 1));
    bool pie = false, static_link = false, shared = false, no_dynamic_linker = false, efi_mode = false;
    const char *script = 0;
    for (int i = 1; i < argc; i++) {
        char *a = argv[i];
        if (strcmp(a, "-o") == 0 && i + 1 < argc)
            output = argv[++i];
        else if (strncmp(a, "-L", 2) == 0) {
            const char *d = a[2] ? a + 2 : (i + 1 < argc ? argv[++i] : 0);
            if (d && ndirs < 64)
                dirs[ndirs++] = (char *)d;
        } else if (strcmp(a, "-dynamic-linker") == 0 && i + 1 < argc)
            interp = argv[++i];
        else if (strcmp(a, "--as-needed") == 0)
            as_needed = true;
        else if (strcmp(a, "--no-as-needed") == 0)
            as_needed = false;
        else if (strcmp(a, "-pie") == 0)
            pie = true;
        else if (strcmp(a, "--hash-style=sysv") == 0 || strcmp(a, "-nostdlib") == 0 || strcmp(a, "-Bstatic") == 0 ||
                 strcmp(a, "-Bdynamic") == 0)
            ;                                   /* (what ld always does) */
        else if (strcmp(a, "-static") == 0)
            static_link = true;
        else if (strcmp(a, "-shared") == 0)
            shared = true;
        else if (strcmp(a, "--efi") == 0)
            efi_mode = true;                    /* (62g) a UEFI application (PE32+) */
        else if (strcmp(a, "--no-dynamic-linker") == 0)
            no_dynamic_linker = true;
        else if (strcmp(a, "-soname") == 0 && i + 1 < argc)
            soname = argv[++i];
        else if (strncmp(a, "-soname=", 8) == 0)
            soname = a + 8;
        else if (strncmp(a, "--version-script=", 17) == 0)
            read_version_script(a + 17);
        else if (strcmp(a, "--version-script") == 0 && i + 1 < argc)
            read_version_script(argv[++i]);
        else if (strcmp(a, "-T") == 0 && i + 1 < argc)
            script = argv[++i];
        else if (strncmp(a, "-T", 2) == 0 && a[2])
            script = a + 2;
        else if (strcmp(a, "-z") == 0 && i + 1 < argc) {
            const char *z = argv[++i];
            if (strcmp(z, "norelro") == 0)
                relro = false;
            else if (strcmp(z, "now") && strcmp(z, "relro") && strcmp(z, "nocopyreloc") && strcmp(z, "defs") &&
                     strncmp(z, "max-page-size=", 14))
                fail("unsupported -z %s", z);
        } else if (a[0] == '-' && a[1] == 'l') {
            needed_mode[ninputs] = as_needed;
            inputs[ninputs++] = a;
        } else if (a[0] == '-')
            fail("unknown option %s", a);
        else {
            needed_mode[ninputs] = as_needed;
            inputs[ninputs++] = a;
        }
    }
    if (!ninputs)
        fail("no input files");
    /* (62d) the kind of output, and the layouts our ld has built in */
    mode = shared ? MODE_SHARED : no_dynamic_linker ? MODE_LDSO : static_link && !pie ? MODE_STATIC : MODE_PIE;
    base = mode == MODE_SHARED || mode == MODE_LDSO ? 0 : BASE;
    bool own_script = false;                    /* (62f) a script read, not one of the built-in layouts */
    if (script) {
        const char *b = strrchr(script, '/') ? strrchr(script, '/') + 1 : script;
        own_script = strcmp(b, "user.ld") != 0 && strcmp(b, "ldso.ld") != 0;
        if (own_script && mode != MODE_STATIC)
            fail("%s: a linker script's layout is for -static links", script);
    }
    for (int i = 0; i < ninputs; i++) {
        struct shlib *before = shlibs_tail;
        add_input(inputs[i], dirs, ndirs);
        /* a library given without --as-needed is always needed */
        if (shlibs_tail != before && !needed_mode[i])
            shlibs_tail->needed = true;
    }
    load_members();
    if (efi_mode) {
        mode = MODE_STATIC;                     /* (every address final when linking) */
        link_efi();
        return 0;
    }
    if (!interp)
        interp = machine == EM_AARCH64 ? "/system/aarch64/lib/ld.so" : "/system/x86_64/lib/ld.so";
    struct symbol *entry = find_global("_start");
    if (mode != MODE_SHARED && (!entry || !entry->defined || entry->shlib))
        fail("no _start (the start code is missing)");
    /* (62d) symbols ld defines: the headers' address, the dynamic section */
    const char *synthetic[] = { "__ehdr_start", "_DYNAMIC", "_GLOBAL_OFFSET_TABLE_" };
    for (int i = 0; i < 3; i++) {
        struct symbol *g = find_global(synthetic[i]);
        if (g && !g->defined) {
            g->defined = true;
            g->synthetic = true;
            g->binding = STB_GLOBAL;
            g->hidden = true;
        }
    }

    if (own_script) {
        link_with_script(script, entry);
        return 0;
    }
    /* input sections into output sections */
    for (int i = 0; i < nobjects; i++)
        for (int j = 1; j < objects[i]->nsections; j++) {
            struct section *s = &objects[i]->sections[j];
            int k = output_of(s);
            if (k >= 0)
                add_part(&outs[k], s);
        }
    /* common symbols at the end of .bss */
    scan_relocations();
    if (mode == MODE_SHARED) {
        /* (62d) the library's interface: its version script's names */
        for (int i = 0; i < nexports; i++) {
            struct symbol *g = find_global(exports[i]);
            if (!g || !g->defined || g->shlib)
                fail("the version script names %s, which is not defined", exports[i]);
        }
        for (int h = 0; h < 16384; h++)
            for (struct symbol *g = table[h]; g; g = g->next)
                if (g->defined && !g->shlib && !g->hidden && !g->synthetic && g->binding != STB_LOCAL &&
                    exported_name(g->name)) {
                    g->exported = true;
                    need_dynsym(g);
                }
    }
    if (mode == MODE_STATIC && machine == EM_AARCH64 && npairs) {
        static unsigned char code[8] = { 0x00, 0x04, 0x40, 0xF9, 0xC0, 0x03, 0x5F, 0xD6 };
        desc_stub = alloc(sizeof(*desc_stub));
        desc_stub->obj = objects[0];
        desc_stub->name = ".text";
        desc_stub->type = SHT_PROGBITS;
        desc_stub->flags = SHF_ALLOC | SHF_EXECINSTR;
        desc_stub->size = 8;
        desc_stub->align = 4;
        desc_stub->data = code;
        add_part(&outs[O_TEXT], desc_stub);
    }
    if (mode == MODE_LDSO) {
        /* (ldso.ld: .text.start first) */
        struct output *t = &outs[O_TEXT];
        int at = 0;
        for (int i = 0; i < t->nparts; i++)
            if (strcmp(t->parts[i]->name, ".text.start") == 0) {
                struct section *x = t->parts[i];
                memmove(t->parts + at + 1, t->parts + at, sizeof(*t->parts) * (size_t)(i - at));
                t->parts[at++] = x;
            }
        if (outs[O_TDATA].nparts || outs[O_TBSS].nparts) {
            struct output *t = outs[O_TDATA].nparts ? &outs[O_TDATA] : &outs[O_TBSS];
            fail("%s: per-thread data in ld.so (it runs on the program's threads)", t->parts[0]->obj->name);
        }
    }

    /* the dynamic tables' sizes */
    dynstr_add("");
    for (struct shlib *l = shlibs; l; l = l->next)
        if (l->needed)
            dynstr_add(l->soname);
    uint32_t soname_at = soname ? dynstr_add(soname) : 0;
    uint32_t *dyn_names = alloc(sizeof(uint32_t) * (size_t)(ndyn + 1));
    for (int i = 0; i < ndyn; i++)
        dyn_names[i] = dynstr_add(dyn_list[i]->name);
    int nbucket = ndyn < 8 ? 1 : ndyn / 2 + 1;
    outs[O_INTERP].size = mode == MODE_PIE ? strlen(interp) + 1 : 0;
    outs[O_DYNSYM].size = 24 * (uint64_t)(ndyn + 1);
    outs[O_HASH].size = 4 * (2 + (uint64_t)nbucket + (uint64_t)(ndyn + 1));
    outs[O_PLT].size = 16 * (uint64_t)nplt;
    outs[O_GOT].size = 8 * (uint64_t)ngot + 16 * (uint64_t)npairs + (module_pair ? 16 : 0);
    outs[O_GOT_PLT].size = 8 * (uint64_t)nplt;
    outs[O_RELA_PLT].size = 24 * (uint64_t)nplt;
    int ndynamic = 0;
    for (struct shlib *l = shlibs; l; l = l->next)
        ndynamic += l->needed;
    ndynamic += 16;                             /* (the fixed entries, room to spare) */
    outs[O_DYNAMIC].size = 16 * (uint64_t)ndynamic;
    if (mode == MODE_STATIC) {
        /* (62d) a static program: no loader's tables */
        int none[] = { O_HASH, O_DYNSYM, O_DYNSTR, O_RELA_DYN, O_RELA_PLT, O_PLT, O_GOT_PLT, O_DYNAMIC };
        for (size_t k = 0; k < sizeof(none) / sizeof(none[0]); k++)
            outs[none[k]].size = 0;
    }

    /* addresses: user/user.ld's order, a page for each kind */
    int order[] = { O_INTERP, O_HASH, O_DYNSYM, O_DYNSTR, O_RELA_DYN, O_RELA_PLT, -1, O_TEXT, O_PLT, -1,
                    O_RODATA, -1, O_TDATA, O_TBSS, O_DATA_REL_RO, O_INIT_ARRAY, O_FINI_ARRAY, O_DYNAMIC, O_GOT,
                    O_GOT_PLT, -2, O_DATA, O_BSS };
    bool has_tls = outs[O_TDATA].nparts || outs[O_TBSS].nparts;
    int nphdrs = 10 + has_tls;                  /* (62b: PT_TLS only with per-thread data) */
    uint64_t header_size = 64 + 56 * (uint64_t)nphdrs;
    uint64_t addr = base + header_size;
    /* .rela.dyn: at most one entry for each 64-bit address in loaded data
     * and each GOT entry (fewer when some need none) */
    outs[O_RELA_DYN].size = 24 * (nabsolute + (uint64_t)ngot + 2 * (uint64_t)npairs + (module_pair ? 1 : 0));
    outs[O_DYNSTR].size = mode == MODE_STATIC ? 0 : dynstr.len;
    if (mode == MODE_STATIC)
        outs[O_RELA_DYN].size = 0;
    uint64_t relro_start = 0, relro_end = 0;
    for (size_t k = 0; k < sizeof(order) / sizeof(order[0]); k++) {
        if (order[k] == -1) {
            addr = align_up(addr, PAGE);
            continue;
        }
        if (order[k] == -2) {
            relro_end = align_up(addr, PAGE);
            addr = relro_end;
            continue;
        }
        struct output *o = &outs[order[k]];
        if (order[k] == O_TDATA)
            relro_start = addr;                 /* (the TLS template leads RELRO, as user.ld) */
        uint64_t size = 0;
        for (int p = 0; p < o->nparts; p++) {
            struct section *s = o->parts[p];
            size = align_up(size, s->align ? s->align : 1);
            s->offset = size;
            size += s->size;
        }
        if (order[k] == O_BSS) {
            /* common symbols */
            for (int h = 0; h < 16384; h++)
                for (struct symbol *g = table[h]; g; g = g->next)
                    if (g->is_common) {
                        uint64_t a = g->value ? g->value : 8;
                        size = align_up(size, a);
                        g->value = size;        /* (made an address below) */
                        size += g->size;
                        if (a > o->align)
                            o->align = a;
                    }
        }
        if (o->nparts || order[k] == O_BSS)
            o->size = size + (order[k] == O_BSS ? 0 : 0);
        addr = align_up(addr, o->align ? o->align : 1);
        o->addr = addr;
        if (order[k] != O_TBSS)                 /* (.tbss: in the TLS template only) */
            addr += o->size;
        if (order[k] == O_BSS)
            for (int h = 0; h < 16384; h++)
                for (struct symbol *g = table[h]; g; g = g->next)
                    if (g->is_common)
                        g->value += o->addr;
    }
    uint64_t end = addr;
    /* contents */
    for (int k = 0; k < O_COUNT; k++) {
        struct output *o = &outs[k];
        if (o->type == SHT_NOBITS || !(o->flags & SHF_ALLOC))
            continue;                           /* (.ritteros.help comes later) */
        o->data = alloc((size_t)o->size + 16);
        for (int p = 0; p < o->nparts; p++)
            if (o->parts[p]->data)
                memcpy(o->data + o->parts[p]->offset, o->parts[p]->data, (size_t)o->parts[p]->size);
    }
    if (mode == MODE_PIE)
        memcpy(outs[O_INTERP].data, interp, strlen(interp) + 1);
    /* (62d) ld's own symbols */
    {
        struct symbol *g = find_global("__ehdr_start");
        if (g && g->synthetic)
            g->value = base;
        g = find_global("_DYNAMIC");
        if (g && g->synthetic)
            g->value = outs[O_DYNAMIC].addr;
        /* (the GOT, as lld: in RELRO, which mmaptest checks by writing it) */
        g = find_global("_GLOBAL_OFFSET_TABLE_");
        if (g && g->synthetic)
            g->value = outs[O_GOT_PLT].size ? outs[O_GOT_PLT].addr : outs[O_GOT].size ? outs[O_GOT].addr : relro_start;
    }
    if (has_tls) {
        struct output *td = &outs[O_TDATA], *tb = &outs[O_TBSS];
        tls_start = td->nparts ? td->addr : tb->addr;
        uint64_t tls_end = tb->nparts ? tb->addr + tb->size : td->addr + td->size;
        tls_memsz = tls_end - tls_start;
        tls_align = td->align > tb->align ? td->align : tb->align;
        if (!tls_align)
            tls_align = 1;
    }
    /* (62b) TLS pairs: filled by ld.so (symbol 0: this program's own block);
     * (62d) a static program's at once: module 1, or (AArch64) ld's own
     * resolver and the offset from the thread pointer, as threaddata.c's
     * static start lays the block out (16 bytes, aligned, then the block) */
    for (int i = 0; i < npairs; i++) {
        struct symbol *t = pair_list[i];
        uint64_t at = pair_address(t);
        unsigned char *d = outs[O_GOT].data + (at - outs[O_GOT].addr);
        if (mode == MODE_STATIC) {
            if (machine == EM_X86_64) {
                put(d, 1, 8);
                put(d + 8, tls_offset(t), 8);
            } else {
                put(d, desc_stub->out->addr + desc_stub->offset, 8);
                put(d + 8, align_up(16, tls_align) + tls_offset(t), 8);
            }
            continue;
        }
        if (machine == EM_X86_64) {
            add_dynrel(at, X_DTPMOD64, imported(t) ? t : 0, 0);
            if (imported(t))
                add_dynrel(at + 8, X_DTPOFF64, t, 0);
            else
                put(d + 8, tls_offset(t), 8);
        } else
            add_dynrel(at, A_TLSDESC, imported(t) ? t : 0, imported(t) ? 0 : (int64_t)tls_offset(t));
    }
    if (module_pair && mode == MODE_STATIC)
        put(outs[O_GOT].data + (module_pair_address() - outs[O_GOT].addr), 1, 8);
    else if (module_pair)
        add_dynrel(module_pair_address(), X_DTPMOD64, 0, 0);
    /* GOT entries: imported ones bound by ld.so, the others relocated */
    for (int i = 0; i < ngot; i++) {
        struct symbol *s = got_list[i];
        uint64_t at = outs[O_GOT].addr + 8 * (uint64_t)i;
        if (imported(s))
            add_dynrel(at, machine == EM_X86_64 ? X_GLOB_DAT : A_GLOB_DAT, s, 0);
        else if (s->defined && !s->is_absolute) {
            put(outs[O_GOT].data + 8 * i, symbol_address(s), 8);
            if (mode != MODE_STATIC)
                add_dynrel(at, machine == EM_X86_64 ? X_RELATIVE : A_RELATIVE, 0, (int64_t)symbol_address(s));
        } else
            put(outs[O_GOT].data + 8 * i, symbol_address(s), 8);
    }
    apply_relocations(0);
    /* the PLT: a jump through each slot */
    for (int i = 0; i < nplt; i++) {
        unsigned char *e = outs[O_PLT].data + 16 * i;
        uint64_t at = outs[O_PLT].addr + 16 * (uint64_t)i, slot = plt_slot(plt_list[i]);
        if (machine == EM_X86_64) {
            /* jmp *slot(%rip), then int3 */
            e[0] = 0xFF;
            e[1] = 0x25;
            put(e + 2, slot - (at + 6), 4);
            memset(e + 6, 0xCC, 10);
        } else {
            /* adrp x16, slot; ldr x17, [x16, #lo12]; br x17; nop */
            int64_t pages = (int64_t)((slot & ~(uint64_t)0xFFF) - (at & ~(uint64_t)0xFFF)) >> 12;
            put(e, 0x90000010u | ((uint32_t)pages & 3) << 29 | (((uint32_t)pages >> 2) & 0x7FFFF) << 5, 4);
            put(e + 4, 0xF9400211u | (uint32_t)((slot & 0xFFF) >> 3) << 10, 4);
            put(e + 8, 0xD61F0220u, 4);
            put(e + 12, 0xD503201Fu, 4);
        }
        unsigned char *r = outs[O_RELA_PLT].data + 24 * i;
        put(r, slot, 8);
        put(r + 8, (uint64_t)plt_list[i]->dynsym << 32 | (uint32_t)(machine == EM_X86_64 ? X_JUMP_SLOT : A_JUMP_SLOT), 8);
        put(r + 16, 0, 8);
    }
    /* .dynsym and .hash */
    for (int i = 0; i < ndyn; i++) {
        unsigned char *s = outs[O_DYNSYM].data + 24 * (i + 1);
        struct symbol *g = dyn_list[i];
        put(s, dyn_names[i], 4);
        s[4] = (unsigned char)((g->binding == STB_WEAK ? STB_WEAK : STB_GLOBAL) << 4 | (g->type == STT_FUNC ? STT_FUNC : g->type));
        if (g->exported) {
            /* (62d) a library's own: its address (the section index comes below) */
            put(s + 8, symbol_address(g), 8);
            put(s + 16, g->size, 8);
        }
    }
    {
        unsigned char *h = outs[O_HASH].data;
        put(h, (uint64_t)nbucket, 4);
        put(h + 4, (uint64_t)(ndyn + 1), 4);
        unsigned char *buckets = h + 8, *chains = h + 8 + 4 * (size_t)nbucket;
        for (int i = 1; i <= ndyn; i++) {
            uint32_t b = elf_hash(dyn_list[i - 1]->name) % (uint32_t)nbucket;
            put(chains + 4 * (size_t)i, get(buckets + 4 * b, 4), 4);
            put(buckets + 4 * b, (uint64_t)i, 4);
        }
    }
    memcpy(outs[O_DYNSTR].data, dynstr.data, dynstr.len);
    /* .rela.dyn: RELATIVE ones first (in address order), then the others */
    {
        unsigned char *r = outs[O_RELA_DYN].data;
        int k = 0;
        for (int pass = 0; pass < 2; pass++)
            for (int i = 0; i < ndynrels; i++) {
                bool relative = dynrels[i].sym == 0;
                if (relative != (pass == 0))
                    continue;
                put(r + 24 * (size_t)k, dynrels[i].offset, 8);
                put(r + 24 * (size_t)k + 8, (uint64_t)(dynrels[i].sym ? dynrels[i].sym->dynsym : 0) << 32 | (uint32_t)dynrels[i].type, 8);
                put(r + 24 * (size_t)k + 16, (uint64_t)dynrels[i].addend, 8);
                k++;
            }
        outs[O_RELA_DYN].size = 24 * (uint64_t)k;
    }
    /* .dynamic */
    if (mode != MODE_STATIC) {
        unsigned char *d = outs[O_DYNAMIC].data;
        int k = 0;
#define DYN(tag, val) (put(d + 16 * k, (uint64_t)(tag), 8), put(d + 16 * k + 8, (uint64_t)(val), 8), k++)
        uint32_t name_at = 1;
        for (struct shlib *l = shlibs; l; l = l->next)
            if (l->needed) {
                DYN(1, name_at);                /* DT_NEEDED */
                name_at += (uint32_t)strlen(l->soname) + 1;
            }
        DYN(4, outs[O_HASH].addr);              /* DT_HASH */
        DYN(5, outs[O_DYNSTR].addr);            /* DT_STRTAB */
        DYN(6, outs[O_DYNSYM].addr);            /* DT_SYMTAB */
        DYN(10, dynstr.len);                    /* DT_STRSZ */
        DYN(11, 24);                            /* DT_SYMENT */
        if (outs[O_RELA_DYN].size) {
            DYN(7, outs[O_RELA_DYN].addr);      /* DT_RELA */
            DYN(8, outs[O_RELA_DYN].size);      /* DT_RELASZ */
            DYN(9, 24);                         /* DT_RELAENT */
        }
        if (nplt) {
            DYN(23, outs[O_RELA_PLT].addr);     /* DT_JMPREL */
            DYN(2, outs[O_RELA_PLT].size);      /* DT_PLTRELSZ */
            DYN(20, 7);                         /* DT_PLTREL: RELA */
            DYN(3, outs[O_GOT_PLT].addr);       /* DT_PLTGOT */
        }
        if (outs[O_INIT_ARRAY].size) {
            DYN(25, outs[O_INIT_ARRAY].addr);
            DYN(27, outs[O_INIT_ARRAY].size);
        }
        if (outs[O_FINI_ARRAY].size) {
            DYN(26, outs[O_FINI_ARRAY].addr);
            DYN(28, outs[O_FINI_ARRAY].size);
        }
        if (soname)
            DYN(14, soname_at);                 /* DT_SONAME */
        DYN(30, 8);                             /* DT_FLAGS: BIND_NOW */
        DYN(0x6FFFFFFB, mode == MODE_PIE ? 0x08000001 : 1);  /* DT_FLAGS_1: NOW (| PIE) */
        DYN(0, 0);
        outs[O_DYNAMIC].size = 16 * (uint64_t)k;
#undef DYN
    }
    /* segments */
    nsegments = 0;
#define SEG(t, f, start, fsz, msz, al)                                                                                 \
    (segments[nsegments++] = (struct segment){ (t), (f), (start) - base, (start), (fsz), (msz), (al) })
    uint64_t first_end = outs[O_RELA_PLT].addr + outs[O_RELA_PLT].size;
    if (mode == MODE_PIE) {
        SEG(6, 4, base + 64, 56 * (uint64_t)nphdrs, 56 * (uint64_t)nphdrs, 8);          /* PT_PHDR */
        SEG(3, 4, outs[O_INTERP].addr, outs[O_INTERP].size, outs[O_INTERP].size, 1);    /* PT_INTERP */
    }
    SEG(1, 4, base, first_end - base, first_end - base, PAGE);
    uint64_t text_end = outs[O_PLT].addr + outs[O_PLT].size;
    if (text_end > outs[O_TEXT].addr)
        SEG(1, 5, outs[O_TEXT].addr, text_end - outs[O_TEXT].addr, text_end - outs[O_TEXT].addr, PAGE);
    if (outs[O_RODATA].size)
        SEG(1, 4, outs[O_RODATA].addr, outs[O_RODATA].size, outs[O_RODATA].size, PAGE);
    SEG(1, 6, relro_start, relro_end - relro_start, relro_end - relro_start, PAGE);
    uint64_t data_start = relro_end, data_file_end = outs[O_DATA].addr + outs[O_DATA].size;
    if (end > data_start)
        SEG(1, 6, data_start, (outs[O_DATA].size ? data_file_end : data_start) - data_start, end - data_start, PAGE);
    if (mode != MODE_STATIC)
        SEG(2, 6, outs[O_DYNAMIC].addr, outs[O_DYNAMIC].size, outs[O_DYNAMIC].size, 8); /* PT_DYNAMIC */
    if (has_tls)
        SEG(7, 4, tls_start, outs[O_TDATA].size, tls_memsz, tls_align);                 /* PT_TLS */
    if (relro && mode != MODE_STATIC)
        SEG(0x6474E552, 4, relro_start, relro_end - relro_start, relro_end - relro_start, 1); /* PT_GNU_RELRO */
    segments[nsegments++] = (struct segment){ 0x6474E551, 6, 0, 0, 0, 0, 0 };          /* PT_GNU_STACK */
#undef SEG
    if (nsegments > nphdrs)
        fail("internal error: too many segments");
    /* the file: everything at offset = address - BASE, then the
     * non-loaded sections and the section headers */
    uint64_t file_end = data_start - base;
    if (outs[O_DATA].size)
        file_end = data_file_end - base;
    uint64_t help_offset = file_end;
    outs[O_HELP].size = 0;
    for (int p = 0; p < outs[O_HELP].nparts; p++) {
        struct section *s = outs[O_HELP].parts[p];
        s->offset = outs[O_HELP].size;
        outs[O_HELP].size += s->size;
    }
    free(outs[O_HELP].data);
    outs[O_HELP].data = alloc((size_t)outs[O_HELP].size + 1);
    for (int p = 0; p < outs[O_HELP].nparts; p++)
        memcpy(outs[O_HELP].data + outs[O_HELP].parts[p]->offset, outs[O_HELP].parts[p]->data,
               (size_t)outs[O_HELP].parts[p]->size);
    strtab_add(&shstrtab.data, &shstrtab.len, &shstrtab.cap, "");
    uint32_t names[O_COUNT];
    int nsh = 1;
    for (int k = 0; k < O_COUNT; k++) {
        bool keep = outs[k].size || k == O_SHSTRTAB || k == O_DYNAMIC || k == O_DYNSYM || k == O_DYNSTR;
        if (k == O_HELP && !outs[k].size)
            keep = false;
        outs[k].index = keep ? nsh++ : 0;
        if (keep)
            names[k] = strtab_add(&shstrtab.data, &shstrtab.len, &shstrtab.cap, outs[k].name);
    }
    /* (62d) exported symbols' sections, now that they are numbered */
    for (int i = 0; i < ndyn; i++) {
        struct symbol *g = dyn_list[i];
        if (!g->exported)
            continue;
        unsigned char *e = outs[O_DYNSYM].data + 24 * (i + 1);
        put(e + 6, g->is_absolute ? SHN_ABS : g->section && g->section->out ? (uint64_t)g->section->out->index
                   : g->is_common ? (uint64_t)outs[O_BSS].index : SHN_ABS, 2);
    }
    uint64_t shstr_offset = help_offset + outs[O_HELP].size;
    uint64_t shoff = align_up(shstr_offset + shstrtab.len, 8);
    uint64_t total = shoff + 64 * (uint64_t)nsh;
    unsigned char *file = alloc((size_t)total);
    memcpy(file, "\177ELF\2\1\1", 7);
    put(file + 16, mode == MODE_STATIC ? 2 : 3, 2);  /* ET_EXEC for a static program, else ET_DYN */
    put(file + 18, (uint64_t)machine, 2);
    put(file + 20, 1, 4);
    put(file + 24, mode == MODE_SHARED ? 0 : symbol_address(entry), 8);
    put(file + 32, 64, 8);
    put(file + 40, shoff, 8);
    put(file + 52, 64, 2);
    put(file + 54, 56, 2);
    put(file + 56, (uint64_t)nphdrs, 2);
    put(file + 58, 64, 2);
    put(file + 60, (uint64_t)nsh, 2);
    put(file + 62, (uint64_t)outs[O_SHSTRTAB].index, 2);
    for (int i = 0; i < nsegments; i++) {
        unsigned char *p = file + 64 + 56 * (size_t)i;
        put(p, (uint64_t)segments[i].type, 4);
        put(p + 4, (uint64_t)segments[i].flags, 4);
        put(p + 8, segments[i].type == 0x6474E551 ? 0 : segments[i].offset, 8);
        put(p + 16, segments[i].vaddr, 8);
        put(p + 24, segments[i].vaddr, 8);
        put(p + 32, segments[i].filesz, 8);
        put(p + 40, segments[i].memsz, 8);
        put(p + 48, segments[i].align, 8);
    }
    for (int i = nsegments; i < nphdrs; i++) {
        /* (unused headers: PT_NULL) */
    }
    for (int k = 0; k < O_COUNT; k++) {
        struct output *o = &outs[k];
        if (!o->size || o->type == SHT_NOBITS || !(o->flags & SHF_ALLOC))
            continue;
        memcpy(file + (o->addr - base), o->data, (size_t)o->size);
    }
    memcpy(file + help_offset, outs[O_HELP].data, (size_t)outs[O_HELP].size);
    memcpy(file + shstr_offset, shstrtab.data, shstrtab.len);
    for (int k = 0; k < O_COUNT; k++) {
        struct output *o = &outs[k];
        if (!o->index)
            continue;
        unsigned char *h = file + shoff + 64 * (size_t)o->index;
        put(h, names[k], 4);
        put(h + 4, (uint64_t)o->type, 4);
        put(h + 8, o->flags, 8);
        put(h + 16, (o->flags & SHF_ALLOC) ? o->addr : 0, 8);
        uint64_t off = (o->flags & SHF_ALLOC) ? o->addr - base : k == O_HELP ? help_offset : shstr_offset;
        put(h + 24, off, 8);
        put(h + 32, k == O_SHSTRTAB ? shstrtab.len : o->size, 8);
        if (k == O_DYNSYM || k == O_HASH)
            put(h + 40, (uint64_t)(k == O_DYNSYM ? outs[O_DYNSTR].index : outs[O_DYNSYM].index), 4);
        if (k == O_DYNSYM)
            put(h + 44, 1, 4);
        if (k == O_RELA_DYN || k == O_RELA_PLT)
            put(h + 40, (uint64_t)outs[O_DYNSYM].index, 4);
        if (k == O_RELA_PLT)
            put(h + 44, (uint64_t)outs[O_GOT_PLT].index, 4);
        if (k == O_DYNAMIC)
            put(h + 40, (uint64_t)outs[O_DYNSTR].index, 4);
        put(h + 48, o->align ? o->align : 1, 8);
        int ent = k == O_DYNSYM ? 24 : k == O_RELA_DYN || k == O_RELA_PLT ? 24 : k == O_DYNAMIC ? 16 : k == O_HASH ? 4 : 0;
        put(h + 56, (uint64_t)ent, 8);
    }
    /* a program: executable (0755, less the umask), as Unix linkers make it */
    unlink(output);
    int fd = open(output, O_WRONLY | O_CREAT | O_TRUNC, 0755);
    if (fd < 0)
        fail("cannot write %s", output);
    for (uint64_t at = 0; at < total;) {
        ssize_t w = write(fd, file + at, (size_t)(total - at));
        if (w <= 0)
            fail("cannot write %s", output);
        at += (uint64_t)w;
    }
    if (close(fd) != 0)
        fail("cannot write %s", output);
    return 0;
}
