/*
 * man [-k WORD] [NAME] (50j): the manual on the machine. The pages are the
 * tree's docs/ as /system/doc: a page per program (commands/NAME.md), the
 * install guide (install.md) and the user's manual (manual.md), written
 * in Markdown and shown as plain terminal text: headings in bold, code
 * indented, tables in columns, links as their text. On a terminal a page
 * longer than the screen is shown a screen at a time (space: the next
 * page, Enter: the next line, b: back, q: quit). `man -k WORD` lists the
 * pages whose title has WORD; `man` alone lists them all.
 */
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include "ritteros.h"

PROGRAM_HELP("man [-k WORD] [NAME]", "the manual: a command's page, a topic (install, manual), or a search");

#define DOC_DIR     "/system/doc"
#define MAX_LINES   20000
#define LINE_MAX_   1024

static char *lines[MAX_LINES];
static int count;
static bool terminal;                   /* standard output is a terminal: bold, paging */
static bool in_span;                    /* inside a `code` span that goes on to the next line */

static void add(const char *text)
{
    if (count < MAX_LINES)
        lines[count++] = strdup(text);
}

/* Inline Markdown into plain text: `code` and [text](link) lose their marks,
 * **strong** becomes bold on a terminal. */
static void inline_text(const char *in, char *out, size_t size)
{
    size_t o = 0;
    bool bold = false, code = in_span;
    for (size_t i = 0; in[i] && o + 8 < size; i++) {
        if (in[i] == '`') {
            /* `code`: as it is, without marks; a span opened by a run of n
             * backticks ends at the next run of n (``a ` in it``). */
            size_t run = 1;
            while (in[i + run] == '`')
                run++;
            const char *end = 0;
            if (!code && run > 1)
                for (const char *q = in + i + run; (q = strchr(q, '`')); ) {
                    size_t r = 1;
                    while (q[r] == '`')
                        r++;
                    if (r == run) {
                        end = q;
                        break;
                    }
                    q += r;
                }
            if (end) {
                const char *from = in + i + run, *to = end;
                if (from < to && *from == ' ')
                    from++;
                if (to > from && to[-1] == ' ')
                    to--;
                while (from < to && o + 8 < size)
                    out[o++] = *from++;
                i = (size_t)(end - in) + run - 1;
                continue;
            }
            code = !code;
            i += run - 1;
            continue;
        }
        if (!code && in[i] == '*' && in[i + 1] == '*') {
            bold = !bold;
            if (terminal)
                o += (size_t)snprintf(out + o, size - o, "%s", bold ? "\033[1m" : "\033[0m");
            i++;
            continue;
        }
        if (!code && in[i] == '[') {                /* [text](link): the text */
            const char *close = strchr(in + i, ']');
            if (close && close[1] == '(' && strchr(close, ')'))
                continue;
        }
        if (!code && in[i] == ']' && in[i + 1] == '(') {
            const char *end = strchr(in + i, ')');
            if (end) {
                i = (size_t)(end - in);
                continue;
            }
        }
        if (!code && in[i] == '\\' && in[i + 1])  /* \| and the like */
            i++;
        out[o++] = in[i];
    }
    if (bold && terminal)
        o += (size_t)snprintf(out + o, size - o, "\033[0m");
    out[o] = 0;
    in_span = code;                                 /* a paragraph's span may cross lines */
}

/* A table's rows (| a | b |), separator rows left out, in columns. */
static void table(char rows[][LINE_MAX_], int n)
{
    enum { COLS = 8 };
    static char cell[64][COLS][LINE_MAX_];
    int width[COLS] = { 0 }, ncols = 0, kept = 0;
    for (int r = 0; r < n && kept < 64; r++) {
        if (strspn(rows[r], "|-: ") == strlen(rows[r]))
            continue;                               /* |---|---| */
        char *p = rows[r] + 1;
        int c = 0;
        while (*p && c < COLS) {
            char *bar = p;
            while (*bar && !(*bar == '|' && bar[-1] != '\\'))
                bar++;
            char raw[LINE_MAX_];
            snprintf(raw, sizeof(raw), "%.*s", (int)(bar - p), p);
            char *s = raw;
            while (*s == ' ')
                s++;
            size_t len = strlen(s);
            while (len && s[len - 1] == ' ')
                s[--len] = 0;
            inline_text(s, cell[kept][c], LINE_MAX_);
            int w = (int)strlen(cell[kept][c]);
            if (w > width[c])
                width[c] = w;
            c++;
            if (!*bar)
                break;
            p = bar + 1;
            if (!*p)
                break;
        }
        for (int k = c; k < COLS; k++)
            cell[kept][k][0] = 0;
        if (c > ncols)
            ncols = c;
        kept++;
    }
    if (width[0] > 28)
        width[0] = 28;                              /* long first columns go on */
    for (int r = 0; r < kept; r++) {
        char line[LINE_MAX_ * 2];
        size_t o = (size_t)snprintf(line, sizeof(line), "  ");
        for (int c = 0; c < ncols && o < sizeof(line) - 1; c++) {
            if (c == ncols - 1)
                o += (size_t)snprintf(line + o, sizeof(line) - o, "%s", cell[r][c]);
            else
                o += (size_t)snprintf(line + o, sizeof(line) - o, "%-*s  ", width[c], cell[r][c]);
        }
        add(line);
        if (r == 0 && kept > 1 && terminal)
            add("");                                /* after the header row */
    }
}

/* The page at path, rendered into lines; false if it cannot be read. */
static bool render(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return false;
    static char rows[256][LINE_MAX_];
    int table_rows = 0;
    bool code = false;
    char raw[LINE_MAX_], text[LINE_MAX_ * 2];
    for (;;) {
        bool got = fgets(raw, sizeof(raw), f) != 0;
        if (got)
            raw[strcspn(raw, "\r\n")] = 0;
        if (table_rows && (!got || raw[0] != '|' || code)) {
            table(rows, table_rows);
            table_rows = 0;
        }
        if (!got)
            break;
        if (!strncmp(raw, "```", 3)) {
            code = !code;
            in_span = false;
            continue;
        }
        if (!code && (raw[0] == 0 || raw[0] == '#' || raw[0] == '|'))
            in_span = false;                        /* a paragraph ends */
        if (code) {
            snprintf(text, sizeof(text), "    %s", raw);
            add(text);
        } else if (raw[0] == '|') {
            if (table_rows < 256)
                snprintf(rows[table_rows++], LINE_MAX_, "%s", raw);
        } else if (raw[0] == '#') {
            const char *h = raw + strspn(raw, "#");
            while (*h == ' ')
                h++;
            char plain[LINE_MAX_];
            inline_text(h, plain, sizeof(plain));
            snprintf(text, sizeof(text), "%s%s%s", terminal ? "\033[1m" : "", plain,
                     terminal ? "\033[0m" : "");
            add(text);
        } else if (!strcmp(raw, "---")) {
            add("");
        } else {
            inline_text(raw, text, sizeof(text));
            add(text);
        }
    }
    fclose(f);
    return true;
}

/* --- the pager --- */

static struct termios saved;

static int key(void)
{
    unsigned char c;
    return read(STDIN_FILENO, &c, 1) == 1 ? c : 'q';
}

static void show(void)
{
    int rows = 0, cols = 0;
    if (!terminal || !isatty(STDIN_FILENO) || ritteros_terminal_size(&rows, &cols) < 0 ||
        rows < 3 || count <= rows - 1) {
        for (int i = 0; i < count; i++)
            printf("%s\n", lines[i]);
        return;
    }
    struct termios raw = { 0 };
    bool restored = tcgetattr(STDIN_FILENO, &saved) == 0;
    if (restored) {
        raw = saved;
        cfmakeraw(&raw);
        tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    }
    int page = rows - 1, at = 0, shown = 0;
    for (;;) {
        while (shown < page && at < count) {
            printf("%s\n", lines[at++]);
            shown++;
        }
        if (at >= count)
            break;
        printf("\033[7m-- line %d of %d (space: next page, Enter: next line, b: back, q: quit) --"
               "\033[0m", at, count);
        fflush(stdout);
        int k = key();
        printf("\r\033[K");
        if (k == 'q' || k == 'Q' || k == 3 || k == 4)
            break;
        if (k == '\r' || k == '\n')
            shown = page - 1;
        else if (k == 'b' || k == 'B') {
            at = at - 2 * page > 0 ? at - 2 * page : 0;
            shown = 0;
        } else
            shown = 0;
    }
    if (restored)
        tcsetattr(STDIN_FILENO, TCSANOW, &saved);
}

/* --- the pages --- */

/* "# NAME: what it does" of a page, as "NAME: what it does" (or 0). */
static bool title_of(const char *path, char *out, size_t size)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return false;
    char raw[LINE_MAX_];
    bool ok = fgets(raw, sizeof(raw), f) && raw[0] == '#';
    fclose(f);
    if (!ok)
        return false;
    raw[strcspn(raw, "\r\n")] = 0;
    const char *h = raw + strspn(raw, "# ");
    inline_text(h, out, size);
    return true;
}

static int by_name(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/* Every command page's title, sorted; with word, only those that have it
 * (case does not matter). The count listed. */
static int list(const char *word)
{
    DIR *d = opendir(DOC_DIR "/commands");
    if (!d) {
        fprintf(stderr, "man: %s/commands: %s\n", DOC_DIR, strerror(errno));
        return -1;
    }
    static char *names[1024];
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < 1024) {
        size_t len = strlen(e->d_name);
        if (len > 3 && !strcmp(e->d_name + len - 3, ".md") && strcmp(e->d_name, "README.md"))
            names[n++] = strdup(e->d_name);
    }
    closedir(d);
    qsort(names, (size_t)n, sizeof(names[0]), by_name);
    int found = 0;
    for (int i = 0; i < n; i++) {
        char path[512], title[LINE_MAX_];
        snprintf(path, sizeof(path), "%s/commands/%s", DOC_DIR, names[i]);
        if (!title_of(path, title, sizeof(title)))
            continue;
        if (word) {
            char low_title[LINE_MAX_], low_word[256];
            size_t k = 0;
            for (; title[k] && k < sizeof(low_title) - 1; k++)
                low_title[k] = (char)tolower((unsigned char)title[k]);
            low_title[k] = 0;
            for (k = 0; word[k] && k < sizeof(low_word) - 1; k++)
                low_word[k] = (char)tolower((unsigned char)word[k]);
            low_word[k] = 0;
            if (!strstr(low_title, low_word))
                continue;
        }
        char *colon = strchr(title, ':');
        char line[LINE_MAX_ + 32];
        if (colon) {
            *colon = 0;
            snprintf(line, sizeof(line), "%-12s %s", title, colon + 1 + (colon[1] == ' '));
        } else {
            snprintf(line, sizeof(line), "%s", title);
        }
        add(line);
        found++;
    }
    return found;
}

int main(int argc, char **argv)
{
    terminal = isatty(STDOUT_FILENO);
    if (argc == 3 && !strcmp(argv[1], "-k")) {
        int n = list(argv[2]);
        if (n < 0)
            return 1;
        if (n == 0) {
            printf("man: no page has '%s' in its title\n", argv[2]);
            return 1;
        }
        show();
        return 0;
    }
    if (argc == 1) {
        add("The pages (man NAME shows one; man -k WORD searches):");
        add("");
        if (list(0) < 0)
            return 1;
        add("");
        add("Topics: install (the install guide), manual (the user's manual).");
        show();
        return 0;
    }
    if (argc != 2 || argv[1][0] == '-' || strchr(argv[1], '/')) {
        fprintf(stderr, "usage: man [-k WORD] [NAME]\n");
        return 2;
    }
    const char *name = strcmp(argv[1], "[") ? argv[1] : "test";     /* [ is test */
    char path[512];
    snprintf(path, sizeof(path), "%s/commands/%s.md", DOC_DIR, name);
    if (!render(path)) {
        snprintf(path, sizeof(path), "%s/%s.md", DOC_DIR, name);    /* a topic */
        if (!render(path)) {
            fprintf(stderr, "man: no page for %s (man -k WORD searches the titles)\n", name);
            return 1;
        }
    }
    show();
    return 0;
}
