/*
 * sh: the RitterOS shell.
 *
 * Reads a line and splits it into words ('single' and "double" quotes keep
 * spaces in a word) and operators (51a, POSIX's): commands joined by '|'
 * into pipelines (one job: a process group, each command's output the
 * next one's input through a pipe; $? is the last one's exit code, '!'
 * before a pipeline inverts it), pipelines joined by && and || (the next
 * one runs only if the last succeeded, or failed), and those run one after
 * another at ';' or in the background at '&'. A word with '*' (and no quotes) becomes the names it
 * matches ('*': any characters within a name, in any part of a path;
 * names starting with '.' only if the pattern does; sorted; no match: the
 * word as typed). $NAME and ${NAME} (22t) become a variable's value, also
 * in "double quotes", not in 'single' ones; NAME=VALUE words before a
 * command set variables for it only, on their own the shell's (export
 * gives them to every program started; unset removes them). Then it runs it: a built-in command (cd, pwd, exit, help, export, unset) or
 * a program, found in the system's bin directory unless the name contains a
 * '/'. help lists both, the programs with their own help text (PROGRAM_HELP). Each program runs as a job: a process group of its own, which gets
 * the terminal (input and Ctrl-C) while the shell waits for it. A trailing
 * '&' runs it in the background instead; finished background jobs are
 * reported before the next prompt. "< FILE", "> FILE" and ">> FILE" (also
 * without the space; not in quotes) give a program a file as its standard
 * input or output (> replaces the file's content, >> appends); a number
 * before them (51a) names the descriptor (2> FILE: errors), and N>&M makes
 * N a copy of M (2>&1), applied in order, after the pipe's. Typed at the
 * prompt, a line whose last pipeline failed reports "NAME: exit code N"
 * (not one whose result && or || used; nothing in scripts or sh -c).
 *
 * The shell edits the typed line itself (the terminal in raw mode for a
 * line editor): Left/Right, Home/End move in it, typing and Backspace or
 * Delete work anywhere, Up/Down step through the commands typed since the
 * shell started (kept only in memory, at most 50; one repeated at once
 * is kept once). Ctrl-U clears the line, Ctrl-L clears the screen and
 * shows the line at its top. Lines longer than the screen are not redrawn
 * well when the cursor moves through them.
 *
 * The shell ignores Ctrl-C itself: at the prompt it cancels the line.
 * Ctrl-D on an empty line ends the shell, like exit.
 *
 * sh -l (a login shell, as login starts it at the console) first runs
 * ~/.config/login (15l) as a script: commands the user wants at every
 * console login, `windowd` for the window system as their interface.
 * sh -c TEXT [NAME ARGS] runs TEXT and ends; sh FILE [ARGS] runs a script.
 *
 * Since 51c the shell is POSIX's in its grammar: a text is parsed into a
 * tree (struct program; a function's body is parsed at each call) of lists,
 * and-or lists, pipelines and commands, simple or compound (if, while,
 * until, for, case, { }, ( ), functions); # comments; a command not
 * finished at a line's end goes on after "> ". There is neither fork nor
 * dup2 yet: the shell keeps its current standard input, output and error
 * (ctx) and passes them on, so redirections of compound commands and
 * built-ins apply here; true subshells run as another sh -c.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <threads.h>
#include <unistd.h>
#include "ritteros.h"

PROGRAM_HELP("sh [-l] | sh -c TEXT [NAME [ARG]...] | sh FILE [ARG]...",
             "the shell");

#define LINE_MAX    256
#define COMMAND_MAX 4096        /* a line from a pipe or a file (ssh HOST < script) */
#define ARGS_MAX    4096                /* words of a command (the kernel's limit too;
                                         * 37g-5a-2: 128 until then) */
#define ARGS_BYTES  131072              /* bytes of them (the kernel's ARGS_MAX) */
#define NAMES_SIZE  ARGS_BYTES          /* bytes of the names '*' expands to */
#define HISTORY     50                  /* commands kept for Up and Down */

static char bin_dir[128];               /* command-line programs: /system/<arch>/bin */
static char apps_dir[128];              /* windowed apps: /system/<arch>/apps */
static pid_t shell_group;
static pid_t shell_pid;                 /* $$: the shell's, also in its subshells (56c) */

/* (51c) Where a built-in's output goes and its input comes from (its own
 * redirections, or the shell's current ones). */
static int builtin_out = STDOUT_FILENO, builtin_in = STDIN_FILENO;
#define bprintf(...) dprintf(builtin_out, __VA_ARGS__)

/* The full path of a program named without a '/': in bin, else (42b) an
 * app bundle in apps, NAME.program/NAME. */
static void resolve(const char *name, char *out, size_t size)
{
    struct stat st;
    snprintf(out, size, "%s/%s", bin_dir, name);
    if (apps_dir[0] && stat(out, &st) != 0)
        snprintf(out, size, "%s/%s.program/%s", apps_dir, name, name);
}

/* --- variables (22t) --- */

/* The shell's own variables (NAME=value without export: not given to the
 * programs it starts); exported ones are the shell's environment. */
#define VARS_MAX 64
static char *var_names[VARS_MAX], *var_values[VARS_MAX];
static int vars;

static bool name_char(char c, bool first)
{
    return c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (!first && c >= '0' && c <= '9');
}

/* The length of a variable's name at the start of s (0: none there). */
static size_t name_length(const char *s)
{
    size_t n = 0;
    while (s[n] && name_char(s[n], n == 0))
        n++;
    return n;
}

static int own_variable(const char *name, size_t len)
{
    for (int i = 0; i < vars; i++)
        if (strlen(var_names[i]) == len && !strncmp(var_names[i], name, len))
            return i;
    return -1;
}

/* A variable's value: the shell's own, else the environment's; 0 unset. */
static const char *variable(const char *name, size_t len)
{
    int i = own_variable(name, len);
    if (i >= 0)
        return var_values[i];
    char n[128];
    if (len >= sizeof(n))
        return 0;
    memcpy(n, name, len);
    n[len] = 0;
    return getenv(n);
}

static void forget_own(const char *name)
{
    int i = own_variable(name, strlen(name));
    if (i < 0)
        return;
    free(var_names[i]);
    free(var_values[i]);
    vars--;
    var_names[i] = var_names[vars];
    var_values[i] = var_values[vars];
}

/* NAME=value: an exported variable stays exported (its new value goes to
 * the environment), any other is the shell's own. */
static void set_variable(const char *name, const char *value)
{
    if (getenv(name)) {
        setenv(name, value, 1);
        return;
    }
    int i = own_variable(name, strlen(name));
    if (i < 0) {
        if (vars == VARS_MAX) {
            dprintf(STDERR_FILENO, "sh: %s: too many variables (at most %d)\n", name, VARS_MAX);
            return;
        }
        i = vars++;
        var_names[i] = strdup(name);
        var_values[i] = 0;
    }
    free(var_values[i]);
    var_values[i] = strdup(value);
}

/* --- the line's tokens (51a) --- */

/* How the last pipeline ended, as an exit code ($?; sh -c ends with it):
 * its own, 128 + the signal that stopped it, 127 not found. */
static int last_status;

enum {
    T_WORD,                             /* raw, quoted */
    T_PIPE, T_AND, T_OR, T_SEMI, T_AMP, /* | && || ; & */
    T_REDIR,                            /* fd, how; its target is the next word */
    T_NEWLINE, T_LPAREN, T_RPAREN, T_DSEMI,     /* (51c) a line break ( ) ;; */
};
enum { R_IN, R_OUT, R_APPEND, R_DUP,    /* < > >> and <& >& */
       R_HEREDOC, R_HEREDOC_TABS };     /* (63i) << and <<- */

struct token {
    int kind;
    char *raw;                          /* T_WORD: the word as typed (quotes, $NAME) */
    bool quoted;                        /* it had quotes (no '*', not an operator) */
    int fd, how;                        /* T_REDIR: the descriptor (0-2) and the kind */
    int src, src_end;                   /* (51c) where it is in the program's text */
    char *body;                         /* (63i) a here-document's text (malloc'ed) */
    bool body_quoted;                   /* its word had quotes: no expansions in it */
};

/* (51c) A parsed text: a line typed, a script, a function's body, eval's
 * words. Each has its own tokens and nodes, so a function can be parsed
 * and run while the line that called it still runs. */
struct node;
struct program {
    char *source;                       /* the text (a copy) */
    struct token *tokens;
    int token_count;
    char *raw;                          /* the words' texts */
    struct node *nodes;
    int node_count, nodes_max;
};
static struct program *prog;            /* the one running */

#define TOKENS (prog->tokens)

/* The words of the command starting now, after expansion. */
static char words_text[ARGS_BYTES];
static char *words_end;

static bool operator_char(char c)
{
    return c == '|' || c == '&' || c == ';' || c == '<' || c == '>' || c == '(' || c == ')' ||
           c == '\n';
}

/* (51b) The end of a $(...), $((...)) or `...` starting at in (at its '$'
 * or '`'): just after its closing ')' or '`', nested ones, quotes and
 * backslashes inside skipped; 0 if it is not closed. */
static const char *substitution_end(const char *in)
{
    if (*in == '`') {
        for (in++; *in; in++) {
            if (*in == '\\' && in[1])
                in++;
            else if (*in == '`')
                return in + 1;
        }
        return 0;
    }
    int depth = 0;
    char quote = 0;
    for (in++; *in; in++) {
        if (quote) {
            if (*in == '\\' && quote == '"' && in[1])
                in++;
            else if (*in == quote)
                quote = 0;
        } else if (*in == '\\' && in[1]) {
            in++;
        } else if (*in == '\'' || *in == '"') {
            quote = *in;
        } else if (*in == '`') {
            const char *e = substitution_end(in);
            if (!e)
                return 0;
            in = e - 1;
        } else if (*in == '(') {
            depth++;
        } else if (*in == ')' && --depth == 0) {
            return in + 1;
        }
    }
    return 0;
}

#define INCOMPLETE (-2)                 /* (51c) the text ends inside a quote or a construct */

/* (63i) When the text ends inside a here-document: the line that ends it
 * (its delimiter), so the reader can gather lines up to it without parsing
 * the whole text again at each one. */
static char heredoc_waiting[256];
static bool heredoc_waiting_tabs;

/* A here-document's delimiter: its word with the quotes taken away. */
static void heredoc_delimiter(const char *raw, char *out, size_t size)
{
    size_t o = 0;
    for (const char *s = raw; *s && o + 1 < size; s++) {
        if (*s == '\\' && s[1])
            out[o++] = *++s;
        else if (*s != '\'' && *s != '"')
            out[o++] = *s;
    }
    out[o] = 0;
}

/* The bodies of the here-documents of the line just ended (tokens
 * pending[0..n), in order) from in: each up to its delimiter's line (with
 * <<- the lines' leading tabs dropped). Returns where the text goes on, or
 * 0 if it ends first (INCOMPLETE: heredoc_waiting says what ends it). */
static const char *heredoc_bodies(struct program *p, const char *in, const int *pending, int n)
{
    for (int k = 0; k < n; k++) {
        struct token *r = &p->tokens[pending[k]], *w = &p->tokens[pending[k] + 1];
        char delim[256];
        heredoc_delimiter(w->raw, delim, sizeof(delim));
        bool tabs = r->how == R_HEREDOC_TABS;
        size_t len = 0, cap = 256;
        char *body = malloc(cap);
        if (!body)
            return 0;
        for (;;) {
            if (!*in) {                         /* (no delimiter yet: more lines to come) */
                free(body);
                snprintf(heredoc_waiting, sizeof(heredoc_waiting), "%s", delim);
                heredoc_waiting_tabs = tabs;
                return 0;
            }
            const char *line = in;
            if (tabs)
                while (*line == '\t')
                    line++;
            size_t n_line = strcspn(line, "\n");
            in = line + n_line + (line[n_line] == '\n');
            if (n_line == strlen(delim) && !memcmp(line, delim, n_line))
                break;                          /* (its own line ends it) */
            if (len + n_line + 2 > cap) {
                cap = (len + n_line + 2) * 2;
                char *grown = realloc(body, cap);
                if (!grown) {
                    free(body);
                    return 0;
                }
                body = grown;
            }
            memcpy(body + len, line, n_line);
            len += n_line;
            body[len++] = '\n';
        }
        body[len] = 0;
        r->body = body;
        r->body_quoted = w->quoted;
    }
    return in;
}

/* Split p's text into tokens: words ('single' and "double" quotes keep
 * spaces and operators in a word; $(...) and `...` whole) and, outside
 * quotes, the operators | && || ; ;; & ( ), line breaks, and the
 * redirections < > >> <& >&, each with the descriptor written right before
 * it (2>, 2>&1); '#' at a word's start begins a comment to the line's end.
 * The words are kept as typed; expand_word makes them what the command
 * gets. Returns 0, INCOMPLETE (an unclosed quote or $( ), or -1 after a
 * message. */
static int tokenize(struct program *p)
{
    const char *in = p->source;
    size_t length = strlen(in);
    p->tokens = malloc((length + 1) * sizeof(struct token));
    p->raw = malloc(2 * length + 2);
    p->token_count = 0;
    if (!p->tokens || !p->raw) {
        dprintf(STDERR_FILENO, "sh: out of memory\n");
        return -1;
    }
    char *out = p->raw;
    int pending[16], pending_count = 0;         /* (63i) here-documents of this line */
    heredoc_waiting[0] = 0;
    for (;;) {
        while (*in == ' ' || *in == '\t' || (*in == '\\' && in[1] == '\n'))
            in += *in == '\\' ? 2 : 1;          /* (\ at a line's end: the line goes on) */
        if (*in == '#') {
            while (*in && *in != '\n')
                in++;
        }
        if (!*in) {
            if (pending_count) {                /* (their bodies come after a line break) */
                char delim[256];
                heredoc_delimiter(p->tokens[pending[0] + 1].raw, delim, sizeof(delim));
                snprintf(heredoc_waiting, sizeof(heredoc_waiting), "%s", delim);
                heredoc_waiting_tabs = p->tokens[pending[0]].how == R_HEREDOC_TABS;
                return INCOMPLETE;
            }
            return 0;
        }
        struct token *t = &p->tokens[p->token_count];
        memset(t, 0, sizeof(*t));
        t->src = (int)(in - p->source);
        const char *word_start = in;
        bool digits = true;
        char quote = 0;
        while (*in && (quote || (*in != ' ' && *in != '\t' && !operator_char(*in)))) {
            if (quote != '\'' && ((*in == '$' && in[1] == '(') || *in == '`')) {
                const char *e = substitution_end(in);   /* (51b: one word, spaces and all) */
                if (!e)
                    return INCOMPLETE;
                in = e;
                digits = false;
                continue;
            }
            if (*in == '\\' && quote != '\'' && in[1]) {
                in += 2;                            /* (51b: \x is x itself) */
                t->quoted = true;
                digits = false;
                continue;
            }
            if (quote && *in == quote)
                quote = 0;
            else if (!quote && (*in == '\'' || *in == '"'))
                quote = *in, t->quoted = true;
            digits &= *in >= '0' && *in <= '9';
            in++;
        }
        if (quote)
            return INCOMPLETE;
        size_t n = (size_t)(in - word_start);
        /* "2>": the digits right before a redirection are its descriptor */
        bool io_number = n && n <= 2 && digits && (*in == '<' || *in == '>');
        if (n && !io_number) {
            memcpy(out, word_start, n);
            out[n] = 0;
            t->kind = T_WORD;
            t->raw = out;
            out += n + 1;
            t->src_end = (int)(in - p->source);
            p->token_count++;
            if (p->token_count >= 2 && p->tokens[p->token_count - 2].kind == T_REDIR &&
                p->tokens[p->token_count - 2].how >= R_HEREDOC) {
                if (pending_count == 16) {
                    dprintf(STDERR_FILENO, "sh: more than 16 here-documents on one line\n");
                    return -1;
                }
                pending[pending_count++] = p->token_count - 2;
            }
            continue;
        }
        char c = *in++;
        t->kind = c == ';' ? T_SEMI : c == '|' ? T_PIPE : c == '&' ? T_AMP : c == '\n' ? T_NEWLINE
                : c == '(' ? T_LPAREN : c == ')' ? T_RPAREN : T_REDIR;
        if (c == '|' && *in == '|')
            t->kind = T_OR, in++;
        else if (c == '&' && *in == '&')
            t->kind = T_AND, in++;
        else if (c == ';' && *in == ';')
            t->kind = T_DSEMI, in++;
        else if (c == '<' || c == '>') {
            t->fd = io_number ? atoi(word_start) : c == '<' ? 0 : 1;
            t->how = c == '<' ? R_IN : R_OUT;
            if (c == '>' && *in == '>')
                t->how = R_APPEND, in++;
            else if (c == '<' && *in == '<') {  /* (63i) a here-document: << or <<- */
                in++;
                t->how = R_HEREDOC;
                if (*in == '-')
                    t->how = R_HEREDOC_TABS, in++;
            } else if (*in == '&')
                t->how = R_DUP, in++;
        }
        t->src_end = (int)(in - p->source);
        p->token_count++;
        if (t->kind == T_NEWLINE && pending_count) {    /* (63i) the bodies follow it */
            const char *after = heredoc_bodies(p, in, pending, pending_count);
            pending_count = 0;
            if (!after)
                return heredoc_waiting[0] ? INCOMPLETE : -1;
            in = after;
        }
    }
}

static void program_free(struct program *p)
{
    if (!p)
        return;
    for (int i = 0; p->tokens && i < p->token_count; i++)
        free(p->tokens[i].body);
    free(p->source);
    free(p->tokens);
    free(p->raw);
    free(p->nodes);
    free(p);
}

/* --- expansions (51b) --- */

/* The positional parameters: $0 (the shell's name, or sh -c's NAME) and
 * $1... (sh -c's ARGS; a script's arguments in 51c). */
static const char *param0 = "sh";
static char **params;
static int param_count;
static pid_t last_background;           /* $! */
static int substituted = -1;            /* the last $(...)'s exit code in this command, or -1 */
static bool opt_u;                      /* (51c) set -u: an unset variable is an error */
static bool opt_e, opt_x, opt_f;        /* set -e, -x, -f */
static bool interactive;                /* typed at a terminal (defined with the rest below) */
static int exit_requested;

/* One word of a command after expansion; a typed word can make several
 * (an unquoted $VAR or $(...) is split at IFS), or none. */
struct field {
    char *text;
    bool glob;                          /* no quotes in it: may match * ? [...] */
    int eq;                             /* a leading NAME= (an assignment), else 0 */
};

static struct field *fields;
static int field_count, fields_max;
static bool field_open;                 /* the current field has begun ("" begins one) */
static char *text_at, *text_end;        /* where the fields' text goes (words_text) */
static bool overflow;

static void begin_field(bool glob)
{
    if (field_open)
        return;
    if (field_count == fields_max) {
        overflow = true;
        return;
    }
    fields[field_count].text = text_at;
    fields[field_count].glob = glob;
    fields[field_count].eq = 0;
    field_open = true;
}

/* (68g) Expanding a case pattern (ESCAPE_CASE): a quoted * ? [ or \ gets
 * a \ before it, so match() takes it as itself while the unquoted parts
 * stay patterns (POSIX: only the quoted characters lose their meaning).
 * (68h) A command's words (ESCAPE_GLOB): every \ too (one from an unquoted
 * $VAR is itself), so a word not globbed gets back its exact text
 * (unescape). */
enum { ESCAPE_NONE, ESCAPE_CASE, ESCAPE_GLOB };
static int pattern_escapes;

static void add_char(char c, bool glob)
{
    bool mark = (pattern_escapes && !glob && (c == '*' || c == '?' || c == '[' || c == '\\')) ||
                (pattern_escapes == ESCAPE_GLOB && c == '\\');
    begin_field(glob);
    if (!field_open || text_at + mark >= text_end) {
        overflow = true;
        return;
    }
    if (!glob)
        fields[field_count].glob = false;           /* (a quoted part: no patterns in it) */
    if (mark)
        *text_at++ = '\\';                          /* (written here: one through add_char
                                                     * would be marked again, for ever) */
    *text_at++ = c;
}

static void end_field(void)
{
    if (!field_open)
        return;
    if (text_at >= text_end) {
        overflow = true;
        return;
    }
    *text_at++ = 0;
    field_count++;
    field_open = false;
}

/* IFS: the separators of a split (unset: space, tab, line break). */
static const char *separators(void)
{
    const char *ifs = variable("IFS", 3);
    return ifs ? ifs : " \t\n";
}

/* A value into the fields: as it is (quoted, or not to be split), or split
 * at IFS's characters (several in a row are one). */
static void add_value(const char *v, bool split, bool glob)
{
    if (!split) {
        begin_field(glob);
        for (; *v; v++)
            add_char(*v, glob);
        return;
    }
    const char *ifs = separators();
    for (; *v; v++) {
        if (*ifs && strchr(ifs, *v))
            end_field();
        else
            add_char(*v, glob);
    }
}

/* The output of `text` run in a subshell (56c: a fork of this shell, in
 * its group, with its variables, functions, options and parameters), its
 * trailing line breaks dropped; malloc'd, or 0 after a message. $? becomes
 * its exit code. */
static pid_t fork_subshell(const char *text, size_t length, const int fds[3], pid_t group,
                           bool background, bool join_shell);

static char *substitute(const char *text, size_t length)
{
    int pipe_fds[2];
    if (ritteros_pipe(pipe_fds) < 0) {
        dprintf(STDERR_FILENO, "sh: $(...): %s\n", strerror(errno));
        return 0;
    }
    int fds[3] = { -1, pipe_fds[1], -1 };
    pid_t pid = fork_subshell(text, length, fds, 0, false, true);
    close(pipe_fds[1]);
    if (pid < 0) {
        close(pipe_fds[0]);
        return 0;
    }
    size_t size = 4096, used = 0;
    char *out = malloc(size);
    for (;;) {
        if (out && used + 1024 > size) {
            char *bigger = realloc(out, size * 2);
            if (!bigger) {
                free(out);
                out = 0;
            } else {
                out = bigger;
                size *= 2;
            }
        }
        char sink[1024];
        ssize_t n = read(pipe_fds[0], out ? out + used : sink, 1024);
        if (n <= 0)
            break;
        if (out)
            used += (size_t)n;
    }
    close(pipe_fds[0]);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
        ;
    last_status = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    substituted = last_status;
    if (!out) {
        dprintf(STDERR_FILENO, "sh: $(...): out of memory\n");
        return 0;
    }
    while (used && out[used - 1] == '\n')
        used--;
    out[used] = 0;
    return out;
}

/* --- $((...)): integer arithmetic, C's operators (POSIX) --- */

static const char *arith_at;
static const char *arith_error;

static void arith_space(void)
{
    while (*arith_at == ' ' || *arith_at == '\t' || *arith_at == '\n')
        arith_at++;
}

static bool arith_take(const char *op)
{
    arith_space();
    size_t n = strlen(op);
    if (strncmp(arith_at, op, n) != 0)
        return false;
    /* not the start of a longer operator: < is not <=, & is not && */
    char next = arith_at[n];
    if ((n == 1 && strchr("<>&|=!", op[0]) && (next == '=' || next == op[0]) &&
         !(op[0] == '!' && next == '!')) ||
        (n == 2 && op[1] != '=' && next == '='))
        return false;
    arith_at += n;
    return true;
}

static int64_t arith_assign(void);

/* A variable's value as a number (unset or empty: 0). */
static int64_t arith_variable(const char *name, size_t len)
{
    const char *v = variable(name, len);
    if (!v || !*v)
        return 0;
    char *end;
    int64_t n = strtoll(v, &end, 0);
    if (*end && !arith_error)
        arith_error = "a variable is not a number";
    return n;
}

static void arith_set(const char *name, size_t len, int64_t value)
{
    char n[128], v[24];
    if (len >= sizeof(n))
        return;
    memcpy(n, name, len);
    n[len] = 0;
    snprintf(v, sizeof(v), "%lld", (long long)value);
    set_variable(n, v);
}

static int64_t arith_primary(void)
{
    arith_space();
    if (*arith_at == '(') {
        arith_at++;
        int64_t v = arith_assign();
        if (!arith_take(")") && !arith_error)
            arith_error = "a ')' is missing";
        return v;
    }
    if (*arith_at >= '0' && *arith_at <= '9') {
        char *end;
        int64_t v = strtoll(arith_at, &end, 0);
        arith_at = end;
        return v;
    }
    size_t len = name_length(arith_at);
    if (len) {
        const char *name = arith_at;
        arith_at += len;
        return arith_variable(name, len);
    }
    if (!arith_error)
        arith_error = *arith_at ? "a number is missing" : "the expression ends early";
    return 0;
}

static int64_t arith_unary(void)
{
    if (arith_take("-"))
        return -arith_unary();
    if (arith_take("+"))
        return arith_unary();
    if (arith_take("!"))
        return !arith_unary();
    if (arith_take("~"))
        return ~arith_unary();
    return arith_primary();
}

/* Binary operators by precedence, lowest last in the table's order. */
static int64_t arith_binary(int level)
{
    static const char *ops[][4] = {
        { "||" }, { "&&" }, { "|" }, { "^" }, { "&" }, { "==", "!=" },
        { "<=", ">=", "<", ">" }, { "<<", ">>" }, { "+", "-" }, { "*", "/", "%" },
    };
    if (level == 10)
        return arith_unary();
    int64_t left = arith_binary(level + 1);
    for (;;) {
        int k = 0;
        while (k < 4 && ops[level][k] && !arith_take(ops[level][k]))
            k++;
        if (k == 4 || !ops[level][k])
            return left;
        const char *op = ops[level][k];
        int64_t right = arith_binary(level + 1);
        if ((op[0] == '/' || op[0] == '%') && right == 0) {
            if (!arith_error)
                arith_error = "division by 0";
            return 0;
        }
        if (!strcmp(op, "||")) left = left || right;
        else if (!strcmp(op, "&&")) left = left && right;
        else if (!strcmp(op, "|")) left |= right;
        else if (!strcmp(op, "^")) left ^= right;
        else if (!strcmp(op, "&")) left &= right;
        else if (!strcmp(op, "==")) left = left == right;
        else if (!strcmp(op, "!=")) left = left != right;
        else if (!strcmp(op, "<=")) left = left <= right;
        else if (!strcmp(op, ">=")) left = left >= right;
        else if (!strcmp(op, "<")) left = left < right;
        else if (!strcmp(op, ">")) left = left > right;
        else if (!strcmp(op, "<<")) left = (int64_t)((uint64_t)left << (right & 63));
        else if (!strcmp(op, ">>")) left >>= right & 63;
        else if (!strcmp(op, "+")) left = (int64_t)((uint64_t)left + (uint64_t)right);
        else if (!strcmp(op, "-")) left = (int64_t)((uint64_t)left - (uint64_t)right);
        else if (!strcmp(op, "*")) left = (int64_t)((uint64_t)left * (uint64_t)right);
        else if (!strcmp(op, "/")) left = (left == INT64_MIN && right == -1) ? left : left / right;
        else left = (left == INT64_MIN && right == -1) ? 0 : left % right;
    }
}

static int64_t arith_conditional(void)
{
    int64_t c = arith_binary(0);
    if (!arith_take("?"))
        return c;
    int64_t a = arith_assign();
    if (!arith_take(":") && !arith_error)
        arith_error = "a ':' is missing";
    int64_t b = arith_assign();
    return c ? a : b;
}

/* NAME = value and NAME op= value set the shell's variable. */
static int64_t arith_assign(void)
{
    arith_space();
    size_t len = name_length(arith_at);
    if (len) {
        const char *name = arith_at, *after = arith_at + len;
        while (*after == ' ' || *after == '\t')
            after++;
        static const char *ops[] = { "=", "*=", "/=", "%=", "+=", "-=", "<<=", ">>=", "&=",
                                     "^=", "|=" };
        for (size_t k = 0; k < sizeof(ops) / sizeof(ops[0]); k++) {
            size_t n = strlen(ops[k]);
            if (strncmp(after, ops[k], n) || (k == 0 && after[1] == '='))
                continue;
            arith_at = after + n;
            int64_t v = arith_assign(), old = arith_variable(name, len);
            if ((k == 2 || k == 3) && v == 0) {
                if (!arith_error)
                    arith_error = "division by 0";
                return 0;
            }
            switch (k) {
            case 1: v = old * v; break;
            case 2: v = old / v; break;
            case 3: v = old % v; break;
            case 4: v = old + v; break;
            case 5: v = old - v; break;
            case 6: v = (int64_t)((uint64_t)old << (v & 63)); break;
            case 7: v = old >> (v & 63); break;
            case 8: v = old & v; break;
            case 9: v = old ^ v; break;
            case 10: v = old | v; break;
            }
            if (!arith_error)
                arith_set(name, len, v);
            return v;
        }
    }
    return arith_conditional();
}

/* The value of an arithmetic expression (its $ already expanded); false
 * after a message. */
static bool arithmetic(const char *text, int64_t *value)
{
    arith_at = text;
    arith_error = 0;
    *value = arith_assign();
    arith_space();
    if (!arith_error && *arith_at)
        arith_error = "an operator is missing";
    if (arith_error) {
        dprintf(STDERR_FILENO, "sh: $((%s)): %s\n", text, arith_error);
        return false;
    }
    return true;
}

static int expand_into(const char *in, const char *end, bool split, bool assignment);
static bool heredoc_mode;               /* (63i) expanding a here-document's body */

/* The text of in..end expanded as one string ($, `, $(( ))), for $((...))
 * and the like: malloc'd, or 0. */
static char *expand_string(const char *in, const char *end)
{
    struct field *saved_fields = fields;
    int saved_count = field_count, saved_max = fields_max;
    bool saved_open = field_open;
    int saved_escapes = pattern_escapes;
    pattern_escapes = ESCAPE_NONE;              /* (68g: a $((...)) is no pattern) */
    struct field one[1];
    fields = one;
    field_count = 0;
    fields_max = 1;
    field_open = false;
    char *start = text_at;
    int r = expand_into(in, end, false, false);
    end_field();
    char *copy = r == 0 && !overflow ? strdup(field_count ? one[0].text : "") : 0;
    text_at = start;
    fields = saved_fields;
    field_count = saved_count;
    fields_max = saved_max;
    field_open = saved_open;
    pattern_escapes = saved_escapes;
    return copy;
}

/* A $ expansion at in ('$' first): its value added to the fields; returns
 * where the text goes on, or 0 after an error (a message printed). quote:
 * inside double quotes (no splitting). */
static const char *dollar(const char *in, bool quote, bool split)
{
    const char *name = in + 1;
    char number[24];
    const char *value = 0;
    if (name[0] == '(' && name[1] == '(') {             /* $((arithmetic)) */
        const char *e = substitution_end(in);
        if (!e || e[-2] != ')')
            return 0;
        char *text = expand_string(name + 2, e - 2);
        int64_t v;
        bool ok = text && arithmetic(text, &v);
        free(text);
        if (!ok)
            return 0;
        snprintf(number, sizeof(number), "%lld", (long long)v);
        add_value(number, split && !quote, !quote);
        return e;
    }
    if (name[0] == '(') {                               /* $(command) */
        const char *e = substitution_end(in);
        if (!e)
            return 0;
        char *out = substitute(name + 1, (size_t)(e - 1 - (name + 1)));
        if (!out)
            return 0;
        add_value(out, split && !quote, !quote);
        free(out);
        return e;
    }
    bool braces = name[0] == '{';
    const char *n = braces ? name + 1 : name;
    size_t len = 0;
    bool all_params = false;
    if (*n == '?' || *n == '$' || *n == '!' || *n == '#' || *n == '@' || *n == '*' ||
        *n == '-' || (*n >= '0' && *n <= '9')) {
        len = 1;
        if (braces)
            while (n[0] >= '0' && n[0] <= '9' && n[len] >= '0' && n[len] <= '9')
                len++;
        switch (*n) {
        case '?': snprintf(number, sizeof(number), "%d", last_status); value = number; break;
        case '$': snprintf(number, sizeof(number), "%d", shell_pid); value = number; break;
        case '!':
            snprintf(number, sizeof(number), "%d", last_background);
            value = last_background ? number : "";
            break;
        case '#': snprintf(number, sizeof(number), "%d", param_count); value = number; break;
        case '-': value = ""; break;
        case '@': case '*': all_params = true; break;
        default: {
            int k = atoi(n);
            value = k == 0 ? param0 : k <= param_count ? params[k - 1] : "";
        }
        }
    } else {
        len = name_length(n);
    }
    if (!len || (braces && n[len] != '}')) {
        add_char('$', !quote);                          /* (a lone $: itself) */
        return in + 1;
    }
    const char *after = n + len + (braces ? 1 : 0);
    if (all_params) {
        /* "$@": each parameter a word of its own; $* and unquoted $@:
         * joined by spaces (then split again unless quoted). */
        if (quote && *n == '@') {
            for (int i = 0; i < param_count; i++) {
                if (i)
                    end_field();
                add_value(params[i], false, false);
            }
            return after;
        }
        for (int i = 0; i < param_count; i++) {
            if (i)
                add_value(" ", split && !quote, !quote);
            add_value(params[i], split && !quote, !quote);
        }
        if (quote)
            begin_field(false);
        return after;
    }
    if (!value)
        value = variable(n, len);
    if (!value && opt_u && (*n < '0' || *n > '9')) {
        dprintf(STDERR_FILENO, "sh: %.*s: not set\n", (int)len, n);   /* (set -u) */
        if (!interactive)
            exit_requested = 1;                         /* (a script ends there, POSIX) */
        return 0;
    }
    if (quote)
        begin_field(false);                             /* ("$EMPTY" is a word) */
    add_value(value ? value : "", split && !quote, !quote);
    return after;
}

/* Expand in..end into the fields: quotes taken away (they keep spaces and
 * stop splitting and patterns), \x as x, ~ at the start, $..., $(...),
 * `...` and $((...)). split: unquoted expansions split at IFS (not for
 * assignments and redirections). 0, or -1 after a message. */
static int expand_into(const char *in, const char *end, bool split, bool assignment)
{
    /* (63i) A here-document's body: as inside double quotes, but a '"' is
     * itself, and \ before a line break joins the lines. */
    char quote = heredoc_mode ? '"' : 0;
    bool start = !heredoc_mode;
    while (in < end) {
        if (start && !quote && *in == '~') {            /* ~ and ~user: a home */
            const char *e = in + 1;
            while (e < end && *e != '/' && (name_char(*e, false) || *e == '.' || *e == '-'))
                e++;
            if (e == end || *e == '/') {
                char user[64];
                size_t n = (size_t)(e - in - 1);
                struct passwd *pw = 0;
                if (n < sizeof(user)) {
                    memcpy(user, in + 1, n);
                    user[n] = 0;
                    const char *home = n == 0 ? variable("HOME", 4) : 0;
                    if (home) {
                        add_value(home, false, false);
                        in = e;
                        start = false;
                        continue;
                    }
                    pw = n == 0 ? getpwuid(getuid()) : getpwnam(user);
                }
                if (pw) {
                    add_value(pw->pw_dir, false, false);
                    in = e;
                    start = false;
                    continue;
                }
            }
        }
        start = false;
        char c = *in;
        if (quote == '\'') {
            if (c == '\'')
                quote = 0;
            else
                add_char(c, false);
            in++;
            continue;
        }
        if (heredoc_mode && c == '"') {
            add_char(c, false);
            in++;
            continue;
        }
        if (heredoc_mode && c == '\\' && in + 1 < end && in[1] == '\n') {
            in += 2;
            continue;
        }
        if (c == '\\' && in + 1 < end) {
            char next = in[1];
            if (quote == '"' && (!strchr("$`\"\\\n", next) || (heredoc_mode && next == '"'))) {
                add_char('\\', false);                  /* (in "": only before $ ` " \) */
                in++;
                continue;
            }
            add_char(next, false);
            in += 2;
            continue;
        }
        if (c == '"') {
            quote = quote ? 0 : '"';
            begin_field(false);                         /* ("" is a word) */
            in++;
            continue;
        }
        if (!quote && c == '\'') {
            quote = '\'';
            begin_field(false);
            in++;
            continue;
        }
        if (c == '$') {
            const char *next = dollar(in, quote == '"', split);
            if (!next)
                return -1;
            in = next;
            continue;
        }
        if (c == '`') {                                 /* `command`: as $(command) */
            const char *e = substitution_end(in);
            if (!e)
                return -1;
            char *inner = malloc((size_t)(e - in));
            size_t n = 0;
            for (const char *p = in + 1; p < e - 1; p++) {
                if (*p == '\\' && p + 1 < e - 1 && strchr("$`\\", p[1]))
                    p++;
                inner[n++] = *p;
            }
            char *out = inner ? substitute(inner, n) : 0;
            free(inner);
            if (!out)
                return -1;
            add_value(out, split && !quote, !quote);
            free(out);
            in = e;
            continue;
        }
        if (assignment && !quote && c == '=' && in[1] == '~')
            start = true;                               /* (NAME=~/x: a home too) */
        if (assignment && !quote && c == ':' && in[1] == '~')
            start = true;
        add_char(c, !quote);
        in++;
    }
    return 0;
}

/* A typed word as the command gets it: into out[] (at most room fields).
 * Assignments (a leading NAME= outside quotes) and, with split false,
 * redirections' files are not split. Returns the number of fields, or -1
 * after a message. */
static int expand_word(const struct token *t, struct field *out, int room, bool split)
{
    fields = out;
    fields_max = room;
    field_count = 0;
    field_open = false;
    overflow = false;
    text_at = words_end;
    text_end = words_text + sizeof(words_text) - 1;
    const char *raw = t->raw;
    size_t name = name_length(raw);
    bool assignment = name && raw[name] == '=';
    int r = expand_into(raw, raw + strlen(raw), split && !assignment, assignment);
    end_field();
    words_end = text_at;
    if (r < 0)
        return -1;
    if (overflow) {
        dprintf(STDERR_FILENO, "sh: the command is too long with what it expands to\n");
        return -1;
    }
    if (assignment && field_count) {
        out[0].eq = (int)name;
        out[0].glob = false;
    }
    return field_count;
}

/* --- the structure (51a; 51c: a tree, the compound commands) ---
 *   program    := list
 *   list       := and_or ((';' | '&' | newline) and_or)*   (empty ones skipped)
 *   and_or     := pipeline (('&&' | '||') newline* pipeline)*
 *   pipeline   := ['!'] command ('|' newline* command)*
 *   command    := simple | compound redirection* | NAME '(' ')' compound
 *   compound   := '(' list ')' | '{' list '}' | if | while | until | for | case
 */

enum { N_SIMPLE, N_PIPE, N_ANDOR, N_LIST, N_SUBSHELL, N_GROUP, N_IF, N_WHILE, N_UNTIL,
       N_FOR, N_CASE, N_ITEM, N_FUNC };

struct node {
    int kind;
    int first, next;                    /* children, and the next sibling (-1: none) */
    int start, end;                     /* its tokens (N_SIMPLE: words and redirections) */
    int redirs, redirs_end;             /* a compound's own redirections (tokens) */
    int word;                           /* N_FOR: the variable; N_CASE: the word; N_FUNC: the name */
    int list, list_end;                 /* N_FOR: the words after "in" (list < 0: none); N_ITEM: the patterns */
    int op;                             /* in an and-or: T_AND or T_OR before it */
    bool negate, background;
};

#define NODES (prog->nodes)

static int parse_at;                    /* the next token */
static int case_depth;                  /* inside a case's branches: ;; ends one */
static bool parse_incomplete;           /* the text ended inside something */

static int new_node(int kind)
{
    if (prog->node_count == prog->nodes_max) {
        int max = prog->nodes_max ? prog->nodes_max * 2 : 64;
        struct node *bigger = realloc(prog->nodes, (size_t)max * sizeof(struct node));
        if (!bigger)
            return -1;
        prog->nodes = bigger;
        prog->nodes_max = max;
    }
    int n = prog->node_count++;
    memset(&NODES[n], 0, sizeof(NODES[n]));
    NODES[n].kind = kind;
    NODES[n].first = NODES[n].next = NODES[n].list = -1;
    NODES[n].start = parse_at;
    return n;
}

static void add_child(int parent, int child)
{
    if (NODES[parent].first < 0) {
        NODES[parent].first = child;
        return;
    }
    int c = NODES[parent].first;
    while (NODES[c].next >= 0)
        c = NODES[c].next;
    NODES[c].next = child;
}

static const char *token_name(int i)
{
    if (i >= prog->token_count)
        return "the end";
    static const char *names[] = { "", "|", "&&", "||", ";", "&", "", "a line break", "(", ")",
                                   ";;" };
    if (TOKENS[i].kind == T_WORD)
        return TOKENS[i].raw;
    if (TOKENS[i].kind == T_REDIR)
        return TOKENS[i].how == R_IN ? "<" : TOKENS[i].how == R_APPEND ? ">>"
             : TOKENS[i].how >= R_HEREDOC ? "<<" : ">";
    return names[TOKENS[i].kind];
}

static int syntax_error(void)
{
    if (parse_at >= prog->token_count) {
        parse_incomplete = true;                /* (more may come: the next line) */
        return -1;
    }
    dprintf(STDERR_FILENO, "sh: syntax error near %s\n", token_name(parse_at));
    return -1;
}

static bool at_kind(int kind)
{
    return parse_at < prog->token_count && TOKENS[parse_at].kind == kind;
}

/* Is the next token the reserved word w (unquoted, in a command's place)? */
static bool at_word(const char *w)
{
    return at_kind(T_WORD) && !TOKENS[parse_at].quoted && !strcmp(TOKENS[parse_at].raw, w);
}

static bool reserved(int i)
{
    static const char *words[] = { "if", "then", "elif", "else", "fi", "do", "done", "case",
                                   "esac", "while", "until", "for", "{", "}", "!", "in" };
    if (TOKENS[i].kind != T_WORD || TOKENS[i].quoted)
        return false;
    for (size_t k = 0; k < sizeof(words) / sizeof(words[0]); k++)
        if (!strcmp(TOKENS[i].raw, words[k]))
            return true;
    return false;
}

static void skip_newlines(void)
{
    while (at_kind(T_NEWLINE))
        parse_at++;
}

static int expect_word(const char *w)
{
    skip_newlines();
    if (!at_word(w))
        return syntax_error();
    parse_at++;
    return 0;
}

static int parse_list(const char *const *stops);
static int parse_command(void);

/* A list up to one of the reserved words in stops (or ')', ';;', the end). */
static bool at_stop(const char *const *stops)
{
    if (parse_at >= prog->token_count || at_kind(T_RPAREN) || (case_depth && at_kind(T_DSEMI)))
        return true;
    for (; stops && *stops; stops++)
        if (at_word(*stops))
            return true;
    return false;
}

static int parse_pipeline(void)
{
    int p = new_node(N_PIPE);
    if (p < 0)
        return -1;
    if (at_word("!")) {
        NODES[p].negate = true;
        parse_at++;
    }
    for (;;) {
        int c = parse_command();
        if (c < 0)
            return -1;
        add_child(p, c);
        if (!at_kind(T_PIPE))
            break;
        parse_at++;
        skip_newlines();
    }
    NODES[p].end = parse_at;
    return p;
}

static int parse_and_or(void)
{
    int a = new_node(N_ANDOR);
    if (a < 0)
        return -1;
    int op = 0;
    for (;;) {
        int p = parse_pipeline();
        if (p < 0)
            return -1;
        NODES[p].op = op;
        add_child(a, p);
        if (!at_kind(T_AND) && !at_kind(T_OR))
            break;
        op = TOKENS[parse_at++].kind;
        skip_newlines();
    }
    NODES[a].end = parse_at;
    return a;
}

static int parse_list(const char *const *stops)
{
    int l = new_node(N_LIST);
    if (l < 0)
        return -1;
    for (;;) {
        while (at_kind(T_NEWLINE) || at_kind(T_SEMI) || (!case_depth && at_kind(T_DSEMI)))
            parse_at++;                         /* (empty commands: skipped, as since 44k;
                                                 * ";;" too, outside a case: the user's) */
        if (at_stop(stops))
            break;
        int a = parse_and_or();
        if (a < 0)
            return -1;
        add_child(l, a);
        if (at_kind(T_AMP)) {
            NODES[a].background = true;
            parse_at++;
        } else if (at_kind(T_SEMI) || at_kind(T_NEWLINE) || (!case_depth && at_kind(T_DSEMI))) {
            parse_at++;
        } else if (!at_stop(stops)) {
            return syntax_error();
        }
    }
    NODES[l].end = parse_at;
    return l;
}

/* A compound command's own redirections, after it. */
static int parse_redirections(int n)
{
    NODES[n].redirs = parse_at;
    while (at_kind(T_REDIR)) {
        parse_at++;
        if (!at_kind(T_WORD))
            return syntax_error();
        parse_at++;
    }
    NODES[n].redirs_end = parse_at;
    return 0;
}

static int parse_compound(void)
{
    static const char *const then_stops[] = { "then", 0 }, *const if_stops[] = { "elif", "else", "fi", 0 };
    static const char *const do_stops[] = { "do", 0 }, *const done_stops[] = { "done", 0 };
    static const char *const brace_stops[] = { "}", 0 }, *const esac_stops[] = { "esac", 0 };
    int n = -1;
    if (at_kind(T_LPAREN)) {                    /* ( list ) */
        n = new_node(N_SUBSHELL);
        parse_at++;
        int l = parse_list(0);
        if (l < 0)
            return -1;
        add_child(n, l);
        if (!at_kind(T_RPAREN))
            return syntax_error();
        parse_at++;
    } else if (at_word("{")) {
        n = new_node(N_GROUP);
        parse_at++;
        int l = parse_list(brace_stops);
        if (l < 0 || expect_word("}") < 0)
            return -1;
        add_child(n, l);
    } else if (at_word("if")) {
        n = new_node(N_IF);
        parse_at++;
        for (;;) {
            int c = parse_list(then_stops);
            if (c < 0 || expect_word("then") < 0)
                return -1;
            int t = parse_list(if_stops);
            if (t < 0)
                return -1;
            add_child(n, c);
            add_child(n, t);
            if (at_word("elif")) {
                parse_at++;
                continue;
            }
            if (at_word("else")) {
                parse_at++;
                int e = parse_list(if_stops);
                if (e < 0)
                    return -1;
                add_child(n, e);
            }
            break;
        }
        if (expect_word("fi") < 0)
            return -1;
    } else if (at_word("while") || at_word("until")) {
        n = new_node(at_word("while") ? N_WHILE : N_UNTIL);
        parse_at++;
        int c = parse_list(do_stops);
        if (c < 0 || expect_word("do") < 0)
            return -1;
        int b = parse_list(done_stops);
        if (b < 0 || expect_word("done") < 0)
            return -1;
        add_child(n, c);
        add_child(n, b);
    } else if (at_word("for")) {
        n = new_node(N_FOR);
        parse_at++;
        if (!at_kind(T_WORD) || !name_length(TOKENS[parse_at].raw) ||
            TOKENS[parse_at].raw[name_length(TOKENS[parse_at].raw)])
            return syntax_error();
        NODES[n].word = parse_at++;
        skip_newlines();
        if (at_word("in")) {
            parse_at++;
            NODES[n].list = parse_at;
            while (at_kind(T_WORD))
                parse_at++;
            NODES[n].list_end = parse_at;
            if (!at_kind(T_SEMI) && !at_kind(T_NEWLINE))
                return syntax_error();
            parse_at++;
        } else if (at_kind(T_SEMI)) {
            parse_at++;                         /* (no "in": "$@") */
        }
        if (expect_word("do") < 0)
            return -1;
        int b = parse_list(done_stops);
        if (b < 0 || expect_word("done") < 0)
            return -1;
        add_child(n, b);
    } else if (at_word("case")) {
        n = new_node(N_CASE);
        parse_at++;
        if (!at_kind(T_WORD))
            return syntax_error();
        NODES[n].word = parse_at++;
        if (expect_word("in") < 0)
            return -1;
        for (;;) {
            skip_newlines();
            if (at_word("esac"))
                break;
            int item = new_node(N_ITEM);
            if (item < 0)
                return -1;
            if (at_kind(T_LPAREN))
                parse_at++;
            NODES[item].list = parse_at;        /* pattern ( '|' pattern )* ')' */
            for (;;) {
                if (!at_kind(T_WORD))
                    return syntax_error();
                parse_at++;
                if (!at_kind(T_PIPE))
                    break;
                parse_at++;
            }
            NODES[item].list_end = parse_at;
            if (!at_kind(T_RPAREN))
                return syntax_error();
            parse_at++;
            case_depth++;
            int b = parse_list(esac_stops);
            case_depth--;
            if (b < 0)
                return -1;
            add_child(item, b);
            add_child(n, item);
            if (at_kind(T_DSEMI)) {
                parse_at++;
                continue;
            }
            skip_newlines();
            if (!at_word("esac"))
                return syntax_error();
        }
        parse_at++;
    } else {
        return syntax_error();
    }
    if (n < 0 || parse_redirections(n) < 0)
        return -1;
    NODES[n].end = parse_at;
    return n;
}

static int parse_command(void)
{
    if (at_kind(T_LPAREN) || at_word("{") || at_word("if") || at_word("while") ||
        at_word("until") || at_word("for") || at_word("case"))
        return parse_compound();
    /* NAME ( ) compound: a function */
    if (at_kind(T_WORD) && parse_at + 2 < prog->token_count &&
        TOKENS[parse_at + 1].kind == T_LPAREN && TOKENS[parse_at + 2].kind == T_RPAREN &&
        !reserved(parse_at)) {
        int f = new_node(N_FUNC);
        if (f < 0)
            return -1;
        NODES[f].word = parse_at;
        parse_at += 3;
        skip_newlines();
        int body = parse_compound();
        if (body < 0)
            return -1;
        add_child(f, body);
        NODES[f].end = parse_at;
        return f;
    }
    int s = new_node(N_SIMPLE);
    if (s < 0)
        return -1;
    while (at_kind(T_WORD) || at_kind(T_REDIR)) {
        if (at_kind(T_WORD) && parse_at == NODES[s].start && reserved(parse_at) &&
            !at_word("!"))
            return syntax_error();              /* (then, fi, done... out of place) */
        if (at_kind(T_REDIR)) {
            parse_at++;
            if (!at_kind(T_WORD))
                return syntax_error();
        }
        parse_at++;
    }
    if (parse_at == NODES[s].start)
        return syntax_error();
    NODES[s].end = parse_at;
    return s;
}

/* Tokenize and parse p (made the running program): its list's node, or
 * -1 (a message printed), or INCOMPLETE (the text ends inside a quote or
 * a construct: more lines may finish it). */
static int parse_program(struct program *p)
{
    struct program *saved = prog;
    prog = p;
    parse_incomplete = false;
    case_depth = 0;
    int r = tokenize(p);
    if (r == 0) {
        parse_at = 0;
        r = parse_list(0);
        if (r >= 0 && parse_at < p->token_count)
            r = syntax_error();
        if (r < 0 && parse_incomplete)
            r = INCOMPLETE;
    }
    prog = saved;
    return r;
}

/* How a job ended, unless normally: an exit code, Ctrl-C, killed. (A fault
 * was reported by the kernel already.) */
static void report(const char *name, int status)
{
    if (WIFEXITED(status)) {
        if (WEXITSTATUS(status))
            printf("%s: exit code %d\n", name, WEXITSTATUS(status));
    } else if (WTERMSIG(status) == SIGINT) {
        printf("^C\n");
    } else if (WTERMSIG(status) == SIGKILL) {
        printf("%s: killed\n", name);
    } else if (WTERMSIG(status) != SIGSEGV && WTERMSIG(status) != SIGILL &&
               WTERMSIG(status) != SIGFPE && WTERMSIG(status) != SIGBUS) {
        printf("%s: stopped by signal %d\n", name, WTERMSIG(status));
    }
}

/* Background pipelines: only their last command is reported when it ends
 * (the others are waited for quietly). */
#define QUIET_MAX 64
static pid_t quiet[QUIET_MAX];

static bool take_quiet(pid_t pid)
{
    for (int i = 0; i < QUIET_MAX; i++)
        if (quiet[i] == pid) {
            quiet[i] = 0;
            return true;
        }
    return false;
}

static void reap_background(void)
{
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        if (take_quiet(pid) || !interactive)
            continue;                           /* (scripts and sh -c report no jobs, as POSIX's) */
        if (WIFEXITED(status))
            printf("[%d] done, exit code %d\n", pid, WEXITSTATUS(status));
        else
            printf("[%d] stopped by signal %d\n", pid, WTERMSIG(status));
    }
}

/* --- '*': names matching a pattern ------------------------------------------ */

static char expanded[NAMES_SIZE];       /* the expanded words of one command */
static size_t expanded_used;

/* (51b) The end of a [...] set at p ('[' first), or 0 if it is not one.
 * (68g) \X in it is X. */
static const char *set_end(const char *p)
{
    const char *q = p + 1;
    if (*q == '!' || *q == '^')
        q++;
    if (*q == ']')
        q++;                                        /* (a ']' first is one of the set) */
    while (*q && *q != ']')
        q += *q == '\\' && q[1] ? 2 : 1;
    return *q ? q + 1 : 0;
}

/* Is c in the set [...] at p? */
static bool in_set(const char *p, const char *end, char c)
{
    const char *q = p + 1;
    bool invert = *q == '!' || *q == '^';
    if (invert)
        q++;
    bool found = false;
    for (bool first = true; q < end - 1; first = false) {
        if (*q == ']' && !first)
            break;
        if (*q == '\\' && q + 1 < end - 1)               /* (68g) \X: X itself */
            q++;
        if (q[1] == '-' && q + 2 < end - 1) {
            found |= c >= q[0] && c <= q[2];
            q += 3;
        } else {
            found |= c == *q;
            q++;
        }
    }
    return found != invert;
}

/* Does name match pattern? '*': any run of characters, also none; '?': one
 * character; [abc], [a-z]: one of the set, [!...]: one not in it (51b);
 * \X: X itself (68g). */
static bool match(const char *pattern, const char *name)
{
    for (; *pattern; pattern++, name++) {
        if (*pattern == '*') {
            while (pattern[1] == '*')
                pattern++;
            for (;; name++) {
                if (match(pattern + 1, name))
                    return true;
                if (!*name)
                    return false;
            }
        }
        if (!*name)
            return false;
        if (*pattern == '?')
            continue;
        if (*pattern == '\\' && pattern[1]) {
            if (*name != *++pattern)
                return false;
            continue;
        }
        const char *e = *pattern == '[' ? set_end(pattern) : 0;
        if (e) {
            if (!in_set(pattern, e, *name))
                return false;
            pattern = e - 1;
            continue;
        }
        if (*name != *pattern)
            return false;
    }
    return !*name;
}

/* Has the word a pattern in it (* ? or a whole [...]; (68h) not \X)? */
static bool has_pattern(const char *w)
{
    for (; *w; w++) {
        if (*w == '\\' && w[1]) {
            w++;
            continue;
        }
        if (*w == '*' || *w == '?' || (*w == '[' && set_end(w)))
            return true;
    }
    return false;
}

/* (68h) A word's text without its \ marks (ESCAPE_GLOB's), in place. */
static void unescape(char *s)
{
    char *o = s;
    for (; *s; s++) {
        if (*s == '\\' && s[1])
            s++;
        *o++ = *s;
    }
    *o = 0;
}

/* (68h) Is this expanded word to be globbed (an unquoted * ? [...], not
 * an assignment, no set -f)? Else its marks come off now. */
static bool glob_ready(struct field *f)
{
    if (!opt_f && !f->eq && has_pattern(f->text))
        return true;
    unescape(f->text);
    return false;
}

/* Add prefix + name (+ "/") to the words; false if there is no room. */
static bool add_name(char **out, int *n, const char *prefix, const char *name, bool slash)
{
    size_t need = strlen(prefix) + strlen(name) + (slash ? 1 : 0) + 1;
    if (*n >= ARGS_MAX || expanded_used + need > sizeof(expanded))
        return false;
    char *w = expanded + expanded_used;
    snprintf(w, need, "%s%s%s", prefix, name, slash ? "/" : "");
    expanded_used += need;
    out[(*n)++] = w;
    return true;
}

/* The names matching rest (a pattern, '/' between its parts) below prefix
 * (the path so far: "", or ending in '/'), added to out; false if they do
 * not fit. */
static bool expand_path(const char *prefix, const char *rest, char **out, int *n)
{
    const char *slash = strchr(rest, '/');
    size_t length = slash ? (size_t)(slash - rest) : strlen(rest);
    char part[256], path[256];
    if (length >= sizeof(part))
        return true;
    memcpy(part, rest, length);
    part[length] = 0;
    if (!has_pattern(part)) {                       /* a plain part: take it as it is */
        struct stat st;
        unescape(part);                             /* (68h: its quoted characters) */
        snprintf(path, sizeof(path), "%s%s%s", prefix, part, slash ? "/" : "");
        if (slash && slash[1])
            return expand_path(path, slash + 1, out, n);
        return stat(path, &st) < 0 || add_name(out, n, path, "", false);
    }
    DIR *dir = opendir(*prefix ? prefix : ".");
    if (!dir)
        return true;
    bool ok = true;
    struct dirent *e;
    while (ok && (e = readdir(dir))) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0 ||
            (e->d_name[0] == '.' && part[0] != '.') || !match(part, e->d_name))
            continue;
        if (!slash)
            ok = add_name(out, n, prefix, e->d_name, false);
        else if (e->d_type != DT_DIR)
            continue;                               /* a path goes on: directories only */
        else if (!slash[1])
            ok = add_name(out, n, prefix, e->d_name, true);
        else {
            snprintf(path, sizeof(path), "%s%s/", prefix, e->d_name);
            ok = expand_path(path, slash + 1, out, n);
        }
    }
    closedir(dir);
    return ok;
}

/* Replace each word with '*' (no quotes) by the names it matches, sorted.
 * Returns the new number of words, or -1 after a message. */
static int expand(char **words, bool *quoted, int count)
{
    static char *out[ARGS_MAX + 1];             /* (static: 36 KiB, and it never nests) */
    static bool out_quoted[ARGS_MAX + 1];
    int n = 0;
    expanded_used = 0;
    for (int i = 0; i < count; i++) {
        int first = n;
        if (!quoted[i] && has_pattern(words[i])) {
            const char *w = words[i];
            bool ok = *w == '/' ? expand_path("/", w + 1, out, &n) : expand_path("", w, out, &n);
            if (!ok) {
                dprintf(STDERR_FILENO, "sh: %s: too many names (at most %d words)\n", w,
                        ARGS_MAX);
                return -1;
            }
            for (int a = first + 1; a < n; a++)     /* sorted (insertion sort) */
                for (int b = a; b > first && strcmp(out[b - 1], out[b]) > 0; b--) {
                    char *t = out[b];
                    out[b] = out[b - 1];
                    out[b - 1] = t;
                }
        }
        if (n == first && !quoted[i])
            unescape(words[i]);                     /* (68h) no match: its text, unmarked */
        if (n == first) {                           /* no '*', or no match: as typed */
            if (n >= ARGS_MAX) {
                dprintf(STDERR_FILENO, "sh: too many words (at most %d)\n", ARGS_MAX);
                return -1;
            }
            out[n++] = words[i];
        }
        for (int k = first; k < n; k++)
            out_quoted[k] = quoted[i] && n == first + 1 && out[first] == words[i];
    }
    for (int i = 0; i < n; i++) {
        words[i] = out[i];
        quoted[i] = out_quoted[i];
    }
    words[n] = 0;
    return n;
}

/* Read size bytes at offset of an open file; false if they are not all there. */
static bool read_at(int fd, uint64_t offset, void *buf, size_t size)
{
    if (lseek(fd, (off_t)offset, SEEK_SET) != (off_t)offset)
        return false;
    for (size_t done = 0; done < size;) {
        ssize_t n = read(fd, (char *)buf + done, size - done);
        if (n <= 0)
            return false;
        done += (size_t)n;
    }
    return true;
}

/* A program's help text ("usage\tdescription", from PROGRAM_HELP): the ELF
 * section .ritteros.help, found through the section headers at the end of
 * the file and the table of their names. */
static bool program_help(const char *path, char *out, size_t size)
{
    unsigned char eh[64], sh[64 * 64];
    char names[1024];
    bool found = false;
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return false;
    uint64_t shoff;
    uint16_t entsize, count, strndx;
    if (!read_at(fd, 0, eh, sizeof(eh)) || memcmp(eh, "\x7f" "ELF", 4))
        goto out;
    memcpy(&shoff, eh + 40, 8);
    memcpy(&entsize, eh + 58, 2);
    memcpy(&count, eh + 60, 2);
    memcpy(&strndx, eh + 62, 2);
    if (entsize != 64 || count > 64 || strndx >= count || !read_at(fd, shoff, sh, count * 64u))
        goto out;
    uint64_t names_at, names_size;
    memcpy(&names_at, sh + strndx * 64 + 24, 8);
    memcpy(&names_size, sh + strndx * 64 + 32, 8);
    if (names_size > sizeof(names) - 1 || !read_at(fd, names_at, names, names_size))
        goto out;
    names[names_size] = 0;
    for (int i = 0; i < count && !found; i++) {
        uint32_t name;
        uint64_t at, length;
        memcpy(&name, sh + i * 64, 4);
        memcpy(&at, sh + i * 64 + 24, 8);
        memcpy(&length, sh + i * 64 + 32, 8);
        if (name < names_size && strcmp(names + name, ".ritteros.help") == 0 &&
            length < size && read_at(fd, at, out, length)) {
            out[length] = 0;
            found = true;
        }
    }
out:
    close(fd);
    return found;
}

static void help_line(const char *usage, const char *description)
{
    if (strlen(usage) < 16)
        bprintf("  %-16s%s\n", usage, description);
    else                                    /* too long: the description below it */
        bprintf("  %s\n  %16s%s\n", usage, "", description);
}

/* Everything the user can type: the built-in commands, then every program
 * in the bin directory with its own help text, sorted by name. */
static void help(void)
{
    bprintf("Built into the shell:\n");
    help_line("cd [DIR]", "change the current directory (default: your home)");
    help_line("pwd", "print the current directory");
    help_line("exit [N]", "end the shell; the kernel's rescue console follows");
    help_line("help", "this list");
    help_line("COMMAND &", "run COMMAND in the background");
    help_line("COMMAND > FILE", "its output into FILE (>> FILE: added at the end)");
    help_line("COMMAND < FILE", "its input from FILE");
    help_line("COMMAND; COMMAND", "one after the other");
    help_line("*", "in a word: the names it matches (*.txt, /home/*/notes)");
    help_line("NAME=VALUE", "set a variable (before a command: for it only)");
    help_line("$NAME ${NAME}", "in a word: the variable's value (not in 'single quotes')");
    help_line("export [NAME[=VALUE]]...", "give variables to the programs started; alone: list them");
    help_line("unset NAME...", "remove variables (and functions)");
    help_line("A | B", "A's output is B's input (one job)");
    help_line("A && B, A || B", "B only if A succeeded, or failed");
    help_line("if, while, until, for, case", "the shell's control flow (see the manual)");
    help_line("NAME() { ...; }", "a function; called like a command, its words in $1...");
    help_line(": . eval exec", "nothing; run a file here; run words; run, then end");
    help_line("set shift read", "options -e -u -x -f, --; drop $1; read a line into names");
    help_line("break continue return", "leave a loop, its next round, a function");
    help_line("umask [-S] [MODE]", "the bits taken off new files' modes (0022: they get 0644)");
    bprintf("Programs (%s and %s):\n", bin_dir, apps_dir);

    enum { HELP_PROGRAMS = 256 };                         /* (65d: 96 cut the 97th off) */
    static char names[HELP_PROGRAMS][64];
    int count = 0;
    const char *dirs[2] = { bin_dir, apps_dir };         /* command-line programs, then apps */
    for (int di = 0; di < 2; di++) {
        DIR *d = opendir(dirs[di]);
        struct dirent *e;
        while (d && count < HELP_PROGRAMS && (e = readdir(d))) {
            size_t len = strlen(e->d_name);
            if (di == 0 && e->d_type == DT_REG && len < sizeof(names[0]) && !strchr(e->d_name, '.'))
                snprintf(names[count++], sizeof(names[0]), "%s", e->d_name);
            else if (di == 1 && e->d_type == DT_DIR && len > 8 && len - 8 < sizeof(names[0]) &&
                     !strcmp(e->d_name + len - 8, ".program"))   /* (42b) an app's bundle */
                snprintf(names[count++], sizeof(names[0]), "%.*s", (int)(len - 8), e->d_name);
        }
        if (d)
            closedir(d);
    }
    for (int i = 1; i < count; i++) {                   /* insertion sort */
        char key[64];
        memcpy(key, names[i], sizeof(key));
        int j = i - 1;
        for (; j >= 0 && strcmp(names[j], key) > 0; j--)
            memcpy(names[j + 1], names[j], sizeof(key));
        memcpy(names[j + 1], key, sizeof(key));
    }
    for (int i = 0; i < count; i++) {
        char path[256], text[256];
        resolve(names[i], path, sizeof(path));          /* bin, else an app's bundle */
        char *tab = 0;
        if (program_help(path, text, sizeof(text)) && (tab = strchr(text, '\t'))) {
            *tab = 0;
            help_line(text, tab + 1);
        } else {
            help_line(names[i], "(no description)");
        }
    }
}

static int number(const char *s)
{
    int n = 0;
    while (*s >= '0' && *s <= '9')
        n = n * 10 + (*s++ - '0');
    return n;
}

/* export (22t): NAME=VALUE or NAME (the shell's own variable) into the
 * environment, which the programs started get; alone, the environment's
 * variables, sorted. */
static int by_name(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static void export(int count, char **words)
{
    if (count == 1) {
        size_t n = 0;
        while (environ[n])
            n++;
        char **sorted = malloc((n + 1) * sizeof(char *));
        if (!sorted)
            return;
        memcpy(sorted, environ, n * sizeof(char *));
        qsort(sorted, n, sizeof(char *), by_name);
        for (size_t i = 0; i < n; i++)
            bprintf("%s\n", sorted[i]);
        free(sorted);
        return;
    }
    for (int i = 1; i < count; i++) {
        char *eq = strchr(words[i], '=');
        size_t len = eq ? (size_t)(eq - words[i]) : strlen(words[i]);
        if (len == 0 || name_length(words[i]) != len) {
            dprintf(STDERR_FILENO, "export: %s: not a name\n", words[i]);
            continue;
        }
        if (eq) {
            *eq = 0;
            setenv(words[i], eq + 1, 1);
        } else {
            const char *own = variable(words[i], len);
            if (own)
                setenv(words[i], own, 1);
        }
        forget_own(words[i]);
    }
}

/* --- running a program (51a; 51c: the tree, functions, options) --- */

static bool interactive;                /* typed at a terminal: report how the line ended */
static int exit_requested = -1;         /* `exit` ran: the shell ends with this code */

/* The last foreground pipeline's ending, reported once the line is done. */
static char last_name[64];
static int last_raw = -1;               /* its wait status, -1: nothing to report */

/* (51c) The shell's own standard input, output and error as its commands
 * get them (-1: its descriptor 0, 1 or 2): a compound command's or a
 * built-in's redirections change them while it runs, `exec > FILE` for good
 * (there is no dup2 yet: the shell passes them on instead). */
static int ctx[3] = { -1, -1, -1 };          /* (56c: forked subshells reset it) */

static int fd_or(const int fds[3], int k)
{
    return fds[k] >= 0 ? fds[k] : k;
}

/* (51c) set's options, and what is going on. */
static int condition_depth;             /* in an if/while condition, before && or ||, after ! */
static int loop_depth, breaking, continuing;
static int function_depth, dot_depth;
static bool returning;

static bool stopping(void)
{
    return exit_requested >= 0 || breaking || continuing || returning;
}

/* (51c) The functions defined: a name and its body's text, parsed again
 * at each call. */
#define FUNCTIONS_MAX 64
static char *function_names[FUNCTIONS_MAX], *function_bodies[FUNCTIONS_MAX];
static int functions;

static int function_index(const char *name)
{
    for (int i = 0; i < functions; i++)
        if (!strcmp(function_names[i], name))
            return i;
    return -1;
}

static const char *node_text(int n, size_t *length)
{
    int a = NODES[n].start, b = NODES[n].end - 1;
    if (b < a) {
        *length = 0;
        return "";
    }
    *length = (size_t)(TOKENS[b].src_end - TOKENS[a].src);
    return prog->source + TOKENS[a].src;
}

static bool is_builtin(const char *w)
{
    static const char *names[] = { "cd", "pwd", "exit", "help", "export", "unset", ":", ".",
                                   "eval", "exec", "set", "shift", "read", "break", "continue",
                                   "return", "umask" };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (!strcmp(w, names[i]))
            return true;
    return false;
}

static int ctx[3];
static int condition_depth, loop_depth, breaking, continuing, function_depth, dot_depth;
static bool returning;
static int list_depth;
static int execute_text(const char *text);

/* (56c) A subshell: a fork of this shell runs text (( ... ), $(...), a
 * built-in or compound command in a pipeline, `a && b &`) with fds as its
 * standard input, output and error -- the shell's own variables, functions,
 * options and parameters are in its memory already, and $$ stays this
 * shell's. Its process ID, or -1 after a message. group: a process group
 * to join, 0 for a new one (in the foreground unless background);
 * join_shell: stay in the shell's own group ($(...)). The child waits
 * until its group (and the terminal) are set, so it never reads the
 * terminal from the background. */
static pid_t fork_subshell(const char *text, size_t length, const int fds[3], pid_t group,
                           bool background, bool join_shell)
{
    int sync[2];
    if (ritteros_pipe(sync) < 0) {
        dprintf(STDERR_FILENO, "sh: a subshell: %s\n", strerror(errno));
        return -1;
    }
    pid_t pid = fork();
    if (pid < 0) {
        dprintf(STDERR_FILENO, "sh: a subshell: %s\n", strerror(errno));
        close(sync[0]);
        close(sync[1]);
        return -1;
    }
    if (pid == 0) {
        close(sync[1]);
        pattern_escapes = ESCAPE_NONE;              /* (68g: a $(...) in a pattern) */
        if (!join_shell)
            setpgid(0, group);
        char c;
        while (read(sync[0], &c, 1) < 0 && errno == EINTR)
            ;                                       /* (until the parent closes its end) */
        /* Its standard descriptors (any order: through copies first), then
         * nothing else of the shell's: a pipe's end held here would keep
         * its reader from ever seeing the end. */
        int copies[3];
        for (int k = 0; k < 3; k++)
            copies[k] = fcntl(fds[k] >= 0 ? fds[k] : ctx[k] >= 0 ? ctx[k] : k, F_DUPFD, 3);
        for (int k = 0; k < 3; k++)
            if (copies[k] >= 0)
                dup2(copies[k], k);
        for (int fd = 3; fd < FD_MAX; fd++)
            close(fd);
        ctx[0] = ctx[1] = ctx[2] = -1;
        builtin_out = STDOUT_FILENO;
        builtin_in = STDIN_FILENO;
        interactive = false;                        /* (as sh -c was: no job reports) */
        shell_group = getpgrp();
        condition_depth = loop_depth = breaking = continuing = 0;
        function_depth = dot_depth = list_depth = 0;
        returning = false;
        char *copy = strndup(text, length);
        int code = copy ? execute_text(copy) : -1;
        exit(code >= 0 ? code : last_status);
    }
    close(sync[0]);
    if (!join_shell) {
        setpgid(pid, group ? group : pid);          /* (both: whichever comes first) */
        if (!group && !background)
            tcsetpgrp(STDIN_FILENO, pid);           /* (allowed or not, as spawn's foreground) */
    }
    close(sync[1]);
    snprintf(last_name, sizeof(last_name), "sh");
    return pid;
}

/* The exit code of a wait status. */
static int code_of(int status)
{
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

static int wait_for(pid_t pid)
{
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
        ;
    return status;
}

/* --- built-in commands --- */


static int run_text(const char *text, size_t length);

/* New positional parameters (copies), the old ones freed if they were. */
static bool params_owned;

static void set_params(char **words, int count)
{
    char **copy = malloc((size_t)(count + 1) * sizeof(char *));
    if (!copy)
        return;
    for (int i = 0; i < count; i++)
        copy[i] = strdup(words[i]);
    copy[count] = 0;
    if (params_owned) {
        for (int i = 0; i < param_count; i++)
            free(params[i]);
        free(params);
    }
    params = copy;
    param_count = count;
    params_owned = true;
}

/* read [-r] NAME...: a line from the standard input, split at IFS into the
 * names (the last gets the rest); 1 at the input's end. One byte at a
 * time, so what comes after the line is left for the next reader. */
static int builtin_read(int count, char **words)
{
    bool raw = false;
    int first = 1;
    if (first < count && !strcmp(words[first], "-r"))
        raw = true, first++;
    static char line[COMMAND_MAX];
    size_t n = 0;
    bool got = false, eof = false;
    for (;;) {
        char c;
        if (read(builtin_in, &c, 1) != 1) {
            eof = true;
            break;
        }
        got = true;
        if (c == '\n')
            break;
        if (c == '\\' && !raw) {
            if (read(builtin_in, &c, 1) != 1)
                break;
            if (c == '\n')
                continue;                       /* (a line going on) */
        }
        if (n + 1 < sizeof(line))
            line[n++] = c;
    }
    line[n] = 0;
    const char *ifs = separators();
    char *at = line;
    for (int i = first; i < count; i++) {
        while (*at && strchr(ifs, *at) && strchr(" \t\n", *at))
            at++;                               /* (IFS's spaces around a field go) */
        char *end = at;
        if (i + 1 < count) {
            while (*end && !strchr(ifs, *end))
                end++;
        } else {
            end = at + strlen(at);
            while (end > at && strchr(ifs, end[-1]) && strchr(" \t\n", end[-1]))
                end--;
        }
        char saved = *end;
        *end = 0;
        if (name_length(words[i]) == strlen(words[i]))
            set_variable(words[i], at);
        *end = saved;
        at = *end ? end + 1 : end;
    }
    return eof && !got ? 1 : eof && n == 0 ? 1 : 0;
}

/* umask [-S] [MODE] (52h, POSIX): the bits taken off new files' modes,
 * shown in octal (0022) or as what stays allowed (-S: u=rwx,g=rx,o=rx);
 * MODE is octal or symbolic like chmod's ([ugoa]*[+-=][rwx]*, commas
 * between), which says what is allowed. A built-in: a program could not
 * change the shell's own mask. */
static int builtin_umask(int count, char **words)
{
    bool symbolic = count > 1 && !strcmp(words[1], "-S");
    int first = symbolic ? 2 : 1;
    mode_t mask = umask(0);
    umask(mask);
    if (first >= count) {
        if (!symbolic) {
            bprintf("%04o\n", mask);
            return 0;
        }
        mode_t allowed = ~mask & 0777;
        const char *who = "ugo";
        for (int k = 0; k < 3; k++) {
            unsigned bits = allowed >> (6 - 3 * k) & 7;
            bprintf("%s%c=%s%s%s", k ? "," : "", who[k], bits & 4 ? "r" : "", bits & 2 ? "w" : "",
                    bits & 1 ? "x" : "");
        }
        bprintf("\n");
        return 0;
    }
    const char *m = words[first];
    if (*m >= '0' && *m <= '7') {
        char *end;
        long v = strtol(m, &end, 8);
        if (*end || v > 0777) {
            dprintf(STDERR_FILENO, "umask: %s: not a mode\n", m);
            return 1;
        }
        umask((mode_t)v);
        return 0;
    }
    mode_t allowed = ~mask & 0777;
    while (*m) {
        unsigned who = 0;
        for (; *m && strchr("ugoa", *m); m++)
            who |= *m == 'u' ? 0700 : *m == 'g' ? 0070 : *m == 'o' ? 0007 : 0777;
        if (!who)
            who = 0777;
        char op = *m;
        if (op != '+' && op != '-' && op != '=') {
            dprintf(STDERR_FILENO, "umask: %s: not a mode\n", words[first]);
            return 1;
        }
        m++;
        unsigned perm = 0;
        for (; *m && *m != ','; m++) {
            if (*m == 'r')
                perm |= 0444;
            else if (*m == 'w')
                perm |= 0222;
            else if (*m == 'x')
                perm |= 0111;
            else {
                dprintf(STDERR_FILENO, "umask: %s: not a mode\n", words[first]);
                return 1;
            }
        }
        perm &= who;
        if (op == '+')
            allowed |= perm;
        else if (op == '-')
            allowed &= ~perm;
        else
            allowed = (allowed & ~who) | perm;
        if (*m == ',')
            m++;
    }
    umask(~allowed & 0777);
    return 0;
}

static int builtin_set(int count, char **words)
{
    if (count == 1) {                           /* the variables, the shell's own first */
        for (int i = 0; i < vars; i++)
            bprintf("%s=%s\n", var_names[i], var_values[i]);
        for (char **e = environ; *e; e++)
            bprintf("%s\n", *e);
        return 0;
    }
    int i = 1;
    for (; i < count && (words[i][0] == '-' || words[i][0] == '+'); i++) {
        if (!strcmp(words[i], "--")) {
            i++;
            set_params(words + i, count - i);
            return 0;
        }
        bool on = words[i][0] == '-';
        for (const char *o = words[i] + 1; *o; o++) {
            if (*o == 'e')
                opt_e = on;
            else if (*o == 'u')
                opt_u = on;
            else if (*o == 'x')
                opt_x = on;
            else if (*o == 'f')
                opt_f = on;
            else {
                dprintf(STDERR_FILENO, "set: -%c: not an option (e, u, x, f)\n", *o);
                return 2;
            }
        }
    }
    if (i < count)
        set_params(words + i, count - i);
    return 0;
}

/* A file's text, malloc'd and NUL-ended, or 0 (errno set). */
static char *read_file(const char *path, size_t *length)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return 0;
    size_t size = 4096, used = 0;
    char *text = malloc(size);
    for (;;) {
        if (!text)
            break;
        if (used + 1024 >= size) {
            char *bigger = realloc(text, size * 2);
            if (!bigger) {
                free(text);
                text = 0;
                break;
            }
            text = bigger;
            size *= 2;
        }
        ssize_t n = read(fd, text + used, size - used - 1);
        if (n <= 0)
            break;
        used += (size_t)n;
    }
    close(fd);
    if (!text) {
        errno = ENOMEM;
        return 0;
    }
    text[used] = 0;
    *length = used;
    return text;
}

/* A built-in command: its exit code. */
static int builtin(int count, char **words)
{
    char cwd[256];
    const char *w = words[0];
    if (!strcmp(w, "cd")) {
        struct passwd *pw = count > 1 ? 0 : getpwuid(getuid());
        const char *dir = count > 1 ? words[1] : pw ? pw->pw_dir : "/";
        if (chdir(dir) < 0) {
            dprintf(STDERR_FILENO, "cd: %s: %s\n", dir, strerror(errno));
            return 1;
        }
    } else if (!strcmp(w, "pwd")) {
        bprintf("%s\n", getcwd(cwd, sizeof(cwd)) ? cwd : "?");
    } else if (!strcmp(w, "exit")) {
        exit_requested = count > 1 ? number(words[1]) : last_status;
    } else if (!strcmp(w, "help")) {
        help();
    } else if (!strcmp(w, "export")) {
        export(count, words);
    } else if (!strcmp(w, "unset")) {
        for (int i = 1; i < count; i++) {
            forget_own(words[i]);
            unsetenv(words[i]);
            int f = function_index(words[i]);
            if (f >= 0) {
                free(function_names[f]);
                free(function_bodies[f]);
                functions--;
                function_names[f] = function_names[functions];
                function_bodies[f] = function_bodies[functions];
            }
        }
    } else if (!strcmp(w, ":")) {
        return 0;
    } else if (!strcmp(w, "set")) {
        return builtin_set(count, words);
    } else if (!strcmp(w, "umask")) {
        return builtin_umask(count, words);
    } else if (!strcmp(w, "shift")) {
        int n = count > 1 ? number(words[1]) : 1;
        if (n > param_count) {
            dprintf(STDERR_FILENO, "shift: only %d parameters\n", param_count);
            return 1;
        }
        set_params(params + n, param_count - n);
    } else if (!strcmp(w, "read")) {
        return builtin_read(count, words);
    } else if (!strcmp(w, "break") || !strcmp(w, "continue")) {
        int n = count > 1 ? number(words[1]) : 1;
        if (n < 1)
            n = 1;
        if (n > loop_depth)
            n = loop_depth;
        if (w[0] == 'b')
            breaking = n;
        else
            continuing = n;
    } else if (!strcmp(w, "return")) {
        if (!function_depth && !dot_depth) {
            dprintf(STDERR_FILENO, "return: not in a function\n");
            return 1;
        }
        returning = true;
        return count > 1 ? number(words[1]) : last_status;
    } else if (!strcmp(w, "eval")) {
        size_t n = 0;
        for (int i = 1; i < count; i++)
            n += strlen(words[i]) + 1;
        char *text = malloc(n + 1), *at = text;
        if (!text)
            return 1;
        *at = 0;
        for (int i = 1; i < count; i++)
            at += sprintf(at, "%s%s", i > 1 ? " " : "", words[i]);
        int r = run_text(text, (size_t)(at - text));
        free(text);
        return r;
    } else if (!strcmp(w, ".")) {
        if (count < 2) {
            dprintf(STDERR_FILENO, ".: a file name is missing\n");
            return 2;
        }
        size_t length;
        char *text = read_file(words[1], &length);
        if (!text) {
            dprintf(STDERR_FILENO, ".: %s: %s\n", words[1], strerror(errno));
            return 1;
        }
        char **saved = params;
        int saved_count = param_count;
        bool saved_owned = params_owned;
        if (count > 2) {
            params_owned = false;
            set_params(words + 2, count - 2);
        }
        dot_depth++;
        int r = run_text(text, length);
        dot_depth--;
        returning = false;
        if (count > 2) {
            set_params(saved, saved_count);     /* (copies again; the file's go) */
            params_owned = true;
            if (saved_owned) {
                for (int i = 0; i < saved_count; i++)
                    free(saved[i]);
                free(saved);
            }
        }
        free(text);
        return r;
    }
    return 0;
}

/* A function call: the body run with the words as $1...; return ends it. */
static int call_function(int f, int count, char **words)
{
    char **saved = params;
    int saved_count = param_count;
    bool saved_owned = params_owned;
    params_owned = false;
    set_params(words + 1, count - 1);
    function_depth++;
    char *body = strdup(function_bodies[f]);     /* (it may be redefined meanwhile) */
    int r = body ? run_text(body, strlen(body)) : 1;
    free(body);
    function_depth--;
    returning = false;
    for (int i = 0; i < param_count; i++)
        free(params[i]);
    free(params);
    params = saved;
    param_count = saved_count;
    params_owned = saved_owned;
    return r;
}

/* (63i) A here-document's text as the command reads it: expanded unless
 * its word was quoted, a line at a time (so a long one does not meet the
 * words' buffer; a line ending in \ goes on with the next). malloc'ed, or
 * 0 after a message. */
static char *heredoc_text(const struct token *t)
{
    if (t->body_quoted)
        return strdup(t->body);
    size_t len = 0, cap = strlen(t->body) + 64;
    char *all = malloc(cap);
    if (!all)
        return 0;
    for (const char *s = t->body; *s;) {
        const char *e = s;
        while (*e && !(*e == '\n' && (e == s || e[-1] != '\\')))
            e++;
        if (*e)
            e++;                                /* (the line break goes with it) */
        text_at = words_end;
        text_end = words_text + sizeof(words_text) - 1;
        overflow = false;
        heredoc_mode = true;
        char *piece = expand_string(s, e);
        heredoc_mode = false;
        if (!piece || overflow) {
            free(piece);
            free(all);
            if (overflow)
                dprintf(STDERR_FILENO, "sh: a here-document line is too long with what it expands to "
                                       "(quote its word: <<'END')\n");
            return 0;
        }
        size_t n = strlen(piece);
        if (len + n + 1 > cap) {
            cap = (len + n + 1) * 2;
            char *grown = realloc(all, cap);
            if (!grown) {
                free(piece);
                free(all);
                return 0;
            }
            all = grown;
        }
        memcpy(all + len, piece, n);
        len += n;
        free(piece);
        s = e;
    }
    all[len] = 0;
    return all;
}

struct heredoc_writer {
    int fd;
    char *text;
    size_t len;
};

/* (63i) A here-document larger than a pipe holds: written by a thread of
 * its own while the command reads it. */
static int write_heredoc(void *arg)
{
    struct heredoc_writer *w = arg;
    for (size_t at = 0; at < w->len;) {
        ssize_t n = write(w->fd, w->text + at, w->len - at);
        if (n <= 0)
            break;                              /* (the reader is gone: the rest is not wanted) */
        at += (size_t)n;
    }
    close(w->fd);
    free(w->text);
    free(w);
    return 0;
}

/* The descriptor a here-document is read from, or -1 after a message. */
static int heredoc_fd(const struct token *t)
{
    char *text = heredoc_text(t);
    if (!text)
        return -1;
    int ends[2];
    if (pipe(ends) < 0) {
        dprintf(STDERR_FILENO, "sh: <<: %s\n", strerror(errno));
        free(text);
        return -1;
    }
    fcntl(ends[1], F_SETFD, FD_CLOEXEC);        /* (an exec'd program must not hold it) */
    size_t len = strlen(text);
    if (len <= 65536) {                         /* (a pipe holds that much: written at once) */
        for (size_t at = 0; at < len;) {
            ssize_t n = write(ends[1], text + at, len - at);
            if (n <= 0)
                break;
            at += (size_t)n;
        }
        close(ends[1]);
        free(text);
        return ends[0];
    }
    struct heredoc_writer *w = malloc(sizeof(*w));
    thrd_t thread;
    if (!w) {
        free(text);
        close(ends[0]);
        close(ends[1]);
        return -1;
    }
    w->fd = ends[1], w->text = text, w->len = len;
    if (thrd_create(&thread, write_heredoc, w) != thrd_success) {
        dprintf(STDERR_FILENO, "sh: <<: no thread to write it\n");
        free(text);
        free(w);
        close(ends[0]);
        close(ends[1]);
        return -1;
    }
    thrd_detach(thread);
    return ends[0];
}

/* Open a command's redirections (tokens [from, to)) onto fds (which start
 * as what it would get otherwise); the files opened go into opened (to be
 * closed after it started). -1 after a message. */
static int redirect(int from, int to, int fds[3], int *opened, int *open_count)
{
    static struct field got[2];
    for (int i = from; i < to; i++) {
        struct token *t = &TOKENS[i];
        if (t->kind != T_REDIR)
            continue;
        if (t->how >= R_HEREDOC) {              /* (63i) its text through a pipe */
            i++;                                /* (its word: the delimiter, already used) */
            if (t->fd > 2) {
                dprintf(STDERR_FILENO, "sh: %d<<: only 0, 1 and 2 can be redirected\n", t->fd);
                return -1;
            }
            if (*open_count == 8) {
                dprintf(STDERR_FILENO, "sh: too many redirections\n");
                return -1;
            }
            int fd = t->body ? heredoc_fd(t) : -1;
            if (fd < 0)
                return -1;
            opened[(*open_count)++] = fd;
            fds[t->fd] = fd;
            continue;
        }
        int n = expand_word(&TOKENS[++i], got, 1, false);   /* (a file: one word) */
        if (n < 0)
            return -1;
        const char *target = n ? got[0].text : "";
        if (t->fd > 2) {
            dprintf(STDERR_FILENO, "sh: %d%s: only 0, 1 and 2 can be redirected\n", t->fd,
                    token_name(i - 1));
            return -1;
        }
        if (t->how == R_DUP) {
            if (!(target[0] >= '0' && target[0] <= '2') || target[1]) {
                dprintf(STDERR_FILENO, "sh: %s: not 0, 1 or 2\n", target);
                return -1;
            }
            int k = target[0] - '0';
            fds[t->fd] = fds[k] >= 0 ? fds[k] : k;
            continue;
        }
        if (*open_count == 8) {
            dprintf(STDERR_FILENO, "sh: too many redirections\n");
            return -1;
        }
        int flags = t->how == R_IN ? O_RDONLY : t->how == R_APPEND
                    ? O_WRONLY | O_CREAT | O_APPEND : O_WRONLY | O_CREAT | O_TRUNC;
        int fd = open(target, flags, 0666);
        if (fd < 0) {
            dprintf(STDERR_FILENO, "sh: %s: %s\n", target, strerror(errno));
            return -1;
        }
        opened[(*open_count)++] = fd;
        fds[t->fd] = fd;
    }
    return 0;
}

static void close_all(int *opened, int count)
{
    for (int k = 0; k < count; k++)
        close(opened[k]);
}

/* (52g) A file the kernel would not start (ENOEXEC) that is no binary:
 * no ELF header and no NUL byte in its first 512 bytes. POSIX's shells
 * run such a file as a script; a program built for another processor
 * stays "Exec format error". */
static bool is_script(const char *path)
{
    unsigned char b[512];
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return false;
    ssize_t n = read(fd, b, sizeof(b));
    close(fd);
    if (n < 0 || (n >= 4 && !memcmp(b, "\177ELF", 4)))
        return false;
    return !memchr(b, 0, (size_t)n);
}

/* Start a program: words[0] found in bin (then apps) unless it has a '/',
 * the NAME=value assignments for it only. Its process ID, or -1 with
 * *status set (127 not found, 126 not started) and a message. */
static pid_t launch(char **words, char **assigned, int *eqs, int assigns, int fds[3], pid_t group,
                    bool background, int *status)
{
    static char *saved_env[ARGS_MAX];           /* (static: launch never nests) */
    for (int i = 0; i < assigns; i++) {
        assigned[i][eqs[i]] = 0;
        const char *had = getenv(assigned[i]);
        saved_env[i] = had ? strdup(had) : 0;
        setenv(assigned[i], assigned[i] + eqs[i] + 1, 1);
    }
    char path[256];
    bool named = !strchr(words[0], '/');
    if (named)
        resolve(words[0], path, sizeof(path));      /* bin, else apps */
    else
        snprintf(path, sizeof(path), "%s", words[0]);
    uint64_t flags = group ? SPAWN_JOIN_GROUP | (uint64_t)group << 32
                           : SPAWN_NEW_GROUP | (background ? 0 : SPAWN_FOREGROUND);
    int32_t copy[3] = { fds[0], fds[1], fds[2] };
    int64_t pid = syscall6(SYS_SPAWN_ENV, (int64_t)path, (int64_t)words, (int64_t)environ,
                           (int64_t)flags, (int64_t)copy, 0);
    if (pid == -ENOEXEC && is_script(path)) {       /* (52g) sh PATH ARGS, as POSIX's */
        int count = 0;
        while (words[count])
            count++;
        char **again = malloc((size_t)(count + 2) * sizeof(char *));
        if (again) {
            char shell[300];
            snprintf(shell, sizeof(shell), "%s/sh", bin_dir);
            again[0] = "sh";
            again[1] = path;
            for (int i = 1; i <= count; i++)
                again[i + 1] = words[i];
            pid = syscall6(SYS_SPAWN_ENV, (int64_t)shell, (int64_t)again, (int64_t)environ,
                           (int64_t)flags, (int64_t)copy, 0);
            free(again);
        }
    }
    for (int i = assigns - 1; i >= 0; i--) {
        if (saved_env[i])
            setenv(assigned[i], saved_env[i], 1);
        else
            unsetenv(assigned[i]);
        free(saved_env[i]);
    }
    if (pid < 0) {
        errno = (int)-pid;
        if (named && errno == ENOENT)
            dprintf(STDERR_FILENO, "sh: %s: command not found\n", words[0]);
        else
            dprintf(STDERR_FILENO, "sh: %s: %s\n", words[0], strerror(errno));
        *status = errno == ENOENT ? 127 : 126;
        return -1;
    }
    *status = 0;
    snprintf(last_name, sizeof(last_name), "%s", words[0]);
    return (pid_t)pid;
}

/* (37g-5a-2) A command's words: ARGS_MAX of them are 86 KiB, too much for
 * a stack frame at every level of functions and eval, so each level has
 * its own, kept for the next command at that level. */
struct command_words {
    char *words[ARGS_MAX + 1];
    bool quoted[ARGS_MAX + 1];
    char *assigned[ARGS_MAX];
    int eqs[ARGS_MAX];
};
static struct command_words **levels;
static int level, levels_made;

static pid_t start_words(int c, int fds[3], pid_t group, bool background, bool alone,
                         int *status, struct command_words *w);

/* Start one simple command with fds as its standard input, output and
 * error (-1: the shell's current ones, ctx), after its own redirections,
 * which apply in order and win over the pipe's (POSIX). group: the
 * pipeline's process group to join, 0 for a new one. Its process ID; 0 if
 * it was a built-in, a function or an assignment (run here, *status set),
 * -1 if it did not start (*status set, a message printed). */
static pid_t start_command(int c, int fds[3], pid_t group, bool background, bool alone,
                           int *status)
{
    if (level == levels_made) {
        struct command_words **more = realloc(levels, (size_t)(level + 1) * sizeof(*levels));
        struct command_words *w = more ? malloc(sizeof(*w)) : 0;
        if (more)
            levels = more;
        if (!w) {
            dprintf(STDERR_FILENO, "sh: out of memory\n");
            *status = 1;
            return -1;
        }
        levels[levels_made++] = w;
    }
    struct command_words *w = levels[level++];
    pid_t pid = start_words(c, fds, group, background, alone, status, w);
    level--;
    return pid;
}

static pid_t start_words(int c, int fds[3], pid_t group, bool background, bool alone,
                         int *status, struct command_words *w)
{
    char **words = w->words;
    bool *quoted = w->quoted;
    int count = 0, assigns = 0;
    char **assigned = w->assigned;
    int *eqs = w->eqs;
    int opened[8], open_count = 0;
    *status = 0;
    for (int k = 0; k < 3; k++)
        if (fds[k] < 0)
            fds[k] = ctx[k];
    words_end = words_text;                         /* (the commands before have started) */
    substituted = -1;
    static struct field got[ARGS_MAX + 1];
    for (int i = NODES[c].start; i < NODES[c].end; i++) {
        struct token *t = &TOKENS[i];
        if (t->kind == T_REDIR) {
            i++;                                    /* (opened below, in order) */
            continue;
        }
        pattern_escapes = ESCAPE_GLOB;              /* (68h: quoted parts marked) */
        int n = expand_word(t, got, ARGS_MAX - count, true);    /* (51b: maybe several) */
        pattern_escapes = ESCAPE_NONE;
        if (n < 0)
            goto failed;
        static bool globbed[ARGS_MAX + 1];          /* (used at once, as got) */
        for (int k = 0; k < n; k++)
            globbed[k] = glob_ready(&got[k]);
        if (n == 1 && count == 0 && got[0].eq && assigns < ARGS_MAX) {
            assigned[assigns] = got[0].text;
            eqs[assigns++] = got[0].eq;
            continue;
        }
        for (int k = 0; k < n; k++) {
            quoted[count] = !globbed[k];
            words[count++] = got[k].text;
        }
    }
    words[count] = 0;
    if (redirect(NODES[c].start, NODES[c].end, fds, opened, &open_count) < 0)
        goto failed;
    if (count == 0) {
        /* NAME=value alone (22t): the shell's own variables; its exit code
         * is its last $(...)'s, if any (POSIX: X=$(false) fails) */
        for (int i = 0; i < assigns; i++) {
            assigned[i][eqs[i]] = 0;
            set_variable(assigned[i], assigned[i] + eqs[i] + 1);
        }
        close_all(opened, open_count);
        *status = substituted >= 0 ? substituted : 0;
        return 0;
    }
    if ((count = expand(words, quoted, count)) < 0)
        goto failed;
    if (opt_x) {                                    /* set -x: the command, before it runs */
        dprintf(fd_or(fds, 2), "+");
        for (int i = 0; i < count; i++)
            dprintf(fd_or(fds, 2), " %s", words[i]);
        dprintf(fd_or(fds, 2), "\n");
    }
    int f = function_index(words[0]);
    if (f >= 0 || is_builtin(words[0])) {
        if (!alone) {                               /* (the caller runs those in a subshell) */
            dprintf(STDERR_FILENO, "sh: %s: cannot run here\n", words[0]);
            goto failed;
        }
        if (!strcmp(words[0], "exec")) {
            if (count == 1) {                       /* exec > FILE: for the rest of the shell */
                for (int k = 0; k < 3; k++)
                    ctx[k] = fds[k];
                return 0;
            }
            /* (55b) exec CMD: the shell becomes CMD, as POSIX's: its
             * redirections on 0-2, its assignments in the environment,
             * SIGINT back to the default (the shell ignores it itself). */
            for (int k = 0; k < 3; k++)
                if (fd_or(fds, k) != k)
                    dup2(fd_or(fds, k), k);
            for (int i = 0; i < assigns; i++) {
                assigned[i][eqs[i]] = 0;
                setenv(assigned[i], assigned[i] + eqs[i] + 1, 1);
            }
            char path[256];
            if (strchr(words[1], '/'))
                snprintf(path, sizeof(path), "%s", words[1]);
            else
                resolve(words[1], path, sizeof(path));
            signal(SIGINT, SIG_DFL);
            execve(path, words + 1, environ);
            if (errno == ENOEXEC && is_script(path)) {     /* (52g: sh PATH ARGS) */
                char shell[300];
                snprintf(shell, sizeof(shell), "%s/sh", bin_dir);
                static char *again[ARGS_MAX + 2];
                int k = 0;
                again[k++] = "sh";
                again[k++] = path;
                for (int i = 2; i < count && k < ARGS_MAX + 1; i++)
                    again[k++] = words[i];
                again[k] = 0;
                execve(shell, again, environ);
            }
            int e = errno;
            signal(SIGINT, SIG_IGN);
            dprintf(STDERR_FILENO, "sh: %s: %s\n", words[1],
                    e == ENOENT && !strchr(words[1], '/') ? "command not found" : strerror(e));
            *status = e == ENOENT ? 127 : 126;
            if (!interactive)
                exit_requested = *status;           /* (POSIX: only a script ends at it) */
            close_all(opened, open_count);
            return 0;
        }
        /* The built-in or the function gets the redirected input and output. */
        int saved[3] = { ctx[0], ctx[1], ctx[2] }, saved_out = builtin_out, saved_in = builtin_in;
        for (int k = 0; k < 3; k++)
            ctx[k] = fds[k];
        builtin_out = fd_or(fds, 1);
        builtin_in = fd_or(fds, 0);
        *status = f >= 0 ? call_function(f, count, words) : builtin(count, words);
        builtin_out = saved_out;
        builtin_in = saved_in;
        for (int k = 0; k < 3; k++)
            ctx[k] = saved[k];
        close_all(opened, open_count);
        return 0;
    }
    pid_t pid = launch(words, assigned, eqs, assigns, fds, group, background, status);
    close_all(opened, open_count);                  /* the program has its own copies */
    return pid;
failed:
    close_all(opened, open_count);
    *status = 1;
    return -1;
}

static int run_list(int l);
static int run_compound(int n);

/* A pipeline's command runs as a program of its own: a simple command
 * whose name is no built-in or function (judged from the word as typed). */
static bool external(int c)
{
    if (NODES[c].kind != N_SIMPLE)
        return false;
    for (int i = NODES[c].start; i < NODES[c].end; i++) {
        struct token *t = &TOKENS[i];
        if (t->kind == T_REDIR) {
            i++;
            continue;
        }
        size_t n = name_length(t->raw);
        if (n && t->raw[n] == '=')
            continue;                               /* (an assignment) */
        return !is_builtin(t->raw) && function_index(t->raw) < 0;
    }
    return true;
}

/* Run a pipeline: its commands joined by pipes, one process group (one
 * job), waited for unless in the background. Built-ins, functions and
 * compound commands in it run in a subshell each (a fork since 56c); a
 * pipeline of one runs them here. Its exit code: the last command's ($?),
 * inverted by '!'. */
static int run_pipeline(int p, bool background)
{
    int count = 0;
    for (int c = NODES[p].first; c >= 0; c = NODES[c].next)
        count++;
    int first = NODES[p].first;
    last_raw = -1;
    if (count == 1 && !background && NODES[first].kind != N_SIMPLE &&
        NODES[first].kind != N_SUBSHELL) {
        int r = run_compound(first);                /* (here, in the shell) */
        return NODES[p].negate ? !r : r;
    }
    pid_t pids[count];                              /* (sized to it: the stack is 64 KiB, */
    int codes[count];                               /* and loops nest pipelines in it) */
    pid_t group = 0;
    int in = -1, i = 0;
    for (int c = first; c >= 0; c = NODES[c].next, i++) {
        int pipe_fds[2] = { -1, -1 };
        if (NODES[c].next >= 0 && ritteros_pipe(pipe_fds) < 0) {
            dprintf(STDERR_FILENO, "sh: a pipe: %s\n", strerror(errno));
            for (; i < count; i++)
                pids[i] = -1, codes[i] = 1;
            break;
        }
        int fds[3] = { in >= 0 ? in : ctx[0], pipe_fds[1] >= 0 ? pipe_fds[1] : ctx[1], ctx[2] };
        codes[i] = 0;
        if (NODES[c].kind == N_SIMPLE && (count == 1 || external(c))) {
            pids[i] = start_command(c, fds, group, background, count == 1 && !background,
                                    &codes[i]);
        } else {                                    /* a subshell: ( ... ), or one in a pipeline */
            size_t length;
            const char *text;
            int opened[8], open_count = 0;
            if (NODES[c].kind == N_SUBSHELL) {      /* (its own redirections: here) */
                text = node_text(NODES[c].first, &length);
                if (redirect(NODES[c].redirs, NODES[c].redirs_end, fds, opened, &open_count) < 0) {
                    pids[i] = -1, codes[i] = 1;
                } else {
                    pids[i] = fork_subshell(text, length, fds, group, background, false);
                    codes[i] = pids[i] < 0 ? 126 : 0;
                }
                close_all(opened, open_count);
            } else {
                text = node_text(c, &length);
                pids[i] = fork_subshell(text, length, fds, group, background, false);
                codes[i] = pids[i] < 0 ? 126 : 0;
            }
        }
        if (in >= 0)
            close(in);                              /* (the commands have their own ends) */
        if (pipe_fds[1] >= 0)
            close(pipe_fds[1]);
        in = pipe_fds[0];
        if (pids[i] > 0 && !group)
            group = pids[i];
        if (exit_requested >= 0)
            break;
    }
    if (in >= 0)
        close(in);
    int last = count - 1, code = codes[last];
    if (exit_requested >= 0)
        return code;
    if (background) {
        for (int k = 0; k < last; k++)
            for (int q = 0; q < QUIET_MAX && pids[k] > 0; q++)
                if (!quiet[q]) {
                    quiet[q] = pids[k];
                    break;
                }
        if (pids[last] > 0) {
            if (interactive)
                printf("[%d]\n", pids[last]);
            last_background = pids[last];           /* $! */
        }
        return 0;
    }
    /* The job got the terminal as it started (SPAWN_FOREGROUND); the shell
     * takes it back once all its commands have ended. */
    bool started = false, interrupted = false;
    for (int k = 0; k <= last; k++) {
        if (pids[k] <= 0)
            continue;
        started = true;
        int status = wait_for(pids[k]);
        interrupted |= !WIFEXITED(status) && WTERMSIG(status) == SIGINT;
        if (k == last) {
            code = code_of(status);
            last_raw = status;
        }
    }
    if (started)
        tcsetpgrp(STDIN_FILENO, shell_group);
    if (interrupted && interactive)
        printf("^C\n");                             /* (at once: the line goes on) */
    if ((interrupted && last_raw >= 0 && !WIFEXITED(last_raw) && WTERMSIG(last_raw) == SIGINT) ||
        NODES[p].negate)
        last_raw = -1;                              /* (shown already; or '!' made it a success) */
    return NODES[p].negate ? !code : code;
}

static int list_depth;                  /* 0: the line's own list (its ending is reported) */
static int reported = -1;

/* An and-or list: each pipeline only if && or || ask for it; set -e ends
 * the shell after a failure that no condition looked at. */
static int run_and_or(int a)
{
    if (NODES[a].background) {
        int p = NODES[a].first;
        if (NODES[p].next < 0)
            return run_pipeline(p, true);
        size_t length;                              /* a && b &: a subshell (56c: a fork) */
        const char *text = node_text(a, &length);
        pid_t pid = fork_subshell(text, length, ctx, 0, true, false);
        if (pid > 0) {
            if (interactive)
                printf("[%d]\n", pid);
            last_background = pid;
        }
        return 0;
    }
    int status = 0;
    bool ran_last = false, negated = false;
    for (int p = NODES[a].first; p >= 0 && !stopping(); p = NODES[p].next) {
        if ((NODES[p].op == T_AND && last_status != 0) || (NODES[p].op == T_OR && last_status == 0)) {
            ran_last = false;
            continue;                               /* (&& after a failure, || after success) */
        }
        bool last = NODES[p].next < 0;
        if (!last || NODES[p].negate)
            condition_depth++;
        status = last_status = run_pipeline(p, false);
        if (!last || NODES[p].negate)
            condition_depth--;
        ran_last = last;
        negated = NODES[p].negate;
        if (list_depth == 0)
            reported = last ? last_raw : -1;        /* (&& or || used it) */
    }
    if (opt_e && ran_last && !negated && status != 0 && condition_depth == 0 && !stopping())
        exit_requested = status;
    return status;
}

static int run_list(int l)
{
    int status = 0;
    for (int a = NODES[l].first; a >= 0 && !stopping(); a = NODES[a].next)
        status = run_and_or(a);
    return status;
}

/* A list nested in a compound command (its endings are not the line's). */
static int run_inner(int l)
{
    list_depth++;
    int r = run_list(l);
    list_depth--;
    return r;
}

static int child(int n, int k)
{
    int c = NODES[n].first;
    while (c >= 0 && k-- > 0)
        c = NODES[c].next;
    return c;
}

/* After a loop's body: break and continue (n levels). true: leave the loop. */
static bool loop_leave(void)
{
    if (breaking) {
        breaking--;
        return true;
    }
    if (continuing) {
        if (--continuing > 0)
            return true;                            /* (continue an outer loop) */
    }
    return exit_requested >= 0 || returning;
}

/* Run a compound command here, with its redirections as the shell's
 * standard input and output meanwhile. */
static int run_compound(int n)
{
    int fds[3] = { ctx[0], ctx[1], ctx[2] }, opened[8], open_count = 0;
    if (NODES[n].kind != N_FUNC &&
        redirect(NODES[n].redirs, NODES[n].redirs_end, fds, opened, &open_count) < 0) {
        close_all(opened, open_count);
        return 1;
    }
    int saved[3] = { ctx[0], ctx[1], ctx[2] };
    for (int k = 0; k < 3; k++)
        ctx[k] = fds[k];
    int status = 0;
    switch (NODES[n].kind) {
    case N_FUNC: {
        int body = NODES[n].first;
        size_t length;
        const char *text = node_text(body, &length);
        const char *name = TOKENS[NODES[n].word].raw;
        int f = function_index(name);
        if (f < 0 && functions == FUNCTIONS_MAX) {
            dprintf(STDERR_FILENO, "sh: %s: too many functions\n", name);
            status = 1;
            break;
        }
        if (f < 0) {
            f = functions++;
            function_names[f] = strdup(name);
        } else {
            free(function_bodies[f]);
        }
        function_bodies[f] = strndup(text, length);
        break;
    }
    case N_GROUP:
        status = run_inner(NODES[n].first);
        break;
    case N_IF: {
        int c = NODES[n].first;
        status = 0;
        while (c >= 0) {
            int then = NODES[c].next;
            if (then < 0) {                         /* else */
                status = run_inner(c);
                break;
            }
            condition_depth++;
            int r = run_inner(c);
            condition_depth--;
            if (stopping())
                break;
            if (r == 0) {
                status = run_inner(then);
                break;
            }
            c = NODES[then].next;
        }
        break;
    }
    case N_WHILE:
    case N_UNTIL:
        loop_depth++;
        for (;;) {
            condition_depth++;
            int r = run_inner(child(n, 0));
            condition_depth--;
            if (stopping() && !(breaking || continuing))
                break;
            if (breaking || continuing) {
                if (loop_leave())
                    break;
                continue;
            }
            if ((r == 0) != (NODES[n].kind == N_WHILE))
                break;
            status = run_inner(child(n, 1));
            if ((breaking || continuing || stopping()) && loop_leave())
                break;
        }
        loop_depth--;
        break;
    case N_FOR: {
        /* the words first (copies: the body's commands reuse words_text) */
        char **list;
        int count = 0;
        if (NODES[n].list < 0) {
            list = malloc((size_t)(param_count + 1) * sizeof(char *));
            for (int i = 0; list && i < param_count; i++)
                list[count++] = strdup(params[i]);
        } else {
            /* (static: copied before the body runs; on the stack they made
             * every level of run_inner 36 KiB) */
            static struct field got[ARGS_MAX + 1];
            static char *words[ARGS_MAX + 1];
            static bool quoted[ARGS_MAX + 1];
            words_end = words_text;
            for (int i = NODES[n].list; i < NODES[n].list_end; i++) {
                pattern_escapes = ESCAPE_GLOB;      /* (68h: quoted parts marked) */
                int k = expand_word(&TOKENS[i], got, ARGS_MAX - count, true);
                pattern_escapes = ESCAPE_NONE;
                if (k < 0) {
                    count = -1;
                    break;
                }
                for (int j = 0; j < k; j++) {
                    quoted[count] = !glob_ready(&got[j]);
                    words[count++] = got[j].text;
                }
            }
            if (count >= 0)
                count = expand(words, quoted, count);
            list = count >= 0 ? malloc((size_t)(count + 1) * sizeof(char *)) : 0;
            for (int i = 0; list && i < count; i++)
                list[i] = strdup(words[i]);
        }
        if (!list || count < 0) {
            free(list);
            status = 1;
            break;
        }
        loop_depth++;
        const char *name = TOKENS[NODES[n].word].raw;
        for (int i = 0; i < count; i++) {
            set_variable(name, list[i]);
            status = run_inner(NODES[n].first);
            if ((breaking || continuing || stopping()) && loop_leave())
                break;
        }
        loop_depth--;
        for (int i = 0; i < count; i++)
            free(list[i]);
        free(list);
        break;
    }
    case N_CASE: {
        static struct field got[2];
        words_end = words_text;
        int k = expand_word(&TOKENS[NODES[n].word], got, 1, false);
        if (k < 0) {
            status = 1;
            break;
        }
        char *word = strdup(k ? got[0].text : "");
        status = 0;
        bool matched = false;
        for (int item = NODES[n].first; item >= 0 && !matched && word; item = NODES[item].next) {
            for (int i = NODES[item].list; i < NODES[item].list_end && !matched; i++) {
                if (TOKENS[i].kind != T_WORD)
                    continue;
                words_end = words_text;
                pattern_escapes = ESCAPE_CASE;      /* (68g: its quoted parts as they are) */
                int made = expand_word(&TOKENS[i], got, 1, false);
                pattern_escapes = ESCAPE_NONE;
                if (made < 0)
                    break;
                matched = match(made ? got[0].text : "", word);
            }
            if (matched && NODES[item].first >= 0)
                status = run_inner(NODES[item].first);
        }
        free(word);
        break;
    }
    }
    for (int k = 0; k < 3; k++)
        ctx[k] = saved[k];
    close_all(opened, open_count);
    return status;
}

/* Parse and run a text in this shell (eval, ., a function's body). Its
 * exit code. */
static int run_text(const char *text, size_t length)
{
    struct program *p = calloc(1, sizeof(*p));
    if (!p)
        return 1;
    p->source = strndup(text, length);
    int l = p->source ? parse_program(p) : -1;
    int status = 1;
    if (l == INCOMPLETE)
        dprintf(STDERR_FILENO, "sh: the text ends early (a quote or a construct not closed)\n");
    if (l >= 0) {
        struct program *saved = prog;
        prog = p;
        list_depth++;
        status = run_list(l);
        list_depth--;
        prog = saved;
    }
    program_free(p);
    return status;
}

/* Run a parsed line or script (p, its list l) as the shell's own: -1, or
 * the exit code of an `exit`; how its last pipeline ended is reported at
 * the prompt. */
static int run_program(struct program *p, int l)
{
    exit_requested = -1;
    struct program *saved = prog;
    prog = p;
    reported = -1;
    last_status = run_list(l);
    prog = saved;
    breaking = continuing = 0;
    returning = false;
    if (exit_requested >= 0)
        return exit_requested;
    if (interactive && reported >= 0)
        report(last_name, reported);
    return -1;
}

/* Parse and run a whole text as the shell's own (sh -c, a script, a line
 * from a pipe): -1, or an `exit`'s code. */
static int execute_text(const char *text)
{
    struct program *p = calloc(1, sizeof(*p));
    if (!p)
        return -1;
    p->source = strdup(text);
    int l = p->source ? parse_program(p) : -1;
    int r = -1;
    if (l == INCOMPLETE) {
        dprintf(STDERR_FILENO, "sh: the text ends early (a quote or a construct not closed)\n");
        last_status = 2;
    } else if (l < 0) {
        last_status = 2;                            /* (a syntax error, as POSIX shells) */
    } else {
        r = run_program(p, l);
    }
    program_free(p);
    return r;
}

/* --- the line editor ------------------------------------------------------ */

static char history[HISTORY][LINE_MAX];
static int history_count;               /* the newest is history[history_count - 1] */

static void out(const char *s, size_t n)
{
    while (n > 0) {
        ssize_t w = write(STDOUT_FILENO, s, n);
        if (w <= 0)
            return;
        s += w, n -= (size_t)w;
    }
}

static void outs(const char *s)
{
    out(s, strlen(s));
}

/* Characters (screen cells) in bytes from..to of a UTF-8 line. */
static size_t cells(const char *b, size_t from, size_t to)
{
    size_t n = 0;
    for (size_t i = from; i < to; i++)
        n += ((uint8_t)b[i] & 0xC0) != 0x80;
    return n;
}

static void left(size_t n)
{
    char seq[16];
    if (n) {
        snprintf(seq, sizeof(seq), "\033[%uD", (unsigned)n);
        outs(seq);
    }
}

/* The whole line again: back to its start, erased, prompt and text, the
 * cursor at cur. ESC [ 0 K before the prompt marks it as shown again. */
static void redraw(const char *prompt, const char *b, size_t len, size_t cur)
{
    outs("\r\033[0K");
    outs(prompt);
    out(b, len);
    left(cells(b, cur, len));
}

/* Remember a command (not an empty one, nor the last one again). */
static void remember(const char *line)
{
    if (!*line || strlen(line) >= LINE_MAX ||   /* (only lines that fit, never cut ones) */
        (history_count && strcmp(history[history_count - 1], line) == 0))
        return;
    if (history_count == HISTORY) {
        memmove(history[0], history[1], sizeof(history[0]) * (HISTORY - 1));
        history_count--;
    }
    snprintf(history[history_count++], LINE_MAX, "%s", line);
}

/* The next typed byte, from what one raw read gave; -1 at the end, -2 if
 * the read was interrupted. */
static int next_byte(void)
{
    static uint8_t buf[64];
    static ssize_t have, at;
    if (at == have) {
        have = read(STDIN_FILENO, buf, sizeof(buf));
        at = 0;
        if (have <= 0) {
            int r = have < 0 && errno == EINTR ? -2 : -1;
            have = 0;
            return r;
        }
    }
    return buf[at++];
}

/* Read a command line after showing prompt: its length; -1 at the end of
 * input (Ctrl-D on an empty line), -2 if cancelled (Ctrl-C). Without a
 * terminal (or not in the foreground) the terminal's own line editing. */
/* A whole line from a pipe or a file (no terminal): a read there may give
 * part of a line or several (unlike a terminal's, one line each), so lines
 * are collected in `waiting`. One longer than COMMAND_MAX is reported and
 * skipped, never run cut short (a cut `rm` could name another file). */
static ssize_t read_piped(char *b, size_t size)
{
    static char waiting[COMMAND_MAX];
    static size_t have;
    static bool ended, skipping;
    for (;;) {
        char *nl = memchr(waiting, '\n', have);
        if (nl || (ended && have)) {
            size_t n = nl ? (size_t)(nl - waiting) : have;
            bool skip = skipping;
            skipping = false;
            if (!skip && n < size) {
                memcpy(b, waiting, n);
                b[n] = 0;
            }
            have -= nl ? n + 1 : n;
            memmove(waiting, waiting + (nl ? n + 1 : n), have);
            if (skip)
                continue;                       /* (the rest of a line too long) */
            if (n >= size) {
                dprintf(STDERR_FILENO, "sh: a line longer than %zu bytes: not run\n", size - 1);
                continue;
            }
            return (ssize_t)n;
        }
        if (ended)
            return -1;
        if (have == sizeof(waiting)) {          /* no line break in the whole buffer */
            if (!skipping)
                dprintf(STDERR_FILENO, "sh: a line longer than %zu bytes: not run\n",
                        size - 1);
            skipping = true;
            have = 0;
        }
        ssize_t r = read(STDIN_FILENO, waiting + have, sizeof(waiting) - have);
        if (r < 0 && errno == EINTR)
            return -2;
        if (r <= 0)
            ended = true;
        else
            have += (size_t)r;
    }
}

static ssize_t read_command(const char *prompt, char *b, size_t size)
{
    outs(prompt);
    /* (63i) Only a terminal gets the line editor: a shell reading a pipe at
     * the console (cat script | sh) switched the console's into it, then
     * read the pipe's bytes as if typed and echoed them. */
    if (!isatty(STDIN_FILENO) || ritteros_terminal_line_editor() < 0)
        return read_piped(b, size);
    if (size > LINE_MAX)
        size = LINE_MAX;                        /* typed lines: as the history keeps them */
    size_t len = 0, cur = 0;
    int shown = history_count;              /* history_count: the line being typed */
    static char draft[LINE_MAX];            /* that line while Up shows older ones */
    char utf8[4];
    size_t utf8_len = 0, utf8_need = 0;
    int previous = 0;
    ssize_t result;
    b[0] = 0;
    for (;;) {
        int c = next_byte();
        if (c < 0) {
            result = c == -2 ? -2 : len ? (ssize_t)len : -1;
            break;
        }
        int before = previous;
        previous = c;
        if (c == '\n' && before == '\r')
            continue;                           /* CR LF: one Enter */
        if (c >= 0x80) {                        /* UTF-8: the whole character at once */
            if ((c & 0xC0) == 0x80) {
                if (!utf8_need)
                    continue;
                utf8[utf8_len++] = (char)c;
                if (--utf8_need)
                    continue;
            } else {
                utf8_need = (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2
                          : (c & 0xF8) == 0xF0 ? 3 : 0;
                utf8_len = 0;
                if (utf8_need)
                    utf8[utf8_len++] = (char)c;
                continue;
            }
        } else {
            utf8_need = 0;
        }
        if (c == '\r' || c == '\n') {
            out(b + cur, len - cur);            /* the cursor to the end, then the line break */
            outs("\n");
            result = (ssize_t)len;
            break;
        }
        if (c == 0x03) {                        /* Ctrl-C */
            out(b + cur, len - cur);
            result = -2;
            break;
        }
        if (c == 0x04) {                        /* Ctrl-D */
            if (len == 0) {
                result = -1;
                break;
            }
            continue;
        }
        if (c == 0x1B) {                        /* ESC [ (or O) ... final: the special keys */
            int kind = next_byte(), param = 0, final = 0;
            if (kind != '[' && kind != 'O')
                continue;
            for (;;) {
                final = next_byte();
                if (final < 0 || final >= 0x40)
                    break;
                if (final >= '0' && final <= '9')
                    param = param * 10 + (final - '0');
            }
            if (final == 'A' || final == 'B') { /* Up, Down: older, newer commands */
                int to = shown + (final == 'A' ? -1 : 1);
                if (to < 0 || to > history_count)
                    continue;
                if (shown == history_count)
                    snprintf(draft, sizeof(draft), "%.*s", (int)len, b);
                shown = to;
                snprintf(b, size, "%s", shown == history_count ? draft : history[shown]);
                len = cur = strlen(b);
                redraw(prompt, b, len, cur);
            } else if (final == 'D' && cur > 0) {   /* Left */
                do
                    cur--;
                while (cur > 0 && ((uint8_t)b[cur] & 0xC0) == 0x80);
                left(1);
            } else if (final == 'C' && cur < len) { /* Right: the character again */
                size_t from = cur;
                do
                    cur++;
                while (cur < len && ((uint8_t)b[cur] & 0xC0) == 0x80);
                out(b + from, cur - from);
            } else if (final == 'H' || (final == '~' && (param == 1 || param == 7))) {
                left(cells(b, 0, cur));             /* Home */
                cur = 0;
            } else if (final == 'F' || (final == '~' && (param == 4 || param == 8))) {
                out(b + cur, len - cur);            /* End */
                cur = len;
            } else if (final == '~' && param == 3 && cur < len) {   /* Delete */
                size_t end = cur;
                do
                    end++;
                while (end < len && ((uint8_t)b[end] & 0xC0) == 0x80);
                memmove(b + cur, b + end, len - end);
                len -= end - cur;
                out(b + cur, len - cur);
                outs(" ");
                left(cells(b, cur, len) + 1);
            }
            continue;
        }
        if (c == 0x7F || c == '\b') {           /* Backspace */
            if (cur == 0)
                continue;
            size_t start = cur;
            do
                start--;
            while (start > 0 && ((uint8_t)b[start] & 0xC0) == 0x80);
            memmove(b + start, b + cur, len - cur);
            len -= cur - start;
            cur = start;
            if (cur == len) {
                outs("\b \b");                  /* at the end: as the terminal's own */
                continue;
            }
            outs("\b");
            out(b + cur, len - cur);
            outs(" ");
            left(cells(b, cur, len) + 1);
            continue;
        }
        if (c == 0x15) {                        /* Ctrl-U: the whole line away */
            out(b + cur, len - cur);
            for (size_t i = cells(b, 0, len); i > 0; i--)
                outs("\b \b");
            len = cur = 0;
            continue;
        }
        if (c == 0x0C) {                        /* Ctrl-L: the line at the top */
            outs("\033[2J\033[H");                /* (as the terminal's own Ctrl-L) */
            outs(prompt);
            out(b, len);
            left(cells(b, cur, len));
            continue;
        }
        if (c < 0x20)
            continue;                           /* other control keys: nothing */
        /* A character (ASCII, or a whole UTF-8 one) at the cursor. */
        char one = (char)c;
        const char *add = c >= 0x80 ? utf8 : &one;
        size_t n = c >= 0x80 ? utf8_len : 1;
        if (len + n + 1 > size)
            continue;
        memmove(b + cur + n, b + cur, len - cur);
        memcpy(b + cur, add, n);
        len += n;
        out(b + cur, len - cur);                /* it and what follows */
        cur += n;
        left(cells(b, cur, len));
    }
    b[len] = 0;
    ritteros_terminal_raw(false);
    return result;
}

/* A login shell's ~/.config/login: each line run as if typed (blank lines
 * and # comments skipped). -1 when the shell goes on to its prompt, else
 * the exit code of an `exit` in the file. */
static int run_login_file(void)
{
    struct passwd *pw = getpwuid(getuid());
    if (!pw)
        return -1;
    char path[300];
    snprintf(path, sizeof(path), "%s/.config/login", pw->pw_dir);
    size_t length;
    char *text = read_file(path, &length);
    if (!text)
        return -1;                              /* (none: nothing to run) */
    int code = execute_text(text);              /* (51c: as a script; # comments) */
    free(text);
    return code;
}

/* (51c) Read a command typed at the prompt (or a line from a pipe), and
 * more lines while it is not complete (an if without its fi, an open
 * quote, a | at the end), with the prompt "> ". Its program (parsed) and
 * list, or 0: Ctrl-C (*interrupted) or the end of the input. */
static struct program *read_program(const char *prompt, int *list, bool *interrupted)
{
    static char line[COMMAND_MAX];
    *interrupted = false;
    ssize_t len = read_command(prompt, line, sizeof(line));
    if (len == -2) {
        *interrupted = true;
        return 0;
    }
    if (len < 0)
        return 0;
    remember(line);
    size_t size = (size_t)len + 1;
    char *text = malloc(size);
    if (!text)
        return 0;
    memcpy(text, line, (size_t)len + 1);
    for (;;) {
        struct program *p = calloc(1, sizeof(*p));
        if (!p) {
            free(text);
            return 0;
        }
        p->source = strdup(text);
        *list = p->source ? parse_program(p) : -1;
        if (*list != INCOMPLETE) {
            free(text);
            return p;                           /* (a syntax error: list -1, a message shown) */
        }
        program_free(p);
        /* (63i) Inside a here-document only its delimiter's line can end it:
         * lines are gathered up to that one, then the text is parsed again
         * (not at every line: a long body would take time squared). */
        char delim[256];
        snprintf(delim, sizeof(delim), "%s", heredoc_waiting);
        bool tabs = heredoc_waiting_tabs;
        for (;;) {
            len = read_command(interactive ? "> " : "", line, sizeof(line));
            if (len < 0) {
                if (len == -2)
                    *interrupted = true;
                else
                    dprintf(STDERR_FILENO, delim[0] ? "sh: the input ended inside a here-document "
                                                      "(no line '%s')\n"
                                                    : "sh: the input ended inside a command\n", delim);
                free(text);
                return 0;
            }
            remember(line);
            char *longer = realloc(text, size + (size_t)len + 1);
            if (!longer) {
                free(text);
                return 0;
            }
            text = longer;
            text[size - 1] = '\n';              /* (the line break between them) */
            memcpy(text + size, line, (size_t)len + 1);
            size += (size_t)len + 1;
            const char *l = line;
            while (tabs && *l == '\t')
                l++;
            if (!delim[0] || !strcmp(l, delim))
                break;
        }
    }
}

int main(int argc, char **argv)
{
    /* Programs live next to the shell: /system/<arch>/bin. */
    size_t n = 0;
    for (size_t i = 0; argv[0][i]; i++)
        if (argv[0][i] == '/')
            n = i;
    if (n == 0)
        snprintf(bin_dir, sizeof(bin_dir), "/system/%s/bin",
#if defined(__x86_64__)
                 "x86_64"
#else
                 "aarch64"
#endif
        );
    else
        snprintf(bin_dir, sizeof(bin_dir), "%.*s", (int)n, argv[0]);
    /* Windowed apps live in the sibling apps directory. */
    snprintf(apps_dir, sizeof(apps_dir), "%s", bin_dir);
    char *tail = strrchr(apps_dir, '/');
    if (tail)
        snprintf(tail + 1, sizeof(apps_dir) - (size_t)(tail + 1 - apps_dir), "apps");
    shell_group = getpgrp();
    shell_pid = getpid();
    signal(SIGINT, SIG_IGN);
    param0 = argv[0];
    if (argc >= 3 && strcmp(argv[1], "-c") == 0) {
        if (argc > 3) {                         /* (51b) sh -c TEXT NAME ARGS: $0, $1... */
            param0 = argv[3];
            params = argv + 4;
            param_count = argc - 4;
        }
        int code = execute_text(argv[2]);       /* (all of it, as one program) */
        return code >= 0 ? code : last_status;
    }
    if (argc >= 2 && argv[1][0] != '-') {       /* (51c) sh FILE ARGS: a script */
        size_t length;
        char *text = read_file(argv[1], &length);
        if (!text) {
            dprintf(STDERR_FILENO, "sh: %s: %s\n", argv[1], strerror(errno));
            return 127;
        }
        param0 = argv[1];
        params = argv + 2;
        param_count = argc - 2;
        int code = execute_text(text);
        return code >= 0 ? code : last_status;
    }

    interactive = isatty(STDIN_FILENO);         /* (51a: then lines report how they ended) */
    if (argc == 2 && strcmp(argv[1], "-l") == 0) {
        int code = run_login_file();
        if (code >= 0)
            return code;                        /* (an `exit` in it) */
    }

    char cwd[256], prompt[300];
    for (;;) {
        reap_background();
        /* (63c: a prompt only for a terminal: a script piped in, as ssh
         * HOST sh < FILE, gets none, as from any POSIX shell) */
        if (interactive)
            snprintf(prompt, sizeof(prompt), "ritteros:%s$ ", getcwd(cwd, sizeof(cwd)) ? cwd : "?");
        else
            prompt[0] = 0;
        int list;
        bool interrupted;
        struct program *p = read_program(prompt, &list, &interrupted);
        if (!p && interrupted) {
            printf("^C\n");                     /* Ctrl-C: the line is dropped */
            continue;
        }
        if (!p) {
            if (interactive)
                printf("exit\n");               /* Ctrl-D (a piped script's end: nothing) */
            return 0;
        }
        int code = -1;
        if (list >= 0)
            code = run_program(p, list);
        else
            last_status = 2;                    /* (a syntax error, as POSIX shells) */
        program_free(p);
        if (code >= 0)
            return code;
    }
}
