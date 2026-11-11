/*
 * sed [-nE] [-i[SUFFIX]] SCRIPT [FILE...], sed [-nE] [-i[SUFFIX]] -e SCRIPT... [-f FILE]...
 * [FILE...] (58b): the stream editor, POSIX's. It reads the files (or its
 * standard input) a line at a time into the pattern space, runs the script
 * on it and prints it (not with -n). Commands, each with no address, one
 * or two (N, $, /RE/, \cREc; A,B a range; ! after them: the other lines):
 *   { }  a group            =  the line number      a\ i\ c\ TEXT  append, insert, change
 *   b t LABEL  jump (t: if s replaced since the last line or t)        : LABEL
 *   d D  delete (D: to the first line break, then the script again)
 *   g G h H x  the hold space: get, get appended, hold, hold appended, exchange
 *   l  show (escapes, $ at the end)     n N  next line (N: appended)
 *   p P  print (P: to the first line break)     q  quit     r FILE  append a file
 *   s/RE/REPLACEMENT/[g][p][N][w FILE]  replace (& the match, \1-\9 groups, \n a
 *      line break)     w FILE  write     y/ABC/XYZ/  translate     #  a comment
 * REs are basic (regex.h), extended with -E; \n in one is a line break, an
 * empty one the last used. A range may end at +N, N lines on (as GNU and BSD). Commands are separated by line breaks or ';'
 * (not after a, i, c, r, w, b, t, :). -i edits each FILE in place (its own
 * line numbers and $), keeping its mode and other names; -iSUFFIX keeps the
 * old content as FILE + SUFFIX first. Also a TEXT on one line (a text, as
 * GNU's). Exit status 0, 1 for a bad script, 2 for a file that cannot be
 * read (it goes on with the others).
 */
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <regex.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "ritteros.h"

PROGRAM_HELP("sed [-nE] [-i[SUFFIX]] [-e SCRIPT | -f SCRIPTFILE]... [SCRIPT] [FILE]...",
             "edit a stream: replace, delete and insert lines by a script");

/* --- growing strings ----------------------------------------------------------- */

struct buf {
    char *s;
    size_t n, room;
};

static void need(struct buf *b, size_t more)
{
    if (b->n + more + 1 <= b->room)
        return;
    size_t room = b->room ? b->room : 128;
    while (b->n + more + 1 > room)
        room *= 2;
    char *s = realloc(b->s, room);
    if (!s) {
        dprintf(STDERR_FILENO, "sed: out of memory\n");
        exit(2);
    }
    b->s = s;
    b->room = room;
}

static void add(struct buf *b, const char *s, size_t n)
{
    need(b, n);
    memcpy(b->s + b->n, s, n);
    b->n += n;
    b->s[b->n] = 0;
}

static void set(struct buf *b, const char *s, size_t n)
{
    b->n = 0;
    add(b, s, n);
}

static void add_char(struct buf *b, char c)
{
    add(b, &c, 1);
}

/* --- the script ------------------------------------------------------------------ */

enum { A_NONE, A_LINE, A_LAST, A_RE, A_PLUS };       /* (A_PLUS: ,+N as a range's end) */

struct address {
    int kind;
    long line;
    regex_t *re;
};

struct output {
    char *name;
    FILE *f;
};

struct cmd {
    struct address a1, a2;
    bool negate;
    char name;
    char *text;                     /* a, i, c: the text; b, t, :: the label; r, w: the file */
    int jump;                       /* b, t: the command index; {: the matching } */
    regex_t *re;                    /* s */
    struct buf replacement;
    bool global, print;
    long occurrence;
    struct output *out;             /* s///w, w */
    unsigned char map[256];         /* y */
    bool in_range;                  /* a range that has begun */
    long range_end;                 /* (a range ending at a line number) */
};

static struct cmd *cmds;
static int cmd_count, cmd_room;
static struct output outputs[16];
static int output_count;
static bool extended, quiet, in_place;
static const char *suffix;
static regex_t *last_re;
static struct buf script;
static int status;

static __attribute__((noreturn)) void bad(const char *what, const char *near)
{
    dprintf(STDERR_FILENO, "sed: %s%s%s\n", what, near ? ": " : "", near ? near : "");
    exit(1);
}

static struct output *output_for(const char *name)
{
    for (int i = 0; i < output_count; i++)
        if (!strcmp(outputs[i].name, name))
            return &outputs[i];
    if (output_count == 16)
        bad("too many files to write", name);
    struct output *o = &outputs[output_count++];
    o->name = strdup(name);
    o->f = !strcmp(name, "/dev/stdout") ? stdout : fopen(name, "w");
    if (!o->f)
        bad(strerror(errno), name);
    return o;
}

/* An RE between delimiters at *p (after the opening one): compiled, *p
 * past the closing one; 0 for an empty RE (the last used). */
static regex_t *parse_re(const char **p, char delim)
{
    struct buf text = { 0 };
    const char *s = *p;
    bool in_set = false;
    for (;; s++) {
        if (!*s)
            bad("an unterminated address or s command", 0);
        if (in_set) {
            if (*s == ']' && !(s[-1] == '[' || (s[-1] == '^' && s[-2] == '[')))
                in_set = false;
            add_char(&text, *s);
            continue;
        }
        if (*s == delim)
            break;
        if (*s == '\\' && s[1]) {
            s++;
            if (*s == delim)
                add_char(&text, delim);             /* (\/ is /) */
            else if (*s == 'n')
                add_char(&text, '\n');
            else {
                add_char(&text, '\\');
                add_char(&text, *s);
            }
            continue;
        }
        if (*s == '[')
            in_set = true;
        add_char(&text, *s);
    }
    *p = s + 1;
    if (!text.n) {
        free(text.s);
        return 0;
    }
    regex_t *re = malloc(sizeof(*re));
    int e = re ? regcomp(re, text.s, extended ? REG_EXTENDED : 0) : REG_ESPACE;
    if (e) {
        char why[100];
        regerror(e, re, why, sizeof(why));
        bad(why, text.s);
    }
    free(text.s);
    return re;
}

static bool parse_address(const char **p, struct address *a)
{
    const char *s = *p;
    if (isdigit((unsigned char)*s)) {
        a->kind = A_LINE;
        a->line = strtol(s, (char **)&s, 10);
    } else if (*s == '$') {
        a->kind = A_LAST;
        s++;
    } else if (*s == '/' || *s == '\\') {
        char delim = '/';
        if (*s == '\\') {
            delim = *++s;
            if (!delim || delim == '\n' || delim == '\\')
                bad("an invalid address delimiter", 0);
        }
        s++;
        a->kind = A_RE;
        a->re = parse_re(&s, delim);
    } else {
        return false;
    }
    *p = s;
    return true;
}

static void skip_blanks(const char **p)
{
    while (**p == ' ' || **p == '\t')
        (*p)++;
}

/* A label or file name: the rest of the line (a ';' ends a label too, as GNU's). */
static char *rest_of_line(const char **p, bool label)
{
    skip_blanks(p);
    const char *s = *p;
    while (*s && *s != '\n' && !(label && *s == ';'))
        s++;
    const char *e = s;
    while (e > *p && (e[-1] == ' ' || e[-1] == '\t'))
        e--;
    char *r = strndup(*p, (size_t)(e - *p));
    *p = s;
    return r;
}

/* a\, i\, c\ TEXT: lines ending in \ go on; \X is X. */
static char *parse_text(const char **p)
{
    const char *s = *p;
    skip_blanks(&s);
    if (*s == '\\') {
        s++;
        skip_blanks(&s);
        if (*s == '\n')
            s++;
    }
    struct buf text = { 0 };
    add(&text, "", 0);
    while (*s && *s != '\n') {
        if (*s == '\\' && s[1]) {
            s++;
            add_char(&text, *s++);                  /* (\ newline: the text goes on) */
            continue;
        }
        add_char(&text, *s++);
    }
    *p = s;
    return text.s;
}

static void parse_replacement(const char **p, char delim, struct buf *out)
{
    const char *s = *p;
    add(out, "", 0);
    for (;; s++) {
        if (!*s)
            bad("an unterminated s command", 0);
        if (*s == delim)
            break;
        if (*s == '\\' && s[1]) {
            s++;
            if (*s == delim)
                add_char(out, delim);
            else if (*s == 'n')
                add_char(out, '\n');
            else if (*s == '\n')
                add_char(out, '\n');
            else {
                add_char(out, '\\');                /* (\1-\9, \&, \\: kept for later) */
                add_char(out, *s);
            }
            continue;
        }
        add_char(out, *s);
    }
    *p = s + 1;
}

static struct cmd *new_cmd(void)
{
    if (cmd_count == cmd_room) {
        cmd_room = cmd_room ? cmd_room * 2 : 32;
        cmds = realloc(cmds, sizeof(*cmds) * (size_t)cmd_room);
        if (!cmds)
            bad("out of memory", 0);
    }
    struct cmd *c = &cmds[cmd_count++];
    memset(c, 0, sizeof(*c));
    return c;
}

static void parse(const char *s)
{
    int open[64], depth = 0;
    for (;;) {
        while (*s == ' ' || *s == '\t' || *s == '\n' || *s == ';')
            s++;
        if (!*s)
            break;
        if (*s == '#') {
            while (*s && *s != '\n')
                s++;
            continue;
        }
        struct cmd *c = new_cmd();
        if (parse_address(&s, &c->a1)) {
            skip_blanks(&s);
            if (*s == ',') {
                s++;
                skip_blanks(&s);
                if (*s == '+' && isdigit((unsigned char)s[1])) {
                    c->a2.kind = A_PLUS;
                    c->a2.line = strtol(s + 1, (char **)&s, 10);
                } else if (!parse_address(&s, &c->a2)) {
                    bad("an address is missing after ','", 0);
                }
            }
        }
        skip_blanks(&s);
        while (*s == '!') {
            c->negate = true;
            s++;
            skip_blanks(&s);
        }
        c->name = *s;
        if (!*s)
            bad("a command is missing", 0);
        s++;
        switch (c->name) {
        case '{':
            if (depth == 64)
                bad("groups nested too deep", 0);
            open[depth++] = cmd_count - 1;
            break;
        case '}':
            if (!depth)
                bad("an unexpected '}'", 0);
            if (c->a1.kind)
                bad("'}' takes no address", 0);
            cmds[open[--depth]].jump = cmd_count - 1;
            break;
        case 'a':
        case 'i':
        case 'c':
            c->text = parse_text(&s);
            break;
        case ':':
            if (c->a1.kind)
                bad("':' takes no address", 0);
            c->text = rest_of_line(&s, true);
            if (!*c->text)
                bad("a label is missing", 0);
            break;
        case 'b':
        case 't':
            c->text = rest_of_line(&s, true);
            break;
        case 'r':
            c->text = rest_of_line(&s, false);
            break;
        case 'w':
            c->text = rest_of_line(&s, false);
            c->out = output_for(c->text);
            break;
        case 's': {
            char delim = *s++;
            if (!delim || delim == '\n' || delim == '\\')
                bad("an invalid delimiter in s", 0);
            c->re = parse_re(&s, delim);
            parse_replacement(&s, delim, &c->replacement);
            for (;;) {
                if (*s == 'g')
                    c->global = true, s++;
                else if (*s == 'p')
                    c->print = true, s++;
                else if (isdigit((unsigned char)*s))
                    c->occurrence = strtol(s, (char **)&s, 10);
                else if (*s == 'w') {
                    s++;
                    char *name = rest_of_line(&s, false);
                    c->out = output_for(name);
                    free(name);
                    break;
                } else
                    break;
            }
            break;
        }
        case 'y': {
            char delim = *s++;
            struct buf from = { 0 }, to = { 0 };
            for (struct buf *b = &from; b; b = b == &from ? &to : 0) {
                add(b, "", 0);
                for (;; s++) {
                    if (!*s)
                        bad("an unterminated y command", 0);
                    if (*s == delim)
                        break;
                    if (*s == '\\' && s[1]) {
                        s++;
                        add_char(b, *s == 'n' ? '\n' : *s);
                        continue;
                    }
                    add_char(b, *s);
                }
                s++;
            }
            if (from.n != to.n)
                bad("y: the two sets differ in length", 0);
            for (int k = 0; k < 256; k++)
                c->map[k] = (unsigned char)k;
            for (size_t k = 0; k < from.n; k++)
                c->map[(unsigned char)from.s[k]] = (unsigned char)to.s[k];
            free(from.s);
            free(to.s);
            break;
        }
        case '=': case 'd': case 'D': case 'g': case 'G': case 'h': case 'H': case 'l':
        case 'n': case 'N': case 'p': case 'P': case 'q': case 'x':
            break;
        default: {
            char name[2] = { c->name, 0 };
            bad("an unknown command", name);
        }
        }
        skip_blanks(&s);
        if (c->name == '{' || (*s == '}' && c->name != '}'))
            continue;                               /* ({p and p} need no ';') */
        if (*s && *s != '\n' && *s != ';' && *s != '#')
            bad("extra characters after a command", s);
    }
    if (depth)
        bad("an unmatched '{'", 0);
    for (int i = 0; i < cmd_count; i++) {           /* the labels */
        struct cmd *c = &cmds[i];
        if (c->name != 'b' && c->name != 't')
            continue;
        c->jump = cmd_count;                        /* (no label: the end) */
        if (!*c->text)
            continue;
        int k = 0;
        while (k < cmd_count && !(cmds[k].name == ':' && !strcmp(cmds[k].text, c->text)))
            k++;
        if (k == cmd_count)
            bad("no such label", c->text);
        c->jump = k;
    }
}

/* --- input ------------------------------------------------------------------------ */

static char **files;
static int file_count, file_at;
static FILE *in;
static long line_number;
static struct buf next_line;
static bool have_next, next_newline;

static bool open_next(void)
{
    while (!in && file_at < file_count) {
        const char *name = files[file_at++];
        in = !strcmp(name, "-") ? stdin : fopen(name, "r");
        if (!in) {
            dprintf(STDERR_FILENO, "sed: %s: %s\n", name, strerror(errno));
            status = 2;
        }
    }
    return in != 0;
}

/* Read the next line into next_line (with whether it ended in a line break). */
static bool read_ahead(void)
{
    next_line.n = 0;
    add(&next_line, "", 0);
    while (open_next()) {
        int ch;
        bool any = false;
        while ((ch = fgetc(in)) != EOF) {
            any = true;
            if (ch == '\n') {
                next_newline = true;
                return true;
            }
            add_char(&next_line, (char)ch);
        }
        if (in != stdin)
            fclose(in);
        in = 0;
        if (any) {
            next_newline = false;
            return true;
        }
    }
    return false;
}

static struct buf pattern_space, hold_space, appended;
static bool newline_after;              /* the current line ended in a line break */

/* The next line into *b (appended after a line break with add_to): false at the end. */
static bool next_input(struct buf *b, bool add_to)
{
    if (!have_next)
        return false;
    if (add_to)
        add_char(b, '\n');
    else
        b->n = 0;
    add(b, next_line.s, next_line.n);
    newline_after = next_newline;
    line_number++;
    have_next = read_ahead();
    return true;
}

static bool last_line(void)
{
    return !have_next;
}

/* --- running ---------------------------------------------------------------------- */

static FILE *out;

static void emit(const char *s, size_t n, bool newline, FILE *f)
{
    fwrite(s, 1, n, f);
    if (newline)
        fputc('\n', f);
}

static bool matches(regex_t *re, const struct buf *b)
{
    if (re)
        last_re = re;
    if (!last_re)
        bad("no previous regular expression", 0);
    return regexec(last_re, b->s, 0, 0, 0) == 0;
}

static bool address_matches(struct address *a)
{
    switch (a->kind) {
    case A_LINE:
        return line_number == a->line;
    case A_LAST:
        return last_line();
    case A_RE:
        return matches(a->re, &pattern_space);
    }
    return true;
}

static bool selected(struct cmd *c)
{
    bool on;
    if (!c->a1.kind)
        on = true;
    else if (!c->a2.kind)
        on = address_matches(&c->a1);
    else if (c->in_range) {
        on = true;
        if (c->a2.kind == A_LINE || c->a2.kind == A_PLUS ? line_number >= c->range_end
                                                          : address_matches(&c->a2))
            c->in_range = false;
    } else if (address_matches(&c->a1)) {
        on = true;
        /* A range whose end is a line number already reached is one line. */
        c->range_end = c->a2.kind == A_PLUS ? line_number + c->a2.line : c->a2.line;
        c->in_range = c->a2.kind == A_LINE || c->a2.kind == A_PLUS ? line_number < c->range_end
                    : c->a2.kind == A_LAST ? !last_line() : true;
    } else {
        on = false;
    }
    return on != c->negate;
}

static void show(const struct buf *b)
{
    int col = 0;
    for (size_t i = 0; i < b->n; i++) {
        char e[8];
        unsigned char ch = (unsigned char)b->s[i];
        const char *esc = ch == '\\' ? "\\\\" : ch == '\a' ? "\\a" : ch == '\b' ? "\\b"
                        : ch == '\f' ? "\\f" : ch == '\n' ? "\\n" : ch == '\r' ? "\\r"
                        : ch == '\t' ? "\\t" : ch == '\v' ? "\\v" : 0;
        if (!esc && !isprint(ch)) {
            snprintf(e, sizeof(e), "\\%03o", ch);
            esc = e;
        }
        char plain[2] = { (char)ch, 0 };
        const char *piece = esc ? esc : plain;
        int len = (int)strlen(piece);
        if (col + len > 69) {                       /* (folded as POSIX's 70 columns) */
            fputs("\\\n", out);
            col = 0;
        }
        fputs(piece, out);
        col += len;
    }
    fputs("$\n", out);
}

static bool substitute(struct cmd *c)
{
    regex_t *re = c->re ? c->re : last_re;
    if (c->re)
        last_re = c->re;
    if (!re)
        bad("no previous regular expression", 0);
    regmatch_t m[10];
    struct buf result = { 0 };
    add(&result, "", 0);
    size_t at = 0, last_end = (size_t)-1;
    long count = 0;
    bool replaced = false;
    const char *s = pattern_space.s;
    while (at <= pattern_space.n) {
        if (regexec(re, s + at, 10, m, at ? REG_NOTBOL : 0) != 0)
            break;
        size_t so = at + (size_t)m[0].rm_so, eo = at + (size_t)m[0].rm_eo;
        if (eo == so && so == last_end) {           /* (empty, right after a match: none) */
            if (so >= pattern_space.n)
                break;
            add_char(&result, s[so]);
            at = so + 1;
            continue;
        }
        count++;
        last_end = eo;
        add(&result, s + at, so - at);
        bool take = c->occurrence ? count >= c->occurrence && (c->global || count == c->occurrence)
                                  : true;
        if (take) {
            replaced = true;
            const struct buf *r = &c->replacement;
            for (size_t k = 0; k < r->n; k++) {
                char ch = r->s[k];
                if (ch == '&') {
                    add(&result, s + so, eo - so);
                } else if (ch == '\\' && k + 1 < r->n) {
                    char d = r->s[++k];
                    if (d >= '0' && d <= '9') {
                        int g = d - '0';
                        if (g <= (int)re->re_nsub && m[g].rm_so >= 0)
                            add(&result, s + at + m[g].rm_so, (size_t)(m[g].rm_eo - m[g].rm_so));
                    } else {
                        add_char(&result, d);
                    }
                } else {
                    add_char(&result, ch);
                }
            }
        } else {
            add(&result, s + so, eo - so);
        }
        if (eo == so) {                             /* (an empty match: one character on) */
            if (so < pattern_space.n)
                add_char(&result, s[so]);
            at = so + 1;
        } else {
            at = eo;
        }
        if (take && !c->global)
            break;
    }
    if (!replaced) {
        free(result.s);
        return false;
    }
    if (at < pattern_space.n)
        add(&result, s + at, pattern_space.n - at);
    free(pattern_space.s);
    pattern_space = result;
    return true;
}

static void flush_appended(void)
{
    if (appended.n)
        fwrite(appended.s, 1, appended.n, out);
    appended.n = 0;
}

static void append_file(const char *name)
{
    FILE *f = fopen(name, "r");
    if (!f)
        return;                                     /* (POSIX: a missing file is nothing) */
    char b[4096];
    size_t n;
    while ((n = fread(b, 1, sizeof(b), f)) > 0)
        add(&appended, b, n);
    fclose(f);
}

/* One run over the input: 0 normally, 1 if q ended it. */
static void run(void)
{
    have_next = read_ahead();
    bool quit = false;
    while (!quit && next_input(&pattern_space, false)) {
        bool replaced = false, deleted = false, restart;
        do {
            restart = false;
            for (int i = 0; i < cmd_count && !deleted && !restart && !quit; i++) {
                struct cmd *c = &cmds[i];
                if (c->name == ':' || c->name == '}')
                    continue;
                if (!selected(c)) {
                    if (c->name == '{')
                        i = c->jump;
                    continue;
                }
                switch (c->name) {
                case '{':
                    break;
                case '=':
                    fprintf(out, "%ld\n", line_number);
                    break;
                case 'a':
                    add(&appended, c->text, strlen(c->text));
                    add_char(&appended, '\n');
                    break;
                case 'i':
                    fprintf(out, "%s\n", c->text);
                    break;
                case 'c':
                    /* In a range: the text once, at its end. */
                    if (!c->a2.kind || !c->in_range || c->negate)
                        fprintf(out, "%s\n", c->text);
                    deleted = true;
                    break;
                case 'b':
                    i = c->jump - 1;
                    break;
                case 't':
                    if (replaced) {
                        replaced = false;
                        i = c->jump - 1;
                    }
                    break;
                case 'd':
                    deleted = true;
                    break;
                case 'D': {
                    char *nl = memchr(pattern_space.s, '\n', pattern_space.n);
                    if (!nl) {
                        deleted = true;
                        break;
                    }
                    size_t cut = (size_t)(nl - pattern_space.s) + 1;
                    memmove(pattern_space.s, pattern_space.s + cut, pattern_space.n - cut + 1);
                    pattern_space.n -= cut;
                    flush_appended();
                    restart = true;                 /* (the script again, no new line read) */
                    break;
                }
                case 'g':
                    set(&pattern_space, hold_space.s ? hold_space.s : "", hold_space.n);
                    break;
                case 'G':
                    add_char(&pattern_space, '\n');
                    add(&pattern_space, hold_space.s ? hold_space.s : "", hold_space.n);
                    break;
                case 'h':
                    set(&hold_space, pattern_space.s, pattern_space.n);
                    break;
                case 'H':
                    add_char(&hold_space, '\n');
                    add(&hold_space, pattern_space.s, pattern_space.n);
                    break;
                case 'x': {
                    struct buf t = pattern_space;
                    pattern_space = hold_space;
                    hold_space = t;
                    if (!pattern_space.s)
                        add(&pattern_space, "", 0);
                    break;
                }
                case 'l':
                    show(&pattern_space);
                    break;
                case 'n':
                    if (!quiet)
                        emit(pattern_space.s, pattern_space.n, newline_after, out);
                    flush_appended();
                    if (!next_input(&pattern_space, false)) {
                        deleted = true;             /* (no next line: the end, printed already) */
                        quit = true;
                    }
                    break;
                case 'N':
                    if (!next_input(&pattern_space, true)) {
                        quit = true;                /* (printed at the end, as GNU and BSD) */
                    }
                    flush_appended();
                    break;
                case 'p':
                    emit(pattern_space.s, pattern_space.n, true, out);
                    break;
                case 'P': {
                    char *nl = memchr(pattern_space.s, '\n', pattern_space.n);
                    emit(pattern_space.s, nl ? (size_t)(nl - pattern_space.s) : pattern_space.n,
                         true, out);
                    break;
                }
                case 'q':
                    quit = true;
                    break;
                case 'r':
                    append_file(c->text);
                    break;
                case 's':
                    if (substitute(c)) {
                        replaced = true;
                        if (c->print)
                            emit(pattern_space.s, pattern_space.n, true, out);
                        if (c->out)
                            emit(pattern_space.s, pattern_space.n, true, c->out->f);
                    }
                    break;
                case 'w':
                    emit(pattern_space.s, pattern_space.n, true, c->out->f);
                    break;
                case 'y':
                    for (size_t k = 0; k < pattern_space.n; k++)
                        pattern_space.s[k] = (char)c->map[(unsigned char)pattern_space.s[k]];
                    break;
                }
            }
        } while (restart && !quit && !deleted);
        if (!deleted && !quiet)
            emit(pattern_space.s, pattern_space.n, newline_after || have_next, out);
        flush_appended();
    }
}

/* -i: one file, its content edited and written back. */
static void edit_in_place(char *name)
{
    struct stat st;
    if (stat(name, &st) < 0 || S_ISDIR(st.st_mode)) {
        dprintf(STDERR_FILENO, "sed: %s: %s\n", name, S_ISDIR(st.st_mode) ? "a folder"
                                                                          : strerror(errno));
        status = 2;
        return;
    }
    out = tmpfile();                                /* (the new content, read back after) */
    if (!out) {
        dprintf(STDERR_FILENO, "sed: a temporary file: %s\n", strerror(errno));
        exit(2);
    }
    files = &name;
    file_count = 1;
    file_at = 0;
    line_number = 0;
    for (int i = 0; i < cmd_count; i++)
        cmds[i].in_range = false;
    run();
    fflush(out);
    long length = ftell(out);
    char *result = length >= 0 ? malloc((size_t)length + 1) : 0;
    size_t size = 0;
    if (result) {
        rewind(out);
        size = fread(result, 1, (size_t)length, out);
    }
    fclose(out);
    if (!result || size != (size_t)length) {
        dprintf(STDERR_FILENO, "sed: %s: the new content could not be read back\n", name);
        status = 2;
        free(result);
        return;
    }
    if (suffix && *suffix) {
        char backup[512];
        snprintf(backup, sizeof(backup), "%s%s", name, suffix);
        if (ritteros_copy_bytes(name, backup, 0, 0) < 0 && errno != EEXIST) {
            dprintf(STDERR_FILENO, "sed: %s: %s\n", backup, strerror(errno));
            status = 2;
            free(result);
            return;
        }
    }
    int fd = open(name, O_WRONLY | O_TRUNC);
    if (fd < 0 || write(fd, result, size) != (ssize_t)size) {
        dprintf(STDERR_FILENO, "sed: %s: %s\n", name, strerror(errno));
        status = 2;
    }
    if (fd >= 0)
        close(fd);
    free(result);
}

int main(int argc, char **argv)
{
    int i = 1;
    bool have_script = false;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "--")) {
            i++;
            break;
        }
        const char *o = argv[i] + 1;
        if (*o == 'i') {                            /* -i[SUFFIX] */
            in_place = true;
            suffix = o + 1;
            continue;
        }
        for (; *o; o++) {
            if (*o == 'n')
                quiet = true;
            else if (*o == 'E' || *o == 'r')
                extended = true;
            else if (*o == 'e' || *o == 'f') {
                const char *arg = o[1] ? o + 1 : i + 1 < argc ? argv[++i] : 0;
                if (!arg)
                    bad("an option needs an argument", *o == 'e' ? "-e" : "-f");
                if (*o == 'e') {
                    add(&script, arg, strlen(arg));
                } else {
                    FILE *f = fopen(arg, "r");
                    if (!f)
                        bad(strerror(errno), arg);
                    int ch;
                    while ((ch = fgetc(f)) != EOF)
                        add_char(&script, (char)ch);
                    fclose(f);
                }
                add_char(&script, '\n');
                have_script = true;
                break;
            } else {
                dprintf(STDERR_FILENO, "usage: sed [-nE] [-i[SUFFIX]] SCRIPT [FILE...]\n");
                return 1;
            }
        }
    }
    if (!have_script) {
        if (i >= argc) {
            dprintf(STDERR_FILENO, "usage: sed [-nE] [-i[SUFFIX]] SCRIPT [FILE...]\n");
            return 1;
        }
        add(&script, argv[i], strlen(argv[i]));
        add_char(&script, '\n');
        i++;
    }
    parse(script.s);
    add(&pattern_space, "", 0);
    add(&next_line, "", 0);
    if (in_place) {
        if (i >= argc)
            bad("-i needs a file", 0);
        for (; i < argc; i++)
            edit_in_place(argv[i]);
    } else {
        static char *standard_input[] = { "-" };
        out = stdout;
        setvbuf(stdout, 0, _IOFBF, BUFSIZ);
        files = i < argc ? argv + i : standard_input;
        file_count = i < argc ? argc - i : 1;
        run();
        fflush(stdout);
    }
    for (int k = 0; k < output_count; k++)
        if (outputs[k].f != stdout)
            fclose(outputs[k].f);
    return status;
}
