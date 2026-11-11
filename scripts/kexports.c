/*
 * (67a) The kernel's interface for its modules; (67b) the driver
 * interface's headers (in drivers/include/) are it: every function they
 * declare, outside the kernel's own build, is what a module may call.
 * Built for the machine that runs the build (the Mac or RitterOS: no
 * Python there, so in C as the other generators).
 *
 * kexports table OUTPUT.c HEADER...
 *     the table the loader links modules with (kernel/module.h's
 *     kernel_exports): each name's address, bound by an assembler label so
 *     that no declaration of the kernel's (a builtin's) is repeated; a
 *     function marked DRIVER_WRAPPED (inline in the kernel) is the
 *     kernel's copy driver_NAME (kernel/driver.c).
 * kexports check MODULE HEADER...
 *     every name the module (a relocatable ELF object) leaves undefined
 *     must be declared there: otherwise the name and the module are
 *     printed and it fails, so a module cannot reach into the kernel past
 *     its interface.
 *
 * A header is read as C: comments and preprocessor lines (with their
 * continuations) left out, statements split at ';' outside braces and
 * parentheses; one with a parenthesis that is not a typedef nor a type's
 * definition declares the function named right before its first one.
 */
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NAMES_MAX 2048

static char *names[NAMES_MAX];
static int name_count;

static int wrapped[NAMES_MAX];

static void add_name(const char *name, int wrap)
{
    for (int i = 0; i < name_count; i++)
        if (!strcmp(names[i], name))
            return;
    if (name_count == NAMES_MAX) {
        fprintf(stderr, "kexports: more than %d names\n", NAMES_MAX);
        exit(1);
    }
    wrapped[name_count] = wrap;
    names[name_count++] = strdup(name);
}

/* One statement: the function it declares, if any. */
static void statement(char *s)
{
    while (isspace((unsigned char)*s))
        s++;
    if (!strncmp(s, "typedef", 7) || strchr(s, '{'))
        return;
    char *paren = strchr(s, '(');
    if (!paren)
        return;
    char *end = paren;
    while (end > s && isspace((unsigned char)end[-1]))
        end--;
    char *begin = end;
    while (begin > s && (isalnum((unsigned char)begin[-1]) || begin[-1] == '_'))
        begin--;
    if (begin == end)
        return;
    char name[128];
    size_t n = (size_t)(end - begin) < sizeof(name) - 1 ? (size_t)(end - begin) : sizeof(name) - 1;
    memcpy(name, begin, n);
    name[n] = 0;
    add_name(name, !strncmp(s, "DRIVER_WRAPPED", 14));
}

static void read_header(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "kexports: cannot read %s\n", path);
        exit(1);
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *text = malloc((size_t)size + 1), *out = malloc((size_t)size + 1);
    if (!text || !out || fread(text, 1, (size_t)size, f) != (size_t)size) {
        fprintf(stderr, "kexports: cannot read %s\n", path);
        exit(1);
    }
    fclose(f);
    text[size] = 0;
    /* Comments and preprocessor lines out. */
    size_t o = 0;
    int line_start = 1, in_directive = 0;
    for (long i = 0; i < size; i++) {
        if (text[i] == '/' && text[i + 1] == '*') {
            char *close = strstr(text + i + 2, "*/");
            i = close ? close - text + 1 : size;
            continue;
        }
        if (text[i] == '/' && text[i + 1] == '/') {
            while (i < size && text[i] != '\n')
                i++;
            i--;
            continue;
        }
        if (line_start && !in_directive) {
            long k = i;
            while (k < size && (text[k] == ' ' || text[k] == '\t'))
                k++;
            if (text[k] == '#')
                in_directive = 1;
        }
        if (text[i] == '\n') {
            int continued = i > 0 && text[i - 1] == '\\';
            line_start = 1;
            if (in_directive && !continued)
                in_directive = 0;
            out[o++] = '\n';
            continue;
        }
        line_start = 0;
        if (!in_directive)
            out[o++] = text[i];
    }
    out[o] = 0;
    /* Statements at depth 0. */
    int depth = 0;
    char *begin = out;
    for (char *p = out; *p; p++) {
        if (*p == '{' || *p == '(')
            depth++;
        else if (*p == '}' || *p == ')')
            depth--;
        else if (*p == ';' && depth == 0) {
            *p = 0;
            statement(begin);
            begin = p + 1;
        }
    }
    free(text);
    free(out);
}

static int listed(const char *name)
{
    for (int i = 0; i < name_count; i++)
        if (!strcmp(names[i], name))
            return 1;
    return 0;
}

static int table(const char *out)
{
    FILE *f = fopen(out, "w");
    if (!f) {
        fprintf(stderr, "kexports: cannot write %s\n", out);
        return 1;
    }
    fprintf(f, "/* Made by scripts/kexports.c from drivers/include/: do not edit. */\n");
    fprintf(f, "#include \"module.h\"\n\n");
    for (int i = 0; i < name_count; i++)
        fprintf(f, "extern char export_%d[] __asm__(\"%s%s\");\n", i, wrapped[i] ? "driver_" : "",
                names[i]);
    fprintf(f, "\nconst struct kernel_export kernel_exports[] = {\n");
    for (int i = 0; i < name_count; i++)
        fprintf(f, "    { \"%s\", export_%d },\n", names[i], i);
    fprintf(f, "};\n\nconst int kernel_export_count = %d;\n", name_count);
    return fclose(f) ? 1 : 0;
}

static uint64_t u64(const unsigned char *p)
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--)
        v = v << 8 | p[i];
    return v;
}

static uint32_t u32(const unsigned char *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static unsigned u16(const unsigned char *p)
{
    return (unsigned)p[0] | (unsigned)p[1] << 8;
}

/* The module's undefined names, each checked against the list. */
static int check(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "kexports: cannot read %s\n", path);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *d = malloc(size > 0 ? (size_t)size : 1);
    if (!d || fread(d, 1, (size_t)size, f) != (size_t)size) {
        fprintf(stderr, "kexports: cannot read %s\n", path);
        return 1;
    }
    fclose(f);
    if (size < 64 || memcmp(d, "\177ELF", 4) || d[4] != 2 || u16(d + 16) != 1) {
        fprintf(stderr, "kexports: %s is not a relocatable ELF object\n", path);
        return 1;
    }
    uint64_t shoff = u64(d + 0x28);
    unsigned shentsize = u16(d + 0x3A), shnum = u16(d + 0x3C);
    int bad = 0;
    for (unsigned i = 0; i < shnum; i++) {
        const unsigned char *s = d + shoff + (uint64_t)i * shentsize;
        if (u32(s + 4) != 2)                        /* SHT_SYMTAB */
            continue;
        const unsigned char *link = d + shoff + (uint64_t)u32(s + 40) * shentsize;
        const char *strings = (const char *)d + u64(link + 24);
        uint64_t off = u64(s + 24), bytes = u64(s + 32);
        for (uint64_t k = 0; k < bytes / 24; k++) {
            const unsigned char *y = d + off + k * 24;
            unsigned bind = y[4] >> 4, shndx = u16(y + 6);
            uint32_t name = u32(y);
            if (shndx || !name || bind == 0)        /* defined, nameless, or local */
                continue;
            if (!listed(strings + name)) {
                fprintf(stderr, "kexports: %s uses %s, which the driver interface does not declare\n",
                        path, strings + name);
                bad = 1;
            }
        }
    }
    free(d);
    return bad;
}

int main(int argc, char **argv)
{
    if (argc >= 4 && !strcmp(argv[1], "table")) {
        for (int i = 3; i < argc; i++)
            read_header(argv[i]);
        return table(argv[2]);
    }
    if (argc >= 4 && !strcmp(argv[1], "check")) {
        for (int i = 3; i < argc; i++)
            read_header(argv[i]);
        return check(argv[2]);
    }
    fprintf(stderr, "usage: kexports table OUTPUT.c HEADER... | kexports check MODULE HEADER...\n");
    return 2;
}
