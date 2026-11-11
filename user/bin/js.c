/*
 * js [FILE [ARG...]] | js -e CODE (27o, roadmap item 27): runs JavaScript
 * with RitterOS's own engine (user/lib/js/), as Mozilla's and Apple's
 * shells do: a file, the text after -e, or, with neither, a prompt `js> `
 * that runs each line and prints its value. The script's arguments are the
 * global array `arguments` (as JavaScriptCore's jsc gives them); print()
 * and console.log() write a line to the standard output.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ritteros.h"
#include "../lib/js/js.h"

PROGRAM_HELP("js [FILE [ARG...]] | js -e CODE [ARG...]",
             "run JavaScript (a prompt without a file)");

static char *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    size_t cap = 65536, n = 0;
    char *buf = malloc(cap);
    for (;;) {
        if (!buf)
            break;
        size_t got = fread(buf + n, 1, cap - n, f);
        n += got;
        if (n < cap)
            break;
        cap *= 2;
        buf = realloc(buf, cap);
    }
    fclose(f);
    if (!buf)
        return 0;
    *len = n;
    return buf;
}

static int report(struct js *J)
{
    jsval e = js_take_exception(J);
    char *t = js_exception_text(J, e);
    fprintf(stderr, "js: uncaught %s\n", t);
    free(t);
    return 3;
}

/* (27r) The jobs the script queued (Promise reactions, queueMicrotask's):
 * run until none is left, as JavaScriptCore's shell does after a script. */
static int run_jobs(struct js *J)
{
    int rc = 0;
    while (js_run_jobs(J) == JS_EXCEPTION)
        rc = report(J);
    return rc;
}

static int run_text(struct js *J, const char *text, size_t len, const char *file)
{
    jsval r = js_eval(J, text, len, file);
    if (r == JS_EXCEPTION)
        return report(J);
    return run_jobs(J);
}

static void prompt_loop(struct js *J)
{
    char line[4096];
    for (;;) {
        fputs("js> ", stdout);
        fflush(stdout);
        if (!fgets(line, sizeof line, stdin))
            break;
        size_t n = strlen(line);
        jsval r = js_eval(J, line, n, "(prompt)");
        if (r == JS_EXCEPTION) {
            report(J);
            continue;
        }
        run_jobs(J);
        if (js_is_undefined(r))
            continue;
        size_t len;
        char *t = js_to_utf8(J, r, &len);
        if (!t) {
            report(J);
            continue;
        }
        fwrite(t, 1, len, stdout);
        fputc('\n', stdout);
        free(t);
    }
    fputc('\n', stdout);
}

int main(int argc, char **argv)
{
    struct js *J = js_new();
    if (!J) {
        fprintf(stderr, "js: out of memory\n");
        return 1;
    }
    js_set_stack_base(J, &J);
    int first_arg = argc;
    const char *code = 0, *file = 0;
    if (argc >= 3 && !strcmp(argv[1], "-e")) {
        code = argv[2];
        first_arg = 3;
    } else if (argc >= 2) {
        if (argv[1][0] == '-' && argv[1][1]) {
            fprintf(stderr, "usage: js [FILE [ARG...]] | js -e CODE\n");
            return 2;
        }
        file = argv[1];
        first_arg = 2;
    }
    jsval args = js_new_array(J, 0);
    for (int i = first_arg; i < argc; i++)
        js_set_index(J, args, (uint32_t)(i - first_arg), js_cstring(J, argv[i]));
    js_define_value(J, js_global(J), "arguments", args);
    int status = 0;
    if (code)
        status = run_text(J, code, strlen(code), "-e");
    else if (file) {
        size_t len;
        char *text = read_file(file, &len);
        if (!text) {
            fprintf(stderr, "js: cannot read %s\n", file);
            return 1;
        }
        status = run_text(J, text, len, file);
        free(text);
    } else
        prompt_loop(J);
    js_free(J);
    return status;
}
