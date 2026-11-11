/*
 * The version script of one of RitterOS's shared libraries (22c; written in
 * C in 63, scripts/libexports.py until then, whose output this writes byte
 * for byte). A library exports exactly the functions its headers declare --
 * its interface, what a program may call -- and keeps everything else
 * inside (helpers shared between its own files). The linker refuses a
 * listed function the library does not define, so a header and its library
 * cannot drift apart. The headers are read as scripts/sdkapi.py reads them
 * (it makes sdk/API.md): comments, the include guard, #include lines and
 * macros left out, each statement at brace depth 0 that is not a type
 * taken for a function, named by its first word before a parenthesis.
 *
 * libexports LIBRARY OUT    (run in the tree: the headers are user/include/)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const struct {
    const char *library;
    const char *headers[48];
} libraries[] = {
    { "libritteros.so.1",
      { "stdio.h", "stdlib.h", "string.h", "unistd.h", "fcntl.h", "errno.h", "sys/stat.h", "sys/wait.h",
        "sys/mman.h", "dlfcn.h", "dirent.h", "signal.h", "pwd.h", "thread.h", "ritteros.h", "strings.h",
        "ctype.h", "assert.h", "math.h", "fenv.h", "setjmp.h", "locale.h", "time.h", "wchar.h", "wctype.h",
        "uchar.h", "complex.h", "threads.h", "poll.h", "termios.h", "sys/socket.h", "netinet/in.h",
        "arpa/inet.h", "sys/time.h", "sys/select.h", "netdb.h", "pthread.h", "sched.h", "spawn.h", "regex.h", "utime.h" } },
    { "libwindow.so.1", { "window.h", "wfont.h" } },
    { "libimage.so.1", { "image.h" } },
    { "libaudio.so.1", { "audio.h" } },
    { "libvideo.so.1", { "video.h" } },
    { "libdraw.so.1", { "draw.h" } },
    { "libwidget.so.1", { "widget.h" } },
    { "libtls.so.1", { "tls.h" } },
};

static _Noreturn void fail(const char *what, const char *where)
{
    fprintf(stderr, "libexports: %s: %s\n", where, what);
    exit(1);
}

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        fail("cannot read it", path);
    size_t cap = 1 << 16, len = 0;
    char *text = malloc(cap);
    for (;;) {
        if (len + 4096 > cap)
            text = realloc(text, cap *= 2);
        if (!text)
            fail("out of memory", path);
        size_t n = fread(text + len, 1, cap - len - 1, f);
        if (!n)
            break;
        len += n;
    }
    fclose(f);
    text[len] = 0;
    return text;
}

/* a growing string */
struct buf {
    char *s;
    size_t n, cap;
};

static void add(struct buf *b, const char *s, size_t n)
{
    if (b->n + n + 1 > b->cap) {
        b->cap = (b->n + n + 1) * 2;
        b->s = realloc(b->s, b->cap);
        if (!b->s)
            fail("out of memory", "a string");
    }
    memcpy(b->s + b->n, s, n);
    b->n += n;
    b->s[b->n] = 0;
}

static int is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

static int is_word(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

/* the text split at newlines (the pieces written in place) */
static char **split_lines(char *text, int *count)
{
    int n = 1;
    for (char *p = text; *p; p++)
        n += *p == '\n';
    char **lines = malloc(sizeof(char *) * (size_t)n);
    int k = 0;
    lines[k++] = text;
    for (char *p = text; *p; p++)
        if (*p == '\n') {
            *p = 0;
            lines[k++] = p + 1;
        }
    *count = k;
    return lines;
}

/* a line without its leading and trailing blanks, in a copy */
static char *stripped(const char *s)
{
    while (is_space(*s))
        s++;
    size_t n = strlen(s);
    while (n && is_space(s[n - 1]))
        n--;
    char *c = malloc(n + 1);
    memcpy(c, s, n);
    c[n] = 0;
    return c;
}

/* (sdkapi.split_header) the header after its opening comment, without
 * its include guard (and that guard's #endif, the last one) and #include
 * lines; the lines are joined again with newlines */
static char *header_body(const char *path)
{
    char *text = read_file(path);
    int n;
    char **lines = split_lines(text, &n);
    int i = 0;
    while (i < n && !*stripped(lines[i]))
        i++;
    if (i < n && strncmp(stripped(lines[i]), "/*", 2) == 0) {
        while (i < n && !strstr(lines[i], "*/"))
            i++;
        i++;
    }
    char **kept = malloc(sizeof(char *) * (size_t)(n + 1));
    int nkept = 0;
    char guard[256] = "";
    for (int k = i; k < n; k++) {
        char *s = stripped(lines[k]);
        if (!*guard && strncmp(s, "#ifndef", 7) == 0 && is_space(s[7])) {
            const char *w = s + 7;
            while (is_space(*w))
                w++;
            const char *e = w;
            while (is_word(*e))
                e++;
            if (e > w && !*e && k + 1 < n && (size_t)(e - w) < sizeof(guard) - 8) {
                char want[300];
                snprintf(want, sizeof(want), "#define %.*s", (int)(e - w), w);
                if (strcmp(stripped(lines[k + 1]), want) == 0) {
                    snprintf(guard, sizeof(guard), "%.*s", (int)(e - w), w);
                    continue;
                }
            }
        }
        if (*guard) {
            char want[300];
            snprintf(want, sizeof(want), "#define %s", guard);
            if (strcmp(s, want) == 0)
                continue;
        }
        if (strncmp(s, "#include", 8) == 0)
            continue;
        kept[nkept++] = lines[k];
    }
    if (*guard)
        for (int k = nkept - 1; k >= 0; k--)
            if (strncmp(stripped(kept[k]), "#endif", 6) == 0) {
                memmove(kept + k, kept + k + 1, sizeof(char *) * (size_t)(nkept - k - 1));
                nkept--;
                break;
            }
    struct buf b = { 0 };
    add(&b, "", 0);
    for (int k = 0; k < nkept; k++) {
        if (k)
            add(&b, "\n", 1);
        add(&b, kept[k], strlen(kept[k]));
    }
    return b.s;
}

/* (sdkapi.strip_comments) the text without its comments, newlines kept */
static void strip_comments(char *t)
{
    char *w = t;
    for (char *p = t; *p;) {
        if (p[0] == '/' && p[1] == '*') {
            char *e = strstr(p + 2, "*/");
            if (!e) {
                *w++ = *p++;                    /* (an unclosed comment: Python leaves it) */
                continue;
            }
            for (char *q = p; q < e + 2; q++)
                if (*q == '\n')
                    *w++ = '\n';
            p = e + 2;
            continue;
        }
        *w++ = *p++;
    }
    *w = 0;
}

/* (sdkapi.drop_attributes) without __attribute__((...)) */
static char *drop_attributes(const char *t)
{
    struct buf b = { 0 };
    add(&b, "", 0);
    size_t len = strlen(t), i = 0;
    for (;;) {
        const char *j = strstr(t + i, "__attribute__");
        if (!j) {
            add(&b, t + i, len - i);
            return b.s;
        }
        add(&b, t + i, (size_t)(j - (t + i)));
        const char *k = strchr(j, '(');
        size_t at = k ? (size_t)(k - t) : len;
        int depth = 0;
        while (at < len) {
            if (t[at] == '(')
                depth++;
            else if (t[at] == ')' && --depth == 0)
                break;
            at++;
        }
        i = at + 1;
        if (i > len)
            return b.s;
    }
}

static char **names;
static int nnames, capnames;

static void add_name(const char *s, size_t n)
{
    for (int k = 0; k < nnames; k++)
        if (strlen(names[k]) == n && strncmp(names[k], s, n) == 0)
            return;
    if (nnames == capnames) {
        capnames = capnames ? 2 * capnames : 256;
        names = realloc(names, sizeof(char *) * (size_t)capnames);
        if (!names)
            fail("out of memory", "names");
    }
    char *c = malloc(n + 1);
    memcpy(c, s, n);
    c[n] = 0;
    names[nnames++] = c;
}

/* one statement: whitespace made single blanks; a function's name */
static void statement_done(const char *statement)
{
    char *d = drop_attributes(statement);
    struct buf s = { 0 };
    add(&s, "", 0);
    for (char *p = d; *p;) {
        while (is_space(*p))
            p++;
        if (!*p)
            break;
        char *e = p;
        while (*e && !is_space(*e))
            e++;
        if (s.n)
            add(&s, " ", 1);
        add(&s, p, (size_t)(e - p));
        p = e;
    }
    free(d);
    /* types (typedefs, struct/enum bodies) and variables are not functions */
    if (!s.n || strncmp(s.s, "typedef", 7) == 0 || strchr(s.s, '{')) {
        free(s.s);
        return;
    }
    /* the first word followed by "(" (blanks between allowed), as re.search */
    for (char *p = s.s; *p; p++) {
        if (!is_word(*p))
            continue;
        char *e = p;
        while (is_word(*e))
            e++;
        char *q = e;
        while (is_space(*q))
            q++;
        if (*q == '(') {
            add_name(p, (size_t)(e - p));
            break;
        }
        /* (a later start inside the same word can match only if this one did) */
        p = e - 1;
    }
    free(s.s);
}

/* (sdkapi.declarations) the functions a header's body declares */
static void declarations(char *body)
{
    strip_comments(body);
    int n;
    char **lines = split_lines(body, &n);
    struct buf statement = { 0 };
    add(&statement, "", 0);
    int depth = 0;
    for (int i = 0; i < n; i++) {
        const char *l = lines[i];
        while (is_space(*l))
            l++;
        if (*l == '#') {
            /* a macro, continued over its backslashed lines */
            while (lines[i][0] && lines[i][strlen(lines[i]) - 1] == '\\' && i + 1 < n)
                i++;
            continue;
        }
        for (const char *p = lines[i]; *p; p++) {
            add(&statement, p, 1);
            if (*p == '{')
                depth++;
            else if (*p == '}')
                depth--;
            else if (*p == ';' && depth == 0) {
                statement_done(statement.s);
                statement.n = 0;
                statement.s[0] = 0;
            }
        }
        add(&statement, "\n", 1);
    }
    free(statement.s);
}

static int by_name(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

int main(int argc, char **argv)
{
    int lib = -1;
    for (size_t k = 0; argc == 3 && k < sizeof(libraries) / sizeof(libraries[0]); k++)
        if (strcmp(argv[1], libraries[k].library) == 0)
            lib = (int)k;
    if (lib < 0) {
        fprintf(stderr, "usage: libexports libaudio.so.1|libdraw.so.1|libimage.so.1|libwidget.so.1|libritteros.so.1|libtls.so.1|"
                        "libvideo.so.1|libwindow.so.1 OUT\n");
        return 2;
    }
    for (int h = 0; libraries[lib].headers[h]; h++) {
        char path[512];
        snprintf(path, sizeof(path), "user/include/%s", libraries[lib].headers[h]);
        declarations(header_body(path));
    }
    qsort(names, (size_t)nnames, sizeof(char *), by_name);
    FILE *out = fopen(argv[2], "w");
    if (!out)
        fail("cannot write it", argv[2]);
    fprintf(out, "/* %s: made by scripts/libexports.c from its headers. */\n{\n  global:\n", argv[1]);
    for (int k = 0; k < nnames; k++)
        fprintf(out, "    %s;\n", names[k]);
    fprintf(out, "  local:\n    *;\n};\n");
    if (fclose(out) != 0)
        fail("cannot write it", argv[2]);
    return 0;
}
