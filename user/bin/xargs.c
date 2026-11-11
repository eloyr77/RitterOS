/*
 * xargs [-t] [-n N | -L N | -I REPL] [COMMAND [ARG]...] (51e, POSIX): runs
 * COMMAND (echo if none) with the words of the input added to its
 * arguments, as many at a time as fit (the kernel takes 128 words and
 * 4096 bytes), or -n N at a time, or the words of -L N lines; -I REPL runs
 * it once per input line with REPL in the arguments replaced by the line.
 * Words are separated by blanks and line breaks, quoted with ' or " or a
 * backslash. -t prints each command on the error output first. Exit code:
 * 0, 123 if a command ended with 1-125, 124 with 255, 125 for a signal,
 * 126 or 127 if it could not be started.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include "ritteros.h"

PROGRAM_HELP("xargs [-t] [-n N | -L N | -I REPL] [COMMAND [ARG]...]",
             "run a command with the input's words as arguments");

#if defined(__x86_64__)
#define SYSTEM_DIR "/system/x86_64"
#else
#define SYSTEM_DIR "/system/aarch64"
#endif

#define WORDS_MAX 120                   /* (some room below the kernel's 128) */
#define BYTES_MAX 3800                  /* (and below its 4096 bytes) */

static bool trace;
static int status;
static char path[256];

/* (52g) Not a binary (no ELF header, no NUL in its first 512 bytes): a
 * file the kernel refuses (ENOEXEC) runs as a shell script, as POSIX's
 * execvp does. */
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

/* spawn, and for a script without #! the shell with it: sh PATH ARGS. */
static pid_t spawn_or_script(const char *path, char **argv, int flags, const int fds[3])
{
    pid_t pid = ritteros_spawn(path, argv, flags, fds);
    if (pid >= 0 || errno != ENOEXEC || !is_script(path))
        return pid;
    int count = 0;
    while (argv[count])
        count++;
    char **again = malloc((size_t)(count + 2) * sizeof(char *));
    if (!again)
        return -1;
    again[0] = "sh";
    again[1] = (char *)path;
    for (int i = 1; i <= count; i++)
        again[i + 1] = argv[i];
    pid = ritteros_spawn(SYSTEM_DIR "/bin/sh", again, flags, fds);
    free(again);
    return pid;
}

/* Runs argv; false if it could not be started or ended with 255 (stop). */
static bool run(char **argv)
{
    if (trace) {
        for (int k = 0; argv[k]; k++)
            fprintf(stderr, "%s%s", k ? " " : "", argv[k]);
        fprintf(stderr, "\n");
    }
    int fds[3] = { -1, -1, -1 };
    pid_t pid = spawn_or_script(path, argv, 0, fds);
    if (pid < 0) {
        fprintf(stderr, "xargs: %s: %s\n", argv[0], errno == ENOENT ? "command not found" : strerror(errno));
        status = errno == ENOENT ? 127 : 126;
        return false;
    }
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
        ;
    if (!WIFEXITED(st)) {
        status = 125;
        return false;
    }
    int code = WEXITSTATUS(st);
    if (code == 255) {
        fprintf(stderr, "xargs: %s exited with 255\n", argv[0]);
        status = 124;
        return false;
    }
    if (code)
        status = 123;
    return true;
}

static int next_char(void)
{
    static unsigned char buf[4096];
    static ssize_t have, at;
    if (at == have) {
        have = read(STDIN_FILENO, buf, sizeof(buf));
        at = 0;
        if (have <= 0)
            return EOF;
    }
    return buf[at++];
}

/* The next word into w (size n); *line_end is set when a line break ended
 * it. 0 at the end of the input, -1 for an unmatched quote. */
static int next_word(char *w, size_t n, bool *line_end, bool whole_line)
{
    int c;
    size_t len = 0;
    *line_end = false;
    do
        c = next_char();
    while (c == ' ' || c == '\t' || (!whole_line && c == '\n'));
    if (c == EOF)
        return 0;
    char quote = 0;
    for (; c != EOF; c = next_char()) {
        if (quote) {
            if (c == quote) {
                quote = 0;
                continue;
            }
            if (c == '\n')
                return -1;
        } else if (c == '\n') {
            *line_end = true;
            break;
        } else if (!whole_line && (c == ' ' || c == '\t')) {
            break;
        } else if (!whole_line && (c == '\'' || c == '"')) {
            quote = (char)c;
            continue;
        } else if (!whole_line && c == '\\') {
            c = next_char();
            if (c == EOF)
                break;
        }
        if (len + 1 < n)
            w[len++] = (char)c;
    }
    if (quote)
        return -1;
    while (whole_line && len > 0 && (w[len - 1] == ' ' || w[len - 1] == '\t'))
        len--;                                  /* (-I: blanks at both ends go, as BSD's) */
    w[len] = 0;
    return 1;
}

/* s with every repl replaced by line, malloc'd. */
static char *replace(const char *s, const char *repl, const char *line)
{
    size_t rl = strlen(repl), ll = strlen(line), n = 0;
    for (const char *p = s; (p = strstr(p, repl)); p += rl)
        n++;
    char *out = malloc(strlen(s) + n * ll + 1), *o = out;
    if (!out)
        return 0;
    for (const char *p = s, *q; ; p = q + rl) {
        q = strstr(p, repl);
        size_t k = q ? (size_t)(q - p) : strlen(p);
        memcpy(o, p, k);
        o += k;
        if (!q)
            break;
        memcpy(o, line, ll);
        o += ll;
    }
    *o = 0;
    return out;
}

int main(int argc, char **argv)
{
    long per = 0, lines = 0;
    const char *repl = 0;
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "--")) {
            i++;
            break;
        }
        if (!strcmp(argv[i], "-t")) {
            trace = true;
            continue;
        }
        char opt = argv[i][1];
        const char *v = argv[i][2] ? argv[i] + 2 : i + 1 < argc ? argv[++i] : 0;
        if (!v || (opt != 'n' && opt != 'L' && opt != 'I'))
            goto usage;
        if (opt == 'I')
            repl = v, per = 0, lines = 0;
        else if ((opt == 'n' ? (per = atol(v)) : (lines = atol(v))) <= 0)
            goto usage;
    }
    char *echo[] = { "echo", 0 };
    char **command = i < argc ? argv + i : echo;
    int fixed = i < argc ? argc - i : 1;
    if (strchr(command[0], '/')) {
        snprintf(path, sizeof(path), "%s", command[0]);
    } else {
        struct stat st;
        snprintf(path, sizeof(path), SYSTEM_DIR "/bin/%s", command[0]);
        if (stat(path, &st) < 0)
            snprintf(path, sizeof(path), SYSTEM_DIR "/apps/%s.program/%s", command[0], command[0]);   /* (42b) */
    }
    size_t fixed_bytes = 0;
    for (int k = 0; k < fixed; k++)
        fixed_bytes += strlen(command[k]) + 1;
    static char word[BYTES_MAX];
    static char *args[WORDS_MAX + 2];
    static char store[BYTES_MAX];
    bool line_end;
    int got;
    if (repl) {
        while ((got = next_word(word, sizeof(word), &line_end, true)) > 0) {
            char *a[WORDS_MAX + 1];
            int n = 0;
            for (int k = 0; k < fixed && n < WORDS_MAX; k++)
                a[n++] = replace(command[k], repl, word);
            a[n] = 0;
            bool go_on = run(a);
            for (int k = 0; k < n; k++)
                free(a[k]);
            if (!go_on)
                return status;
        }
        return status;
    }
    /* A word that does not fit begins the next command (pending). */
    bool pending = false, end = false;
    while (!end) {
        int n = 0;
        size_t used = 0;
        for (int k = 0; k < fixed; k++)
            args[n++] = command[k];
        long line_count = 0;
        while (n < WORDS_MAX && (!per || n - fixed < per)) {
            if (!pending) {
                got = next_word(word, sizeof(word), &line_end, false);
                if (got < 0) {
                    fprintf(stderr, "xargs: unmatched quote\n");
                    return 1;
                }
                if (got == 0) {
                    end = true;
                    break;
                }
            }
            size_t len = strlen(word) + 1;
            if (fixed_bytes + used + len > BYTES_MAX) {
                if (n == fixed) {
                    fprintf(stderr, "xargs: argument too long\n");
                    return 1;
                }
                pending = true;
                break;
            }
            pending = false;
            args[n++] = memcpy(store + used, word, len);
            used += len;
            if (lines && line_end && ++line_count == lines)
                break;
        }
        args[n] = 0;
        if (n > fixed && !run(args))            /* (no input: nothing runs, as BSD's) */
            return status;
    }
    return status;
usage:
    fprintf(stderr, "usage: xargs [-t] [-n N | -L N | -I REPL] [COMMAND [ARG]...]\n");
    return 1;
}
