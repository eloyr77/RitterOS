/*
 * make [-f MAKEFILE]... [-C DIR] [-nsikqeBt] [NAME=VALUE]... [TARGET]... (59):
 * POSIX's make, with what most Makefiles use from GNU make.
 *
 * A Makefile (makefile, Makefile, GNUmakefile; or -f) holds rules
 * (TARGETS: PREREQUISITES, then recipe lines starting with a tab; ; RECIPE
 * on the rule's line; | ORDER-ONLY), variables (=, :=, ::=, ?=, +=, !=;
 * override; define ... endef; export), conditionals (ifeq, ifneq, ifdef,
 * ifndef, else, endif), include and -include, pattern rules (%.o: %.c),
 * static pattern rules (OBJS: %.o: %.c), suffix rules (.c.o:) and the
 * built-in rules for C. A target is remade when it is missing, phony
 * (.PHONY), or older than a prerequisite; each recipe line runs in sh -c,
 * echoed unless it starts with @ (or -s, .SILENT); a - in front ignores its
 * failure; + runs it under -n too (as does a line with $(MAKE)).
 * References: $(VAR), ${VAR}, $X, $(VAR:A=B) and $(VAR:%.c=%.o), the
 * automatic $@ $< $^ $+ $? $* $| with their D and F forms, and the
 * functions subst patsubst strip findstring filter filter-out sort word
 * wordlist words firstword lastword dir notdir suffix basename addsuffix
 * addprefix join wildcard realpath abspath if or and foreach call value
 * origin flavor shell eval error warning info. One job at a time (-j is
 * taken and ignored). Exit status 0, 1 for -q with something out of date,
 * 2 after an error.
 */
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include "ritteros.h"

PROGRAM_HELP("make [-f MAKEFILE]... [-C DIR] [-nsikqBteSw] [NAME=VALUE]... [TARGET]...",
             "build targets from a Makefile's rules (POSIX make with common GNU make)");

/* --- strings --------------------------------------------------------------------- */

struct text {
    char *s;
    size_t n, room;
};

static void *must(void *p)
{
    if (!p) {
        dprintf(STDERR_FILENO, "make: out of memory\n");
        exit(2);
    }
    return p;
}

static void add(struct text *t, const char *s, size_t n)
{
    if (t->n + n + 1 > t->room) {
        size_t room = t->room ? t->room : 64;
        while (t->n + n + 1 > room)
            room *= 2;
        t->s = must(realloc(t->s, room));
        t->room = room;
    }
    memcpy(t->s + t->n, s, n);
    t->n += n;
    t->s[t->n] = 0;
}

static void add_str(struct text *t, const char *s)
{
    add(t, s, strlen(s));
}

static void add_char(struct text *t, char c)
{
    add(t, &c, 1);
}

static char *done(struct text *t)
{
    if (!t->s)
        add(t, "", 0);
    return t->s;
}

static char *copy(const char *s)
{
    return must(strdup(s));
}

static char *copy_n(const char *s, size_t n)
{
    char *r = must(malloc(n + 1));
    memcpy(r, s, n);
    r[n] = 0;
    return r;
}

static int level;                       /* MAKELEVEL: 0 at the top */
static const char *current_file;
static int current_line;

static void message(const char *format, ...)
{
    fflush(stdout);
    char text[1024];
    va_list ap;
    va_start(ap, format);
    vsnprintf(text, sizeof(text), format, ap);
    va_end(ap);
    if (level)
        dprintf(STDERR_FILENO, "make[%d]: %s\n", level, text);
    else
        dprintf(STDERR_FILENO, "make: %s\n", text);
}

/* A note on the standard output (GNU's "Nothing to be done for ..."). */
static void note(const char *format, ...)
{
    char text[1024];
    va_list ap;
    va_start(ap, format);
    vsnprintf(text, sizeof(text), format, ap);
    va_end(ap);
    if (level)
        printf("make[%d]: %s\n", level, text);
    else
        printf("make: %s\n", text);
    fflush(stdout);
}

static __attribute__((noreturn)) void fatal(const char *format, ...)
{
    fflush(stdout);
    char text[1024];
    va_list ap;
    va_start(ap, format);
    vsnprintf(text, sizeof(text), format, ap);
    va_end(ap);
    if (current_file)
        dprintf(STDERR_FILENO, "%s:%d: *** %s.  Stop.\n", current_file, current_line, text);
    else if (level)
        dprintf(STDERR_FILENO, "make[%d]: *** %s.  Stop.\n", level, text);
    else
        dprintf(STDERR_FILENO, "make: *** %s.  Stop.\n", text);
    exit(2);
}

/* Words: blank-separated pieces. */
struct words {
    char **w;
    int n, room;
};

static void push_word(struct words *ws, char *w)
{
    if (ws->n == ws->room) {
        ws->room = ws->room ? ws->room * 2 : 8;
        ws->w = must(realloc(ws->w, sizeof(char *) * (size_t)ws->room));
    }
    ws->w[ws->n++] = w;
}

static struct words split_words(const char *s)
{
    struct words ws = { 0 };
    while (*s) {
        while (isspace((unsigned char)*s))
            s++;
        if (!*s)
            break;
        const char *e = s;
        while (*e && !isspace((unsigned char)*e))
            e++;
        push_word(&ws, copy_n(s, (size_t)(e - s)));
        s = e;
    }
    return ws;
}

static void free_words(struct words *ws)
{
    for (int i = 0; i < ws->n; i++)
        free(ws->w[i]);
    free(ws->w);
    ws->w = 0;
    ws->n = ws->room = 0;
}

static char *join_words(struct words *ws, int from, int to)
{
    struct text t = { 0 };
    for (int i = from; i < to && i < ws->n; i++) {
        if (t.n)
            add_char(&t, ' ');
        add_str(&t, ws->w[i]);
    }
    return done(&t);
}

static char *trim(const char *s)
{
    while (isspace((unsigned char)*s))
        s++;
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1]))
        n--;
    return copy_n(s, n);
}

/* --- variables ----------------------------------------------------------------------- */

enum { O_DEFAULT, O_ENVIRONMENT, O_FILE, O_COMMAND, O_OVERRIDE, O_AUTOMATIC };
static const char *const origin_names[] = { "default", "environment", "file", "command line",
                                            "override", "automatic" };

struct variable {
    char *name, *value;
    bool simple;                        /* := (expanded once) or = (at each use) */
    bool exported;
    int origin;
    bool expanding;                     /* (a reference to itself: an error) */
    struct variable *next;
};

#define BUCKETS 512
static struct variable *variables[BUCKETS];
static bool environment_overrides;      /* -e */

static unsigned hash(const char *s)
{
    unsigned h = 5381;
    while (*s)
        h = h * 33 + (unsigned char)*s++;
    return h % BUCKETS;
}

static struct variable *find_variable(const char *name)
{
    for (struct variable *v = variables[hash(name)]; v; v = v->next)
        if (!strcmp(v->name, name))
            return v;
    return 0;
}

/* Set name, unless a stronger origin set it (command line over the file,
 * override over both; -e: the environment over the file). */
static struct variable *set_variable(const char *name, const char *value, bool simple, int origin)
{
    struct variable *v = find_variable(name);
    if (v) {
        int have = v->origin;
        if (environment_overrides && have == O_ENVIRONMENT && origin == O_FILE)
            return v;
        if (have == O_COMMAND && origin < O_COMMAND)
            return v;
        if (have == O_OVERRIDE && origin < O_OVERRIDE)
            return v;
        free(v->value);
    } else {
        v = must(calloc(1, sizeof(*v)));
        v->name = copy(name);
        unsigned h = hash(name);
        v->next = variables[h];
        variables[h] = v;
    }
    v->value = copy(value);
    v->simple = simple;
    v->origin = origin;
    return v;
}

/* The automatic variables of the recipe being run. */
struct automatic {
    const char *target, *first, *stem;
    char *all, *all_dups, *newer, *order_only;
};

static struct automatic *automatic;

static char *expand(const char *s);

/* --- functions ------------------------------------------------------------------------- */

/* Does text match pattern (one %); the stem in *stem (malloc'd)? */
static bool pattern_match(const char *pattern, const char *text, char **stem)
{
    const char *pct = strchr(pattern, '%');
    if (!pct) {
        if (strcmp(pattern, text))
            return false;
        if (stem)
            *stem = copy("");
        return true;
    }
    size_t pre = (size_t)(pct - pattern), post = strlen(pct + 1), n = strlen(text);
    if (n < pre + post || strncmp(text, pattern, pre) || strcmp(text + n - post, pct + 1))
        return false;
    if (stem)
        *stem = copy_n(text + pre, n - pre - post);
    return true;
}

static char *pattern_replace(const char *pattern, const char *stem)
{
    const char *pct = strchr(pattern, '%');
    if (!pct)
        return copy(pattern);
    struct text t = { 0 };
    add(&t, pattern, (size_t)(pct - pattern));
    add_str(&t, stem);
    add_str(&t, pct + 1);
    return done(&t);
}

static char *patsubst(const char *from, const char *to, const char *text)
{
    struct words ws = split_words(text);
    struct text t = { 0 };
    for (int i = 0; i < ws.n; i++) {
        char *stem;
        if (t.n)
            add_char(&t, ' ');
        if (pattern_match(from, ws.w[i], &stem)) {
            char *r = pattern_replace(to, stem);
            add_str(&t, r);
            free(r);
            free(stem);
        } else {
            add_str(&t, ws.w[i]);
        }
    }
    free_words(&ws);
    return done(&t);
}

/* A shell-style pattern (* ? [set]) against one name. */
static bool glob_match(const char *p, const char *s)
{
    for (; *p; p++, s++) {
        if (*p == '*') {
            while (*p == '*')
                p++;
            if (!*p)
                return true;
            for (; *s; s++)
                if (glob_match(p, s))
                    return true;
            return glob_match(p, s);
        }
        if (!*s)
            return false;
        if (*p == '?')
            continue;
        if (*p == '[') {
            const char *q = p + 1;
            bool negate = *q == '!' || *q == '^';
            if (negate)
                q++;
            bool hit = false;
            for (bool first = true; *q && (*q != ']' || first); q++, first = false) {
                if (q[1] == '-' && q[2] && q[2] != ']') {
                    hit |= *s >= q[0] && *s <= q[2];
                    q += 2;
                } else {
                    hit |= *s == *q;
                }
            }
            if (*q != ']')
                return *s == '[';                   /* (no end: a plain [) */
            if (hit == negate)
                return false;
            p = q;
            continue;
        }
        if (*p == '\\' && p[1])
            p++;
        if (*p != *s)
            return false;
    }
    return !*s;
}

static void glob_into(const char *dir, const char *rest, struct words *out)
{
    const char *slash = strchr(rest, '/');
    size_t len = slash ? (size_t)(slash - rest) : strlen(rest);
    char *part = copy_n(rest, len);
    bool magic = strpbrk(part, "*?[") != 0;
    if (!magic) {
        struct text path = { 0 };
        add_str(&path, dir);
        if (path.n && path.s[path.n - 1] != '/')
            add_char(&path, '/');
        add_str(&path, part);
        if (slash) {
            glob_into(path.s, slash + 1, out);
            free(path.s);
        } else {
            struct stat st;
            if (stat(path.s, &st) == 0)
                push_word(out, path.s);
            else
                free(path.s);
        }
        free(part);
        return;
    }
    DIR *d = opendir(*dir ? dir : ".");
    if (!d) {
        free(part);
        return;
    }
    struct words names = { 0 };
    for (struct dirent *e; (e = readdir(d));) {
        if (e->d_name[0] == '.' && part[0] != '.')
            continue;
        if (glob_match(part, e->d_name))
            push_word(&names, copy(e->d_name));
    }
    closedir(d);
    for (int i = 1; i < names.n; i++)               /* sorted, as GNU make's */
        for (int j = i; j > 0 && strcmp(names.w[j - 1], names.w[j]) > 0; j--) {
            char *t = names.w[j];
            names.w[j] = names.w[j - 1];
            names.w[j - 1] = t;
        }
    for (int i = 0; i < names.n; i++) {
        struct text path = { 0 };
        if (*dir) {
            add_str(&path, dir);
            if (path.s[path.n - 1] != '/')
                add_char(&path, '/');
        }
        add_str(&path, names.w[i]);
        if (slash) {
            struct stat st;
            if (stat(path.s, &st) == 0 && S_ISDIR(st.st_mode))
                glob_into(path.s, slash + 1, out);
            free(path.s);
        } else {
            push_word(out, path.s);
        }
    }
    free_words(&names);
    free(part);
}

static char *wildcard(const char *patterns)
{
    struct words ps = split_words(patterns), out = { 0 };
    for (int i = 0; i < ps.n; i++) {
        const char *p = ps.w[i];
        if (p[0] == '/')
            glob_into("/", p + 1, &out);
        else
            glob_into("", p, &out);
    }
    char *r = join_words(&out, 0, out.n);
    free_words(&ps);
    free_words(&out);
    return r;
}

static char *absolute(const char *path, bool real)
{
    struct text t = { 0 };
    if (path[0] != '/') {
        char cwd[512];
        if (getcwd(cwd, sizeof(cwd)))
            add_str(&t, cwd);
    }
    add_char(&t, '/');
    add_str(&t, path);
    /* Normalize: no "." or "..", no double slashes. */
    struct words parts = { 0 };
    for (char *s = t.s; *s;) {
        while (*s == '/')
            s++;
        if (!*s)
            break;
        char *e = s;
        while (*e && *e != '/')
            e++;
        char *name = copy_n(s, (size_t)(e - s));
        if (!strcmp(name, ".")) {
            free(name);
        } else if (!strcmp(name, "..")) {
            free(name);
            if (parts.n)
                free(parts.w[--parts.n]);
        } else {
            push_word(&parts, name);
        }
        s = e;
    }
    struct text r = { 0 };
    for (int i = 0; i < parts.n; i++) {
        add_char(&r, '/');
        add_str(&r, parts.w[i]);
    }
    if (!r.n)
        add_char(&r, '/');
    free_words(&parts);
    free(t.s);
    if (real) {
        struct stat st;
        if (stat(r.s, &st) < 0) {
            free(r.s);
            return copy("");
        }
    }
    return done(&r);
}

static char *run_shell(const char *command)
{
    fflush(stdout);
    FILE *p = popen(command, "r");
    struct text t = { 0 };
    if (p) {
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), p)) > 0)
            add(&t, buf, n);
        int status = pclose(p);
        char code[16];
        snprintf(code, sizeof(code), "%d", WIFEXITED(status) ? WEXITSTATUS(status) : 1);
        set_variable(".SHELLSTATUS", code, true, O_AUTOMATIC);
    }
    done(&t);
    while (t.n && t.s[t.n - 1] == '\n')             /* (trailing line breaks go, others */
        t.s[--t.n] = 0;                             /* become blanks) */
    for (size_t i = 0; i < t.n; i++)
        if (t.s[i] == '\n')
            t.s[i] = ' ';
    return t.s;
}

/* The arguments of $(func a,b,c): split at top-level commas (max pieces). */
static struct words arguments(const char *s, int max)
{
    struct words ws = { 0 };
    int depth = 0;
    const char *start = s;
    for (const char *p = s;; p++) {
        if (!*p || (*p == ',' && depth == 0 && (max == 0 || ws.n < max - 1))) {
            push_word(&ws, copy_n(start, (size_t)(p - start)));
            if (!*p)
                break;
            start = p + 1;
            continue;
        }
        if (*p == '(' || *p == '{')
            depth++;
        else if ((*p == ')' || *p == '}') && depth)
            depth--;
    }
    return ws;
}

static void parse_makefile_text(const char *text, const char *name);

static char *call_function(const char *name, const char *rest, bool *found)
{
    *found = true;
#define IS(n) (!strcmp(name, n))
    /* Lazy ones first: their arguments are expanded as needed. */
    if (IS("if") || IS("or") || IS("and")) {
        struct words a = arguments(rest, IS("if") ? 3 : 0);
        char *r = 0;
        if (IS("if")) {
            char *c = expand(a.n > 0 ? a.w[0] : ""), *t = trim(c);
            bool yes = t[0] != 0;
            free(c), free(t);
            r = yes ? expand(a.n > 1 ? a.w[1] : "") : expand(a.n > 2 ? a.w[2] : "");
        } else {
            for (int i = 0; i < a.n; i++) {
                char *e = expand(a.w[i]), *t = trim(e);
                free(e);
                bool empty = !t[0];
                if (IS("or") && !empty) {
                    r = t;
                    break;
                }
                if (IS("and") && empty) {
                    free(t);
                    r = copy("");
                    break;
                }
                free(r);
                r = t;
            }
            if (!r)
                r = copy("");
        }
        free_words(&a);
        return r;
    }
    if (IS("foreach")) {
        struct words a = arguments(rest, 3);
        char *var = a.n > 0 ? expand(a.w[0]) : copy(""), *v = trim(var), *list = a.n > 1 ? expand(a.w[1]) : copy("");
        free(var);
        struct words items = split_words(list);
        struct text out = { 0 };
        struct variable *old = find_variable(v);
        char *saved = old ? copy(old->value) : 0;
        bool saved_simple = old && old->simple;
        int saved_origin = old ? old->origin : 0;
        for (int i = 0; i < items.n; i++) {
            set_variable(v, items.w[i], true, O_AUTOMATIC);
            char *e = expand(a.n > 2 ? a.w[2] : "");
            if (out.n)
                add_char(&out, ' ');
            add_str(&out, e);
            free(e);
        }
        if (saved) {
            struct variable *x = find_variable(v);
            free(x->value);
            x->value = saved;
            x->simple = saved_simple;
            x->origin = saved_origin;
        } else if (items.n) {
            set_variable(v, "", true, O_AUTOMATIC);
        }
        free_words(&items);
        free(list), free(v);
        free_words(&a);
        return done(&out);
    }
    if (IS("call")) {
        struct words a = arguments(rest, 0);
        char *fname = a.n ? expand(a.w[0]) : copy(""), *fn = trim(fname);
        free(fname);
        struct variable *body = find_variable(fn);
        /* $(1)... set for the call, the old values back afterwards. */
        int count = a.n > 10 ? a.n : 10;
        char **saved = must(calloc((size_t)count + 1, sizeof(char *)));
        bool *had = must(calloc((size_t)count + 1, sizeof(bool)));
        for (int i = 0; i <= count; i++) {
            char key[8];
            snprintf(key, sizeof(key), "%d", i);
            struct variable *old = find_variable(key);
            had[i] = old != 0;
            saved[i] = old ? copy(old->value) : 0;
            char *value = i == 0 ? copy(fn) : i < a.n ? expand(a.w[i]) : copy("");
            set_variable(key, value, true, O_AUTOMATIC);
            free(value);
        }
        char *r = body ? (body->simple ? copy(body->value) : expand(body->value)) : copy("");
        for (int i = 0; i <= count; i++) {
            char key[8];
            snprintf(key, sizeof(key), "%d", i);
            set_variable(key, saved[i] ? saved[i] : "", true, O_AUTOMATIC);
            free(saved[i]);
        }
        free(saved), free(had), free(fn);
        free_words(&a);
        return r;
    }
    if (IS("value")) {
        char *n = expand(rest), *t = trim(n);
        struct variable *v = find_variable(t);
        free(n), free(t);
        return copy(v ? v->value : "");
    }
    if (IS("origin") || IS("flavor")) {
        char *n = expand(rest), *t = trim(n);
        struct variable *v = find_variable(t);
        bool automatic_one = !strcmp(t, "@") || !strcmp(t, "<") || !strcmp(t, "^");
        free(n), free(t);
        if (IS("flavor"))
            return copy(!v ? "undefined" : v->simple ? "simple" : "recursive");
        return copy(automatic_one && automatic ? "automatic" : v ? origin_names[v->origin]
                                                              : "undefined");
    }
    /* The others: all arguments expanded first. */
    struct words raw = arguments(rest, IS("subst") || IS("patsubst") ? 3
                                       : IS("filter") || IS("filter-out") || IS("word") ||
                                         IS("addsuffix") || IS("addprefix") || IS("join") ||
                                         IS("findstring") ? 2 : IS("wordlist") ? 3 : 1);
    struct words a = { 0 };
    for (int i = 0; i < raw.n; i++)
        push_word(&a, expand(raw.w[i]));
    free_words(&raw);
    const char *a0 = a.n > 0 ? a.w[0] : "", *a1 = a.n > 1 ? a.w[1] : "", *a2 = a.n > 2 ? a.w[2] : "";
    char *r = 0;
    if (IS("subst")) {
        struct text t = { 0 };
        size_t fl = strlen(a0);
        for (const char *s = a2; *s;) {
            if (fl && !strncmp(s, a0, fl)) {
                add_str(&t, a1);
                s += fl;
            } else {
                add_char(&t, *s++);
            }
        }
        r = done(&t);
    } else if (IS("patsubst")) {
        char *from = trim(a0), *to = trim(a1);
        r = patsubst(from, to, a2);
        free(from), free(to);
    } else if (IS("strip")) {
        struct words ws = split_words(a0);
        r = join_words(&ws, 0, ws.n);
        free_words(&ws);
    } else if (IS("findstring")) {
        r = copy(strstr(a1, a0) ? a0 : "");
    } else if (IS("filter") || IS("filter-out")) {
        struct words ps = split_words(a0), ws = split_words(a1), out = { 0 };
        for (int i = 0; i < ws.n; i++) {
            bool hit = false;
            for (int k = 0; k < ps.n && !hit; k++)
                hit = pattern_match(ps.w[k], ws.w[i], 0);
            if (hit == IS("filter"))
                push_word(&out, copy(ws.w[i]));
        }
        r = join_words(&out, 0, out.n);
        free_words(&ps), free_words(&ws), free_words(&out);
    } else if (IS("sort")) {
        struct words ws = split_words(a0), out = { 0 };
        for (int i = 1; i < ws.n; i++)
            for (int j = i; j > 0 && strcmp(ws.w[j - 1], ws.w[j]) > 0; j--) {
                char *t = ws.w[j];
                ws.w[j] = ws.w[j - 1];
                ws.w[j - 1] = t;
            }
        for (int i = 0; i < ws.n; i++)
            if (!out.n || strcmp(out.w[out.n - 1], ws.w[i]))
                push_word(&out, copy(ws.w[i]));
        r = join_words(&out, 0, out.n);
        free_words(&ws), free_words(&out);
    } else if (IS("word")) {
        struct words ws = split_words(a1);
        int n = atoi(a0);
        if (n <= 0)
            fatal("first argument to 'word' function must be greater than 0");
        r = copy(n <= ws.n ? ws.w[n - 1] : "");
        free_words(&ws);
    } else if (IS("wordlist")) {
        struct words ws = split_words(a2);
        int s = atoi(a0), e = atoi(a1);
        r = s >= 1 && e >= s ? join_words(&ws, s - 1, e) : copy("");
        free_words(&ws);
    } else if (IS("words")) {
        struct words ws = split_words(a0);
        char n[16];
        snprintf(n, sizeof(n), "%d", ws.n);
        r = copy(n);
        free_words(&ws);
    } else if (IS("firstword") || IS("lastword")) {
        struct words ws = split_words(a0);
        r = copy(!ws.n ? "" : IS("firstword") ? ws.w[0] : ws.w[ws.n - 1]);
        free_words(&ws);
    } else if (IS("dir") || IS("notdir") || IS("suffix") || IS("basename")) {
        struct words ws = split_words(a0);
        struct text t = { 0 };
        for (int i = 0; i < ws.n; i++) {
            const char *w = ws.w[i], *slash = strrchr(w, '/');
            const char *dot = strrchr(w, '.');
            if (dot && slash && dot < slash)
                dot = 0;
            if (IS("suffix") && !dot)
                continue;
            if (t.n)
                add_char(&t, ' ');
            if (IS("dir"))
                slash ? add(&t, w, (size_t)(slash - w) + 1) : add_str(&t, "./");
            else if (IS("notdir"))
                add_str(&t, slash ? slash + 1 : w);
            else if (IS("suffix"))
                add_str(&t, dot);
            else
                dot ? add(&t, w, (size_t)(dot - w)) : add_str(&t, w);
        }
        free_words(&ws);
        r = done(&t);
    } else if (IS("addsuffix") || IS("addprefix")) {
        struct words ws = split_words(a1);
        struct text t = { 0 };
        for (int i = 0; i < ws.n; i++) {
            if (t.n)
                add_char(&t, ' ');
            if (IS("addprefix"))
                add_str(&t, a0);
            add_str(&t, ws.w[i]);
            if (IS("addsuffix"))
                add_str(&t, a0);
        }
        free_words(&ws);
        r = done(&t);
    } else if (IS("join")) {
        struct words x = split_words(a0), y = split_words(a1);
        struct text t = { 0 };
        for (int i = 0; i < x.n || i < y.n; i++) {
            if (t.n)
                add_char(&t, ' ');
            if (i < x.n)
                add_str(&t, x.w[i]);
            if (i < y.n)
                add_str(&t, y.w[i]);
        }
        free_words(&x), free_words(&y);
        r = done(&t);
    } else if (IS("wildcard")) {
        r = wildcard(a0);
    } else if (IS("abspath") || IS("realpath")) {
        struct words ws = split_words(a0);
        struct text t = { 0 };
        for (int i = 0; i < ws.n; i++) {
            char *p = absolute(ws.w[i], IS("realpath"));
            if (*p) {
                if (t.n)
                    add_char(&t, ' ');
                add_str(&t, p);
            }
            free(p);
        }
        free_words(&ws);
        r = done(&t);
    } else if (IS("shell")) {
        r = run_shell(a0);
    } else if (IS("eval")) {
        parse_makefile_text(a0, current_file ? current_file : "(eval)");
        r = copy("");
    } else if (IS("error")) {
        fatal("%s", a0);
    } else if (IS("warning")) {
        fflush(stdout);
        dprintf(STDERR_FILENO, "%s:%d: %s\n", current_file ? current_file : "make", current_line, a0);
        r = copy("");
    } else if (IS("info")) {
        printf("%s\n", a0);
        fflush(stdout);
        r = copy("");
    } else {
        *found = false;
    }
#undef IS
    free_words(&a);
    return r;
}

/* --- expansion ------------------------------------------------------------------------- */

static char *automatic_value(const char *name)
{
    if (!automatic)
        return 0;
    char kind = name[0];
    const char *v = kind == '@' ? automatic->target : kind == '<' ? automatic->first
                  : kind == '^' ? automatic->all : kind == '+' ? automatic->all_dups
                  : kind == '?' ? automatic->newer : kind == '*' ? automatic->stem
                  : kind == '|' ? automatic->order_only : 0;
    if (!v || !strchr("@<^+?*|", kind))
        return 0;
    if (!name[1])
        return copy(v);
    if ((name[1] == 'D' || name[1] == 'F') && !name[2]) {   /* $(@D), $(<F)... */
        struct words ws = split_words(v);
        struct text t = { 0 };
        for (int i = 0; i < ws.n; i++) {
            const char *slash = strrchr(ws.w[i], '/');
            if (t.n)
                add_char(&t, ' ');
            if (name[1] == 'D')
                slash ? add(&t, ws.w[i], (size_t)(slash == ws.w[i] ? 1 : slash - ws.w[i]))
                      : add_str(&t, ".");
            else
                add_str(&t, slash ? slash + 1 : ws.w[i]);
        }
        free_words(&ws);
        return done(&t);
    }
    return 0;
}

static char *variable_value(const char *name)
{
    char *a = automatic_value(name);
    if (a)
        return a;
    struct variable *v = find_variable(name);
    if (!v)
        return copy("");
    if (v->simple)
        return copy(v->value);
    if (v->expanding)
        fatal("Recursive variable '%s' references itself (eventually)", name);
    v->expanding = true;
    char *r = expand(v->value);
    v->expanding = false;
    return r;
}

/* The end of a reference opened at s (after its '(' or '{'). */
static const char *reference_end(const char *s, char open)
{
    char close = open == '(' ? ')' : '}';
    int depth = 1;
    for (; *s; s++) {
        if (*s == open)
            depth++;
        else if (*s == close && --depth == 0)
            return s;
    }
    return 0;
}

static char *expand(const char *s)
{
    struct text t = { 0 };
    while (*s) {
        if (*s != '$') {
            const char *e = strchr(s, '$');
            size_t n = e ? (size_t)(e - s) : strlen(s);
            add(&t, s, n);
            s += n;
            continue;
        }
        s++;
        if (!*s)
            break;
        if (*s == '$') {
            add_char(&t, '$');
            s++;
            continue;
        }
        if (*s != '(' && *s != '{') {               /* $X */
            char name[2] = { *s++, 0 };
            char *v = variable_value(name);
            add_str(&t, v);
            free(v);
            continue;
        }
        char open = *s++;
        const char *end = reference_end(s, open);
        if (!end)
            fatal("unterminated variable reference");
        char *inner = copy_n(s, (size_t)(end - s));
        s = end + 1;
        /* A function: a known name, then a blank. */
        size_t name_len = 0;
        while (inner[name_len] && !isspace((unsigned char)inner[name_len]) && inner[name_len] != ':' &&
               inner[name_len] != '$')
            name_len++;
        if (inner[name_len] && isspace((unsigned char)inner[name_len])) {
            char *fname = copy_n(inner, name_len);
            const char *rest = inner + name_len;
            while (isspace((unsigned char)*rest))
                rest++;
            bool found;
            char *r = call_function(fname, rest, &found);
            free(fname);
            if (found) {
                add_str(&t, r);
                free(r);
                free(inner);
                continue;
            }
        }
        /* $(NAME) or $(NAME:A=B), NAME itself expanded first (computed names). */
        char *name_part = inner, *subst = 0;
        int depth = 0;
        for (char *p = inner; *p; p++) {
            if (*p == '(' || *p == '{')
                depth++;
            else if (*p == ')' || *p == '}')
                depth--;
            else if (*p == ':' && depth == 0 && strchr(p, '=')) {
                *p = 0;
                subst = p + 1;
                break;
            }
        }
        char *name = expand(name_part);
        char *value = variable_value(name);
        if (subst) {
            char *sub = expand(subst), *eq = strchr(sub, '=');
            *eq = 0;
            char *from = sub, *to = eq + 1;
            char *f2, *t2;
            if (strchr(from, '%')) {
                f2 = copy(from);
                t2 = copy(to);
            } else {                                /* $(VAR:.c=.o): %.c=%.o */
                f2 = must(malloc(strlen(from) + 2));
                t2 = must(malloc(strlen(to) + 2));
                snprintf(f2, strlen(from) + 2, "%%%s", from);
                snprintf(t2, strlen(to) + 2, "%%%s", to);
            }
            char *r = patsubst(f2, t2, value);
            free(value);
            value = r;
            free(f2), free(t2), free(sub);
        }
        add_str(&t, value);
        free(value), free(name), free(inner);
    }
    return done(&t);
}

/* --- rules -------------------------------------------------------------------------------- */

struct target {
    char *name;
    struct words prereqs, order_only;
    char **recipe;                      /* lines as written (expanded when run) */
    int recipe_count;
    bool has_recipe, phony, precious;
    bool explicit_rule;                 /* named as a rule's target */
    struct words target_vars;           /* target: NAME OP VALUE, as "OP\001NAME\001VALUE" */
    bool fresh;                         /* a new rule line: its recipe lines replace the old */
    char *static_stem;                  /* (a static pattern rule's) */
    /* while making */
    int state;                          /* 0 new, 1 being made, 2 done */
    bool failed, remade;
    long long time;                     /* -1: missing */
    struct target *next;
};

struct pattern_rule {
    struct words targets, prereqs, order_only;
    char **recipe;
    int recipe_count;
    bool builtin;
    struct pattern_rule *next;
};

static struct target *targets[BUCKETS];
static struct pattern_rule *patterns, **patterns_tail = &patterns;
static char *default_goal;
static struct target *default_rule;     /* .DEFAULT */
static struct words suffixes;
static bool all_silent, ignore_all;

static struct target *find_target(const char *name)
{
    for (struct target *t = targets[hash(name)]; t; t = t->next)
        if (!strcmp(t->name, name))
            return t;
    return 0;
}

static struct target *get_target(const char *name)
{
    struct target *t = find_target(name);
    if (t)
        return t;
    t = must(calloc(1, sizeof(*t)));
    t->name = copy(name);
    unsigned h = hash(name);
    t->next = targets[h];
    targets[h] = t;
    return t;
}

/* --- parsing ------------------------------------------------------------------------------ */

struct conditional {
    bool active, seen_true, in_else;
};

static struct conditional conds[64];
static int cond_depth;

static bool skipping(void)
{
    for (int i = 0; i < cond_depth; i++)
        if (!conds[i].active)
            return true;
    return false;
}

/* The targets of the last rule line (for its recipe lines), or none. */
static struct target **rule_targets;
static int rule_target_count;
static struct pattern_rule *rule_pattern;

/* A recipe line keeps where it was written (for messages from its
 * expansion): "FILE\001LINE\001TEXT". */
static char *placed(const char *line)
{
    size_t n = strlen(line) + strlen(current_file ? current_file : "") + 24;
    char *r = must(malloc(n));
    snprintf(r, n, "%s\001%d\001%s", current_file ? current_file : "", current_line, line);
    return r;
}

static void add_recipe_line(const char *line)
{
    if (rule_pattern) {
        rule_pattern->recipe = must(realloc(rule_pattern->recipe,
                                            sizeof(char *) * (size_t)(rule_pattern->recipe_count + 1)));
        rule_pattern->recipe[rule_pattern->recipe_count++] = placed(line);
        return;
    }
    for (int i = 0; i < rule_target_count; i++) {
        struct target *t = rule_targets[i];
        if (t->fresh) {                             /* (a second recipe replaces the first) */
            for (int k = 0; k < t->recipe_count; k++)
                free(t->recipe[k]);
            t->recipe_count = 0;
            t->fresh = false;
        }
        t->recipe = must(realloc(t->recipe, sizeof(char *) * (size_t)(t->recipe_count + 1)));
        t->recipe[t->recipe_count++] = placed(line);
        t->has_recipe = true;
    }
}

/* ifeq's two arguments: (a,b), "a" "b" or 'a' 'b'. */
static bool equal_arguments(const char *s, bool *ok)
{
    *ok = true;
    while (isspace((unsigned char)*s))
        s++;
    char *a, *b;
    if (*s == '(') {
        const char *end = reference_end(s + 1, '(');
        if (!end) {
            *ok = false;
            return false;
        }
        char *inner = copy_n(s + 1, (size_t)(end - s - 1));
        struct words args = arguments(inner, 2);
        free(inner);
        char *x = args.n > 0 ? expand(args.w[0]) : copy(""), *y = args.n > 1 ? expand(args.w[1]) : copy("");
        /* (leading blanks of the second and trailing of the first go, as GNU) */
        a = x;
        b = y;
        size_t n = strlen(a);
        while (n && isspace((unsigned char)a[n - 1]))
            a[--n] = 0;
        char *bb = b;
        while (isspace((unsigned char)*bb))
            bb++;
        bool r = !strcmp(a, bb);
        free(x), free(y);
        free_words(&args);
        return r;
    }
    char q1 = *s;
    if (q1 != '"' && q1 != '\'') {
        *ok = false;
        return false;
    }
    const char *e1 = strchr(s + 1, q1);
    if (!e1) {
        *ok = false;
        return false;
    }
    char *x = copy_n(s + 1, (size_t)(e1 - s - 1));
    s = e1 + 1;
    while (isspace((unsigned char)*s))
        s++;
    char q2 = *s;
    const char *e2 = q2 == '"' || q2 == '\'' ? strchr(s + 1, q2) : 0;
    if (!e2) {
        free(x);
        *ok = false;
        return false;
    }
    char *y = copy_n(s + 1, (size_t)(e2 - s - 1));
    a = expand(x);
    b = expand(y);
    bool r = !strcmp(a, b);
    free(x), free(y), free(a), free(b);
    return r;
}

/* A directive (ifeq... include, export, override, define): true if line was one. */
static bool directive(char *line, FILE *in, const char *name);

static void parse_line(char *line, FILE *in, const char *name);

static void parse_makefile(FILE *in, const char *name);

static void include_file(const char *path, bool optional)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        if (!optional)
            fatal("%s: %s", path, strerror(errno));
        return;
    }
    const char *saved_file = current_file;
    int saved_line = current_line;
    struct target **saved_targets = rule_targets;
    int saved_count = rule_target_count;
    struct pattern_rule *saved_pattern = rule_pattern;
    rule_targets = 0;
    rule_target_count = 0;
    rule_pattern = 0;
    char *keep = copy(path);
    parse_makefile(f, keep);
    fclose(f);
    current_file = saved_file;
    current_line = saved_line;
    rule_targets = saved_targets;
    rule_target_count = saved_count;
    rule_pattern = saved_pattern;
    struct variable *list = find_variable("MAKEFILE_LIST");
    struct text t = { 0 };
    add_str(&t, list ? list->value : "");
    add_char(&t, ' ');
    add_str(&t, path);
    set_variable("MAKEFILE_LIST", t.s, true, O_FILE);
    free(t.s);
}

/* Read one logical line (a backslash joins the next); false at the end. */
static bool read_line(FILE *in, struct text *line, bool *recipe)
{
    line->n = 0;
    done(line);
    line->s[0] = 0;                                 /* (an empty line is empty) */
    int ch;
    bool any = false, first = true;
    *recipe = false;
    while ((ch = fgetc(in)) != EOF) {
        any = true;
        if (first) {
            first = false;
            *recipe = ch == '\t';
        }
        if (ch == '\n') {
            current_line++;
            if (line->n && line->s[line->n - 1] == '\\' &&
                !(line->n > 1 && line->s[line->n - 2] == '\\' && !*recipe)) {
                if (*recipe) {
                    add_char(line, '\n');           /* (the shell sees backslash-newline) */
                    int next = fgetc(in);
                    if (next != '\t' && next != EOF)
                        ungetc(next, in);
                } else {
                    line->s[--line->n] = 0;         /* \ newline blanks: one blank */
                    while (line->n && isspace((unsigned char)line->s[line->n - 1]))
                        line->s[--line->n] = 0;
                    add_char(line, ' ');
                    int next;
                    while ((next = fgetc(in)) == ' ' || next == '\t')
                        ;
                    if (next != EOF)
                        ungetc(next, in);
                }
                continue;
            }
            return true;
        }
        add_char(line, (char)ch);
    }
    if (any)
        current_line++;
    return any;
}

/* A line with its comment (outside $(...)) taken away. */
static void strip_comment(char *s)
{
    int depth = 0;
    for (char *p = s; *p; p++) {
        if (*p == '\\' && (p[1] == '#' || p[1] == '\\')) {
            if (p[1] == '#')
                memmove(p, p + 1, strlen(p));      /* \# is a # */
            continue;
        }
        if (*p == '$' && (p[1] == '(' || p[1] == '{')) {
            depth++;
            p++;
            continue;
        }
        if ((*p == ')' || *p == '}') && depth)
            depth--;
        else if (*p == '#' && depth == 0) {
            *p = 0;
            return;
        }
    }
}

static void assign(char *name, char *op, const char *value, int origin, bool exported)
{
    char *n = trim(name);
    if (!*n)
        fatal("empty variable name");
    char *v = trim(value);
    /* (a value's leading blanks go, its trailing ones stay: GNU keeps them) */
    const char *raw = value;
    while (isspace((unsigned char)*raw))
        raw++;
    free(v);
    v = copy(raw);
    struct variable *var;
    if (!strcmp(op, ":=") || !strcmp(op, "::=")) {
        char *e = expand(v);
        var = set_variable(n, e, true, origin);
        free(e);
    } else if (!strcmp(op, "?=")) {
        var = find_variable(n);
        if (!var)
            var = set_variable(n, v, false, origin);
    } else if (!strcmp(op, "+=")) {
        var = find_variable(n);
        if (!var) {
            var = set_variable(n, v, false, origin);
        } else {
            char *add_value = var->simple ? expand(v) : copy(v);
            struct text t = { 0 };
            add_str(&t, var->value);
            if (t.n && *add_value)
                add_char(&t, ' ');
            add_str(&t, add_value);
            bool simple = var->simple;
            int keep = var->origin > origin ? var->origin : origin;
            var = set_variable(n, t.s, simple, keep);
            free(t.s), free(add_value);
        }
    } else if (!strcmp(op, "!=")) {
        char *e = expand(v), *out = run_shell(e);
        var = set_variable(n, out, true, origin);
        free(e), free(out);
    } else {
        var = set_variable(n, v, false, origin);
    }
    if (exported && var)
        var->exported = true;
    free(n), free(v);
}

/* Is line an assignment? The operator's place and text. */
static char *assignment_op(char *line, char **op_text)
{
    int depth = 0;
    for (char *p = line; *p; p++) {
        if (*p == '$' && (p[1] == '(' || p[1] == '{')) {
            depth++;
            p++;
            continue;
        }
        if ((*p == ')' || *p == '}') && depth) {
            depth--;
            continue;
        }
        if (depth)
            continue;
        if (*p == '=') {
            if (p > line && strchr(":?+!", p[-1])) {
                if (p[-1] == ':' && p - 1 > line && p[-2] == ':') {
                    *op_text = "::=";
                    return p - 2;
                }
                *op_text = p[-1] == ':' ? ":=" : p[-1] == '?' ? "?=" : p[-1] == '+' ? "+=" : "!=";
                return p - 1;
            }
            *op_text = "=";
            return p;
        }
        if (*p == ':' && p[1] != '=' && !(p[1] == ':' && p[2] == '='))
            return 0;                               /* a rule */
        if (*p == ';')
            return 0;
    }
    return 0;
}

static void add_pattern_rule(struct words *tws, struct words *pws, struct words *ows, const char *inline_recipe)
{
    struct pattern_rule *r = must(calloc(1, sizeof(*r)));
    r->targets = *tws;
    r->prereqs = *pws;
    r->order_only = *ows;
    *patterns_tail = r;
    patterns_tail = &r->next;
    rule_pattern = r;
    rule_target_count = 0;
    if (inline_recipe)
        add_recipe_line(inline_recipe);
}

static bool is_suffix(const char *s)
{
    for (int i = 0; i < suffixes.n; i++)
        if (!strcmp(suffixes.w[i], s))
            return true;
    return false;
}

/* .c.o or .c: a suffix rule, as the pattern rule it means. */
static bool suffix_rule(const char *name, struct words *tws, struct words *pws)
{
    if (name[0] != '.' || strchr(name, '/'))
        return false;
    for (int i = 0; i < suffixes.n; i++) {
        size_t n = strlen(suffixes.w[i]);
        if (strncmp(name, suffixes.w[i], n))
            continue;
        const char *rest = name + n;
        if (!*rest || is_suffix(rest)) {
            char *t = must(malloc(strlen(rest) + 2)), *p = must(malloc(n + 2));
            snprintf(t, strlen(rest) + 2, "%%%s", rest);
            snprintf(p, n + 2, "%%%s", suffixes.w[i]);
            push_word(tws, t);
            push_word(pws, p);
            return true;
        }
    }
    return false;
}

static void rule_line(char *line)
{
    /* The targets end at the first ':' outside references. */
    int depth = 0;
    char *colon = 0;
    for (char *p = line; *p; p++) {
        if (*p == '$' && (p[1] == '(' || p[1] == '{')) {
            depth++;
            p++;
        } else if ((*p == ')' || *p == '}') && depth) {
            depth--;
        } else if (*p == ':' && !depth) {
            colon = p;
            break;
        }
    }
    if (!colon) {
        char *t = trim(line);
        bool empty = !*t;
        free(t);
        if (!empty) {
            char *e = expand(line), *et = trim(e);
            bool still = *et != 0;
            free(e), free(et);
            if (still)
                fatal("missing separator");
        }
        return;
    }
    *colon = 0;
    char *rest = colon + 1;
    if (*rest == ':')
        rest++;                                     /* (:: as :) */
    char *inline_recipe = 0;
    depth = 0;
    for (char *p = rest; *p; p++) {
        if (*p == '$' && (p[1] == '(' || p[1] == '{')) {
            depth++;
            p++;
        } else if ((*p == ')' || *p == '}') && depth) {
            depth--;
        } else if (*p == ';' && !depth) {
            *p = 0;
            inline_recipe = p + 1;
            break;
        }
    }
    char *tex = expand(line);
    struct words tws = split_words(tex);
    free(tex);
    /* A target-specific variable (target: VAR = value): taken as an assignment for all. */
    char *op;
    char *where = assignment_op(rest, &op);
    if (where && !inline_recipe) {                  /* target: NAME = VALUE, for its recipe */
        char saved = *where;
        *where = 0;
        char *name = trim(rest);
        *where = saved;
        const char *value = where + strlen(op);
        while (isspace((unsigned char)*value))
            value++;
        for (int i = 0; i < tws.n; i++) {
            struct target *t = get_target(tws.w[i]);
            size_t n = strlen(op) + strlen(name) + strlen(value) + 3;
            char *entry = must(malloc(n));
            snprintf(entry, n, "%s\001%s\001%s", op, name, value);
            push_word(&t->target_vars, entry);
        }
        free(name);
        free_words(&tws);
        rule_target_count = 0;
        rule_pattern = 0;
        return;
    }
    /* A static pattern rule: targets: target-pattern: prereq-patterns. */
    char *second = 0;
    depth = 0;
    for (char *p = rest; *p; p++) {
        if (*p == '$' && (p[1] == '(' || p[1] == '{')) {
            depth++;
            p++;
        } else if ((*p == ')' || *p == '}') && depth) {
            depth--;
        } else if (*p == ':' && !depth) {
            *p = 0;
            second = p + 1;
            break;
        }
    }
    char *pex = expand(second ? second : rest);
    char *bar = strchr(pex, '|');
    char *order = 0;
    if (bar) {
        *bar = 0;
        order = bar + 1;
    }
    struct words pws = split_words(pex), ows = split_words(order ? order : "");
    free(pex);
    char *target_pattern = 0;
    if (second) {
        char *tp = expand(rest);
        target_pattern = trim(tp);
        free(tp);
    }
    free(rule_targets);
    rule_targets = 0;
    rule_target_count = 0;
    rule_pattern = 0;
    if (!tws.n) {
        free_words(&pws), free_words(&ows), free(target_pattern);
        return;
    }
    /* Special targets. */
    const char *first = tws.w[0];
    if (!strcmp(first, ".PHONY") || !strcmp(first, ".PRECIOUS") || !strcmp(first, ".SECONDARY") ||
        !strcmp(first, ".INTERMEDIATE") || !strcmp(first, ".NOTPARALLEL") ||
        !strcmp(first, ".DELETE_ON_ERROR") || !strcmp(first, ".POSIX") ||
        !strcmp(first, ".SECONDEXPANSION") || !strcmp(first, ".EXPORT_ALL_VARIABLES") ||
        !strcmp(first, ".ONESHELL")) {
        bool phony = !strcmp(first, ".PHONY");
        for (int i = 0; i < pws.n; i++) {
            struct target *t = get_target(pws.w[i]);
            if (phony)
                t->phony = true;
            else if (!strcmp(first, ".PRECIOUS"))
                t->precious = true;
        }
        free_words(&tws), free_words(&pws), free_words(&ows), free(target_pattern);
        return;
    }
    if (!strcmp(first, ".SUFFIXES")) {
        if (!pws.n)
            free_words(&suffixes);
        for (int i = 0; i < pws.n; i++)
            push_word(&suffixes, copy(pws.w[i]));
        free_words(&tws), free_words(&pws), free_words(&ows), free(target_pattern);
        return;
    }
    if (!strcmp(first, ".SILENT") || !strcmp(first, ".IGNORE")) {
        if (!pws.n) {
            if (!strcmp(first, ".SILENT"))
                all_silent = true;
            else
                ignore_all = true;
        }
        free_words(&tws), free_words(&pws), free_words(&ows), free(target_pattern);
        return;
    }
    if (!strcmp(first, ".DEFAULT")) {
        default_rule = get_target(".DEFAULT");
        rule_targets = must(malloc(sizeof(*rule_targets)));
        rule_targets[0] = default_rule;
        rule_target_count = 1;
        if (inline_recipe)
            add_recipe_line(inline_recipe);
        free_words(&tws), free_words(&pws), free_words(&ows), free(target_pattern);
        return;
    }
    /* Suffix rules and pattern rules. */
    struct words st = { 0 }, sp = { 0 };
    if (tws.n == 1 && !pws.n && !second && suffix_rule(first, &st, &sp)) {
        free_words(&tws);
        add_pattern_rule(&st, &sp, &ows, inline_recipe);
        free_words(&pws);
        return;
    }
    if (!second && strchr(first, '%')) {
        add_pattern_rule(&tws, &pws, &ows, inline_recipe);
        return;
    }
    rule_targets = must(malloc(sizeof(*rule_targets) * (size_t)tws.n));
    for (int i = 0; i < tws.n; i++) {
        struct target *t = get_target(tws.w[i]);
        rule_targets[rule_target_count++] = t;
        t->explicit_rule = true;
        t->fresh = true;
        char *stem = 0;
        if (second && !pattern_match(target_pattern, tws.w[i], &stem)) {
            message("target '%s' doesn't match the target pattern", tws.w[i]);
            continue;
        }
        for (int k = 0; k < pws.n; k++) {
            char *p = second ? pattern_replace(pws.w[k], stem) : copy(pws.w[k]);
            push_word(&t->prereqs, p);
        }
        for (int k = 0; k < ows.n; k++)
            push_word(&t->order_only, copy(ows.w[k]));
        if (stem) {
            free(t->static_stem);
            t->static_stem = stem;
        }
        if (!default_goal && tws.w[i][0] != '.' )
            default_goal = copy(tws.w[i]);
        else if (!default_goal && strchr(tws.w[i], '/'))
            default_goal = copy(tws.w[i]);
    }
    if (inline_recipe)
        add_recipe_line(inline_recipe);
    free_words(&tws), free_words(&pws), free_words(&ows), free(target_pattern);
}

static bool directive(char *line, FILE *in, const char *name)
{
    char *s = line;
    while (*s == ' ' || *s == '\t')
        s++;
    size_t n = 0;
    while (s[n] && !isspace((unsigned char)s[n]) && s[n] != '(')
        n++;
    char *word = copy_n(s, n);
    char *rest = s + n;
    while (isspace((unsigned char)*rest))
        rest++;
#define WORD(w) (!strcmp(word, w))
    bool r = true;
    if (WORD("ifeq") || WORD("ifneq") || WORD("ifdef") || WORD("ifndef")) {
        if (cond_depth == 64)
            fatal("conditionals nested too deep");
        bool value = false;
        if (!skipping()) {
            if (WORD("ifdef") || WORD("ifndef")) {
                char *e = expand(rest), *t = trim(e);
                struct variable *v = find_variable(t);
                value = v && v->value[0];
                if (WORD("ifndef"))
                    value = !value;
                free(e), free(t);
            } else {
                bool ok;
                value = equal_arguments(rest, &ok);
                if (!ok)
                    fatal("invalid syntax in conditional");
                if (WORD("ifneq"))
                    value = !value;
            }
        }
        conds[cond_depth++] = (struct conditional){ value, value, false };
    } else if (WORD("else")) {
        if (!cond_depth)
            fatal("extraneous 'else'");
        struct conditional *c = &conds[cond_depth - 1];
        if (c->in_else && !*rest)
            fatal("only one 'else' per conditional");
        if (!*rest) {
            c->active = !c->seen_true;
            c->in_else = true;
        } else {                                    /* else ifeq ... */
            char *inner = rest;
            size_t k = 0;
            while (inner[k] && !isspace((unsigned char)inner[k]) && inner[k] != '(')
                k++;
            char *kw = copy_n(inner, k);
            char *args = inner + k;
            bool value = false;
            cond_depth--;
            bool outer_skip = skipping();
            cond_depth++;
            if (!c->seen_true && !outer_skip) {
                if (!strcmp(kw, "ifdef") || !strcmp(kw, "ifndef")) {
                    char *e = expand(args), *t = trim(e);
                    struct variable *v = find_variable(t);
                    value = v && v->value[0];
                    if (!strcmp(kw, "ifndef"))
                        value = !value;
                    free(e), free(t);
                } else {
                    bool ok;
                    value = equal_arguments(args, &ok);
                    if (!ok)
                        fatal("invalid syntax in conditional");
                    if (!strcmp(kw, "ifneq"))
                        value = !value;
                }
            }
            c->active = value && !c->seen_true;
            if (value)
                c->seen_true = true;
            free(kw);
        }
    } else if (WORD("endif")) {
        if (!cond_depth)
            fatal("extraneous 'endif'");
        cond_depth--;
    } else if (skipping()) {
        /* (inside a false branch only the nesting counts) */
        if (WORD("define")) {
            struct text l = { 0 };
            bool recipe;
            int nest = 1;
            while (nest && read_line(in, &l, &recipe)) {
                char *t = trim(l.s);
                if (!strncmp(t, "define", 6) && (!t[6] || isspace((unsigned char)t[6])))
                    nest++;
                else if (!strcmp(t, "endef"))
                    nest--;
                free(t);
            }
            free(l.s);
        }
    } else if (WORD("include") || WORD("-include") || WORD("sinclude")) {
        char *e = expand(rest);
        struct words files = split_words(e);
        free(e);
        for (int i = 0; i < files.n; i++) {
            if (strpbrk(files.w[i], "*?[")) {
                char *w = wildcard(files.w[i]);
                struct words ws = split_words(w);
                for (int k = 0; k < ws.n; k++)
                    include_file(ws.w[k], !WORD("include"));
                free_words(&ws);
                free(w);
            } else {
                include_file(files.w[i], !WORD("include"));
            }
        }
        free_words(&files);
    } else if (WORD("define")) {
        char *spec = trim(rest), *op = "=";
        size_t k = strlen(spec);
        if (k >= 2 && !strcmp(spec + k - 2, ":="))
            op = ":=", spec[k - 2] = 0;
        else if (k >= 1 && spec[k - 1] == '=')
            spec[k - 1] = 0;
        else if (k >= 2 && !strcmp(spec + k - 2, "+="))
            op = "+=", spec[k - 2] = 0;
        struct text body = { 0 }, l = { 0 };
        bool recipe;
        int nest = 1;
        bool first = true;
        while (read_line(in, &l, &recipe)) {
            char *t = trim(l.s);
            if (!strncmp(t, "define", 6) && (!t[6] || isspace((unsigned char)t[6])))
                nest++;
            else if (!strcmp(t, "endef") && --nest == 0) {
                free(t);
                break;
            }
            free(t);
            if (!first)
                add_char(&body, '\n');
            add_str(&body, l.s);
            first = false;
        }
        free(l.s);
        assign(spec, op, done(&body), O_FILE, false);
        free(body.s);
        free(spec);
    } else if (WORD("override")) {
        char *op;
        char *where = assignment_op(rest, &op);
        if (!where)
            fatal("override without an assignment");
        char saved = *where;
        *where = 0;
        char *vname = copy(rest);
        *where = saved;
        assign(vname, op, where + strlen(op), O_OVERRIDE, false);
        free(vname);
    } else if (WORD("export") || WORD("unexport")) {
        char *op;
        char *where = assignment_op(rest, &op);
        if (where && WORD("export")) {
            char saved = *where;
            *where = 0;
            char *vname = copy(rest);
            *where = saved;
            assign(vname, op, where + strlen(op), O_FILE, true);
            free(vname);
        } else {
            char *e = expand(rest);
            struct words ws = split_words(e);
            for (int i = 0; i < ws.n; i++) {
                struct variable *v = find_variable(ws.w[i]);
                if (!v)
                    v = set_variable(ws.w[i], "", false, O_FILE);
                v->exported = WORD("export");
            }
            free_words(&ws);
            free(e);
        }
    } else if (WORD("vpath")) {
        /* (taken, not used) */
    } else {
        r = false;
    }
#undef WORD
    (void)name;
    free(word);
    return r;
}

static void parse_line(char *line, FILE *in, const char *name)
{
    if (directive(line, in, name))
        return;
    if (skipping())
        return;
    char *op;
    char *where = assignment_op(line, &op);
    if (where) {
        char saved = *where;
        *where = 0;
        char *vname = copy(line);
        *where = saved;
        assign(vname, op, where + strlen(op), O_FILE, false);
        free(vname);
        rule_target_count = 0;
        rule_pattern = 0;
        return;
    }
    rule_line(line);
}

static void parse_makefile(FILE *in, const char *name)
{
    current_file = name;
    current_line = 0;
    int depth_at_start = cond_depth;
    struct text line = { 0 };
    bool recipe;
    while (read_line(in, &line, &recipe)) {
        if (recipe && (rule_target_count || rule_pattern)) {
            if (!skipping())
                add_recipe_line(line.s + 1);
            continue;
        }
        strip_comment(line.s);
        char *t = trim(line.s);
        bool empty = !*t;
        free(t);
        if (empty)
            continue;
        parse_line(line.s, in, name);
    }
    free(line.s);
    if (cond_depth != depth_at_start)
        fatal("missing 'endif'");
}

static void parse_makefile_text(const char *text, const char *name)
{
    /* $(eval): the text as Makefile lines (through a temporary file). */
    FILE *f = tmpfile();
    if (!f)
        fatal("$(eval): %s", strerror(errno));
    fputs(text, f);
    fputc('\n', f);
    rewind(f);
    const char *saved_file = current_file;
    int saved_line = current_line;
    parse_makefile(f, name);
    current_file = saved_file;
    current_line = saved_line;
    fclose(f);
}

/* --- making --------------------------------------------------------------------------------- */

static bool dry_run, silent, keep_going, ignore_errors, question, always_make, touch_only;
static bool print_directory = true;
static char *make_path;                 /* $(MAKE) */
static int errors_seen;
static long commands_run;               /* recipe lines run (or shown, with -n) */

static long long file_time(const char *name)
{
    struct stat st;
    return stat(name, &st) == 0 ? (long long)st.st_mtime : -1;
}

static void set_environment(void)
{
    for (int i = 0; i < BUCKETS; i++)
        for (struct variable *v = variables[i]; v; v = v->next)
            if (v->exported || v->origin == O_COMMAND) {
                char *value = v->simple ? copy(v->value) : expand(v->value);
                setenv(v->name, value, 1);
                free(value);
            }
}

static bool run_one(struct target *t, const char *raw, char *line);

/* Run one recipe line for t; false if it failed (and that counts). A line
 * that expands to several (a define'd recipe) runs as several, each with
 * its own @ - +. */
static bool run_line(struct target *t, const char *recipe_line)
{
    const char *sep = strchr(recipe_line, '\001'), *raw = recipe_line;
    const char *saved_file = current_file;
    int saved_line = current_line;
    char *file = 0;
    if (sep) {
        file = copy_n(recipe_line, (size_t)(sep - recipe_line));
        current_line = atoi(sep + 1);
        raw = strchr(sep + 1, '\001') + 1;
        current_file = *file ? file : 0;
    }
    char *all = expand(raw);
    current_file = saved_file;
    current_line = saved_line;
    bool ok = true;
    char *start = all;
    for (char *p = all;; p++) {
        if (*p == '\n' && (p == all || p[-1] != '\\')) {
            *p = 0;
            ok = run_one(t, raw, copy(start));
            start = p + 1;
            if (!ok)
                break;
            continue;
        }
        if (!*p) {
            ok = run_one(t, raw, copy(start));
            break;
        }
    }
    free(all);
    free(file);
    return ok;
}

static bool run_one(struct target *t, const char *raw, char *line)
{
    const char *s = line;
    bool quiet = silent || all_silent, ignore = ignore_errors || ignore_all, always = false;
    for (;; s++) {
        if (*s == '@')
            quiet = true;
        else if (*s == '-')
            ignore = true;
        else if (*s == '+')
            always = true;
        else if (*s != ' ' && *s != '\t')
            break;
    }
    if (!*s) {
        free(line);
        return true;
    }
    bool recursive = strstr(raw, "$(MAKE)") || strstr(raw, "${MAKE}");
    commands_run++;
    if (!quiet || dry_run) {                        /* (-n shows @ lines too, $(MAKE) ones as well) */
        printf("%s\n", s);
        fflush(stdout);
    }
    if (dry_run && !always && !recursive) {
        free(line);
        return true;
    }
    fflush(stdout);
    set_environment();
    int status = system(s);
    free(line);
    if (status == 0)
        return true;
    int code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    if (ignore) {
        message("[%s] Error %d (ignored)", t->name, code);
        return true;
    }
    message("*** [%s] Error %d", t->name, code);
    return false;
}

static struct target *make_target(const char *name, const char *needed_by, int depth);

/* An implicit rule for t: a pattern rule whose prerequisites exist or can be
 * made. Its stem and prerequisites in *stem and *prereqs. */
static struct pattern_rule *implicit_rule(struct target *t, char **stem, struct words *prereqs,
                                          int depth)
{
    if (depth > 4)
        return 0;
    for (struct pattern_rule *r = patterns; r; r = r->next) {
        if (!r->recipe_count && !r->prereqs.n)
            continue;
        for (int k = 0; k < r->targets.n; k++) {
            char *s;
            if (!pattern_match(r->targets.w[k], t->name, &s))
                continue;
            /* A "match anything" rule (%) only for files without a suffix rule's dot. */
            bool ok = true;
            struct words ps = { 0 };
            for (int i = 0; i < r->prereqs.n && ok; i++) {
                char *p = pattern_replace(r->prereqs.w[i], s);
                struct target *known = find_target(p);
                if (file_time(p) < 0 && !(known && (known->has_recipe || known->phony))) {
                    /* Can a pattern rule make it (one step further)? */
                    struct target probe = { .name = p };
                    char *s2;
                    struct words p2 = { 0 };
                    struct pattern_rule *chain = implicit_rule(&probe, &s2, &p2, depth + 1);
                    if (!chain || chain == r)
                        ok = false;
                    if (chain) {
                        free(s2);
                        free_words(&p2);
                    }
                }
                push_word(&ps, p);
            }
            if (ok) {
                *stem = s;
                *prereqs = ps;
                return r;
            }
            free(s);
            free_words(&ps);
        }
    }
    return 0;
}

static struct target *make_target(const char *name, const char *needed_by, int depth)
{
    struct target *t = get_target(name);
    if (t->state == 2)
        return t;
    if (t->state == 1) {
        message("Circular %s <- %s dependency dropped.", needed_by ? needed_by : "", name);
        return t;
    }
    t->state = 1;
    t->time = file_time(name);
    /* The rule: its own recipe, an implicit one, or .DEFAULT's. */
    struct pattern_rule *rule = 0;
    char *stem = 0;
    struct words implicit_prereqs = { 0 };
    char **recipe = t->recipe;
    int recipe_count = t->recipe_count;
    if (!t->has_recipe && !t->phony) {
        rule = implicit_rule(t, &stem, &implicit_prereqs, 0);
        if (rule) {
            recipe = rule->recipe;
            recipe_count = rule->recipe_count;
        }
    }
    if (t->static_stem && !stem)
        stem = copy(t->static_stem);
    if (!stem) {
        /* $*: the name without a known suffix, as POSIX for suffix rules */
        const char *dot = strrchr(name, '.');
        stem = dot && !strchr(dot, '/') ? copy_n(name, (size_t)(dot - name)) : copy("");
    }
    struct words all = { 0 }, dups = { 0 };
    for (int i = 0; i < implicit_prereqs.n; i++)
        push_word(&all, copy(implicit_prereqs.w[i]));
    for (int i = 0; i < t->prereqs.n; i++)
        push_word(&all, copy(t->prereqs.w[i]));
    if (!t->has_recipe && !rule && !t->phony && t->time < 0 && !all.n && t->explicit_rule) {
        t->remade = true;                           /* (FORCE: and the like: always new) */
        t->state = 2;
        free(stem);
        free_words(&all);
        free_words(&implicit_prereqs);
        return t;
    }
    if (!t->has_recipe && !rule && !t->phony && t->time < 0 && !all.n) {
        if (default_rule && default_rule->has_recipe) {
            recipe = default_rule->recipe;
            recipe_count = default_rule->recipe_count;
        } else {
            if (needed_by)
                message("*** No rule to make target '%s', needed by '%s'.  Stop.", name, needed_by);
            else
                message("*** No rule to make target '%s'.  Stop.", name);
            t->failed = true;
            t->state = 2;
            errors_seen++;
            if (!keep_going)
                exit(2);
            free(stem);
            free_words(&all);
            free_words(&implicit_prereqs);
            return t;
        }
    }
    /* The prerequisites first. */
    bool out_of_date = t->time < 0 || t->phony || always_make, failed = false;
    struct text newer = { 0 }, unique = { 0 };
    for (int i = 0; i < all.n; i++) {
        struct target *p = make_target(all.w[i], name, depth + 1);
        if (p->failed) {
            failed = true;
            if (!keep_going)
                break;
            continue;
        }
        long long pt = p->time;
        bool is_newer = t->time < 0 || p->phony || pt < 0 || pt > t->time || p->remade;
        if (is_newer) {
            out_of_date = true;
            if (newer.n)
                add_char(&newer, ' ');
            add_str(&newer, all.w[i]);
        }
        bool seen = false;
        for (int k = 0; k < i; k++)
            seen |= !strcmp(all.w[k], all.w[i]);
        if (!seen) {
            if (unique.n)
                add_char(&unique, ' ');
            add_str(&unique, all.w[i]);
        }
        push_word(&dups, copy(all.w[i]));
    }
    for (int i = 0; i < t->order_only.n && !failed; i++) {
        struct target *p = make_target(t->order_only.w[i], name, depth + 1);
        if (p->failed)
            failed = true;
    }
    if (failed) {
        t->failed = true;
        t->state = 2;
        if (!keep_going)
            exit(2);
        message("Target '%s' not remade because of errors.", name);
        free(stem), free_words(&all), free_words(&dups), free(newer.s), free(unique.s);
        free_words(&implicit_prereqs);
        return t;
    }
    if (out_of_date && recipe_count) {
        if (question) {
            exit(1);
        }
        struct automatic a = { name, all.n ? all.w[0] : "", stem, done(&unique),
                               join_words(&dups, 0, dups.n), done(&newer),
                               join_words(&t->order_only, 0, t->order_only.n) };
        struct automatic *saved = automatic;
        automatic = &a;
        /* Its target-specific variables while its recipe runs (GNU also gives
         * them to its prerequisites: not here). */
        struct words old_values = { 0 }, old_names = { 0 };
        for (int i = 0; i < t->target_vars.n; i++) {
            char *e = copy(t->target_vars.w[i]), *n = strchr(e, '\001') + 1, *v = strchr(n, '\001') + 1;
            n[-1] = 0;
            v[-1] = 0;
            struct variable *old = find_variable(n);
            push_word(&old_names, copy(n));
            push_word(&old_values, old ? copy(old->value) : 0);
            assign(n, e, v, O_OVERRIDE - 1, false);     /* (over the file's, under override) */
            free(e);
        }
        if (touch_only && !t->phony) {
            if (!silent)
                printf("touch %s\n", name);
            if (!dry_run) {
                int fd = open(name, O_RDWR | O_CREAT, 0666);
                char c;
                if (fd >= 0 && read(fd, &c, 1) == 1) {
                    lseek(fd, 0, SEEK_SET);
                    write(fd, &c, 1);
                }
                if (fd >= 0)
                    close(fd);
            }
        } else {
            for (int i = 0; i < recipe_count; i++)
                if (!run_line(t, recipe[i])) {
                    t->failed = true;
                    errors_seen++;
                    break;
                }
        }
        for (int i = old_names.n - 1; i >= 0; i--) {
            struct variable *v = find_variable(old_names.w[i]);
            if (v) {
                free(v->value);
                v->value = copy(old_values.w[i] ? old_values.w[i] : "");
            }
            free(old_values.w[i]);
        }
        free(old_values.w);
        free_words(&old_names);
        automatic = saved;
        free(a.all_dups);
        free(a.order_only);
        t->remade = true;
        if (t->failed) {
            t->state = 2;
            if (!keep_going) {
                fflush(stdout);
                exit(2);
            }
        }
    } else if (out_of_date && !t->phony && t->time < 0 && !recipe_count && rule) {
        t->remade = true;
    } else if (out_of_date && !recipe_count && all.n) {
        t->remade = t->time < 0 || t->phony;
    }
    long long now = file_time(name);
    t->time = now >= 0 ? now : t->time;
    if (t->remade && now < 0)
        t->time = -1;
    t->state = 2;
    free(stem), free_words(&all), free_words(&dups), free(newer.s), free(unique.s);
    free_words(&implicit_prereqs);
    return t;
}

/* --- the start -------------------------------------------------------------------------------- */

static void builtin_rules(void)
{
    static const char *const defaults[][2] = {
        { "CC", "cc" }, { "AR", "ar" }, { "ARFLAGS", "rv" }, { "AS", "as" }, { "RM", "rm -f" },
        { "CPP", "$(CC) -E" }, { "LD", "ld" }, { "SHELL", "/bin/sh" },
        { "COMPILE.c", "$(CC) $(CFLAGS) $(CPPFLAGS) $(TARGET_ARCH) -c" },
        { "LINK.c", "$(CC) $(CFLAGS) $(CPPFLAGS) $(LDFLAGS) $(TARGET_ARCH)" },
        { "LINK.o", "$(CC) $(LDFLAGS) $(TARGET_ARCH)" }, { "OUTPUT_OPTION", "-o $@" },
        { "COMPILE.s", "$(AS) $(ASFLAGS) $(TARGET_MACH)" },
    };
    for (size_t i = 0; i < sizeof(defaults) / sizeof(defaults[0]); i++)
        if (!find_variable(defaults[i][0]))
            set_variable(defaults[i][0], defaults[i][1], false, O_DEFAULT);
    static const char *const sfx[] = { ".out", ".a", ".o", ".c", ".s", ".S", ".h", ".sh" };
    for (size_t i = 0; i < sizeof(sfx) / sizeof(sfx[0]); i++)
        push_word(&suffixes, copy(sfx[i]));
    static const char *const rules[][3] = {
        { "%.o", "%.c", "$(COMPILE.c) $(OUTPUT_OPTION) $<" },
        { "%", "%.o", "$(LINK.o) $^ $(LOADLIBES) $(LDLIBS) -o $@" },
        { "%", "%.c", "$(LINK.c) $^ $(LOADLIBES) $(LDLIBS) -o $@" },
        { "%.o", "%.s", "$(COMPILE.s) -o $@ $<" },
    };
    for (size_t i = 0; i < sizeof(rules) / sizeof(rules[0]); i++) {
        struct words t = { 0 }, p = { 0 }, o = { 0 };
        push_word(&t, copy(rules[i][0]));
        push_word(&p, copy(rules[i][1]));
        add_pattern_rule(&t, &p, &o, rules[i][2]);
        rule_pattern->builtin = true;
    }
    rule_pattern = 0;
}

static void command_variable(const char *arg)
{
    const char *eq = strchr(arg, '=');
    char *name = copy_n(arg, (size_t)(eq - arg));
    const char *op = "=";
    size_t n = strlen(name);
    if (n && name[n - 1] == ':') {
        name[n - 1] = 0;
        op = ":=";
    } else if (n && (name[n - 1] == '+' || name[n - 1] == '?')) {
        name[n - 1] = 0;
        op = name[n - 1] == '+' ? "+=" : "?=";
    }
    assign(name, (char *)op, eq + 1, O_COMMAND, false);
    free(name);
}

int main(int argc, char **argv)
{
    const char *env_level = getenv("MAKELEVEL");
    level = env_level ? atoi(env_level) : 0;
    make_path = copy(argv[0][0] && strchr(argv[0], '/') ? argv[0] : "make");
    struct words files = { 0 }, goals = { 0 }, cmdvars = { 0 };
    const char *directory = 0;
    /* MAKEFLAGS from a parent make: its letters, then -- and its variables. */
    const char *mf = getenv("MAKEFLAGS");
    if (mf && level > 0) {
        struct words ws = split_words(mf);
        bool vars = false;
        for (int i = 0; i < ws.n; i++) {
            const char *w = ws.w[i];
            if (!strcmp(w, "--")) {
                vars = true;
                continue;
            }
            if (vars || strchr(w, '=')) {
                push_word(&cmdvars, copy(w));
                continue;
            }
            for (const char *c = w[0] == '-' ? w + 1 : w; *c; c++) {
                if (*c == 'n') dry_run = true;
                else if (*c == 's') silent = true;
                else if (*c == 'k') keep_going = true;
                else if (*c == 'i') ignore_errors = true;
                else if (*c == 'e') environment_overrides = true;
            }
        }
        free_words(&ws);
    }
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--no-print-directory")) {
            print_directory = false;
            continue;
        }
        if (!strcmp(a, "--print-directory") || !strcmp(a, "-w")) {
            print_directory = true;
            continue;
        }
        if (a[0] == '-' && a[1] == '-') {
            message("unknown option '%s'", a);
            return 2;
        }
        if (a[0] == '-' && a[1]) {
            for (const char *c = a + 1; *c; c++) {
                switch (*c) {
                case 'n': dry_run = true; break;
                case 's': silent = true; break;
                case 'k': keep_going = true; break;
                case 'S': keep_going = false; break;
                case 'i': ignore_errors = true; break;
                case 'q': question = true; break;
                case 'e': environment_overrides = true; break;
                case 'B': always_make = true; break;
                case 't': touch_only = true; break;
                case 'r': break;
                case 'j':
                    while (isdigit((unsigned char)c[1]))
                        c++;
                    if (!c[1] && i + 1 < argc && isdigit((unsigned char)argv[i + 1][0]))
                        i++;
                    break;
                case 'f':
                case 'C': {
                    const char *v = c[1] ? c + 1 : i + 1 < argc ? argv[++i] : 0;
                    if (!v) {
                        message("option requires an argument -- '%c'", *c);
                        return 2;
                    }
                    if (*c == 'f')
                        push_word(&files, copy(v));
                    else
                        directory = v;
                    c += strlen(c) - 1;
                    break;
                }
                default:
                    message("invalid option -- '%c'", *c);
                    dprintf(STDERR_FILENO, "usage: make [-f FILE] [-C DIR] [-nsikq] [NAME=VALUE]... [TARGET]...\n");
                    return 2;
                }
            }
            continue;
        }
        if (strchr(a, '=') && a[0] != '=')
            push_word(&cmdvars, copy(a));
        else
            push_word(&goals, copy(a));
    }
    if (directory) {
        if (chdir(directory) < 0) {
            message("*** %s: %s.  Stop.", directory, strerror(errno));
            return 2;
        }
    }
    char cwd[512];
    if (!getcwd(cwd, sizeof(cwd)))
        snprintf(cwd, sizeof(cwd), ".");
    bool show_dirs = print_directory && (level > 0 || directory);
    if (show_dirs) {
        printf("make[%d]: Entering directory '%s'\n", level, cwd);
        fflush(stdout);
    }
    /* Variables: the environment, the built-in ones, the command line's. */
    for (char **e = environ; e && *e; e++) {
        const char *eq = strchr(*e, '=');
        if (!eq)
            continue;
        char *name = copy_n(*e, (size_t)(eq - *e));
        if (strcmp(name, "MAKEFLAGS") && strcmp(name, "SHELL"))
            set_variable(name, eq + 1, false, O_ENVIRONMENT)->exported = true;
        free(name);
    }
    set_variable("MAKE", make_path, true, O_DEFAULT);
    set_variable("CURDIR", cwd, true, O_FILE);
    set_variable("MAKEFILE_LIST", "", true, O_FILE);
    char lvl[16];
    snprintf(lvl, sizeof(lvl), "%d", level + 1);
    setenv("MAKELEVEL", lvl, 1);
    builtin_rules();
    for (int i = 0; i < cmdvars.n; i++)
        command_variable(cmdvars.w[i]);
    {
        struct text f = { 0 };
        if (dry_run) add_char(&f, 'n');
        if (silent) add_char(&f, 's');
        if (keep_going) add_char(&f, 'k');
        if (ignore_errors) add_char(&f, 'i');
        if (environment_overrides) add_char(&f, 'e');
        if (cmdvars.n) {
            add_str(&f, " --");
            for (int i = 0; i < cmdvars.n; i++) {
                add_char(&f, ' ');
                add_str(&f, cmdvars.w[i]);
            }
        }
        setenv("MAKEFLAGS", done(&f), 1);
        set_variable("MAKEFLAGS", f.s, true, O_FILE);
        free(f.s);
    }
    /* The Makefile(s). */
    if (!files.n) {
        /* By the folder's own names (a disk that ignores case would take
         * makefile for Makefile). */
        static const char *const names[] = { "GNUmakefile", "makefile", "Makefile" };
        DIR *d = opendir(".");
        bool found[3] = { false, false, false };
        for (struct dirent *e; d && (e = readdir(d));)
            for (size_t i = 0; i < 3; i++)
                found[i] |= !strcmp(e->d_name, names[i]);
        if (d)
            closedir(d);
        for (size_t i = 0; i < 3; i++)
            if (found[i]) {
                push_word(&files, copy(names[i]));
                break;
            }
    }
    for (int i = 0; i < files.n; i++) {
        FILE *f = !strcmp(files.w[i], "-") ? stdin : fopen(files.w[i], "r");
        if (!f) {
            message("%s: %s", files.w[i], strerror(errno));
            current_file = 0;
            fatal("No rule to make target '%s'", files.w[i]);
        }
        struct variable *list = find_variable("MAKEFILE_LIST");
        struct text t = { 0 };
        add_str(&t, list ? list->value : "");
        if (t.n)
            add_char(&t, ' ');
        add_str(&t, files.w[i]);
        set_variable("MAKEFILE_LIST", t.s, true, O_FILE);
        free(t.s);
        parse_makefile(f, files.w[i]);
        if (f != stdin)
            fclose(f);
    }
    current_file = 0;
    struct variable *goal_var = find_variable(".DEFAULT_GOAL");
    if (!goals.n) {
        const char *g = goal_var && goal_var->value[0] ? goal_var->value : default_goal;
        if (!g) {
            if (!files.n)
                fatal("No targets specified and no makefile found");
            fatal("No targets");
        }
        push_word(&goals, copy(g));
    }
    for (int i = 0; i < goals.n; i++) {
        long before = commands_run;
        int before_errors = errors_seen;
        struct target *made = make_target(goals.w[i], 0, 0);
        if (question)
            continue;
        if (commands_run == before && !made->failed && errors_seen == before_errors) {
            char *stem = 0;
            struct words p = { 0 };
            bool has_recipe = made->has_recipe || implicit_rule(made, &stem, &p, 0);
            free(stem);
            free_words(&p);
            if (!has_recipe || made->phony)
                note("Nothing to be done for '%s'.", goals.w[i]);
            else
                note("'%s' is up to date.", goals.w[i]);
        }
    }
    fflush(stdout);
    if (show_dirs)
        printf("make[%d]: Leaving directory '%s'\n", level, cwd);
    return errors_seen ? 2 : 0;
}
