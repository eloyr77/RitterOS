/*
 * The driver (60g): cc's options, and the steps of a build as on Unix --
 * each C file compiled to assembly, assembled by `as`, and everything
 * linked by `ld` with the start code and the C library. A C file is
 * compiled in a process of its own (cc runs itself with -S): the parser's
 * tables are for one translation unit.
 *
 * cc [-c|-S|-E] [-o FILE] [-I DIR] [-isystem DIR] [-D NAME[=VALUE]] [-U NAME]
 *    [-L DIR] [-lNAME] [-B DIR] [-target ARCH] [-v] [-MD|-MMD] [-MP] [-MF FILE] [-MT TARGET] FILE...
 *
 * -MD and -MMD (63g, clang's and GCC's) also write a make rule of what each
 * object was built from (OBJECT: SOURCE HEADERS..., -MMD without the
 * system's headers) beside the object as OBJECT.d, or in -MF's file; -MP
 * adds an empty rule for each header (so a deleted one does not stop make),
 * -MT names the target.
 */
#include <errno.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include "cc/ir.h"

/* (built on the Mac too, for the tests: there without RitterOS's header) */
#if __has_include("ritteros.h")
#include "ritteros.h"
PROGRAM_HELP("cc [-c|-S|-E] [-o FILE] [OPTION]... FILE...",
             "compile C programs");
#endif

struct target target;

/* Where a RitterOS disk keeps what cc needs (CC_ROOT: the system folder). */
#ifndef CC_ROOT
#define CC_ROOT "/system"
#endif

static bool verbose;
static int dep_mode;                            /* (63g) 0, 1 -MMD, 2 -MD */
static bool dep_phony;                          /* -MP */
static const char *dep_file, *dep_target;       /* -MF, -MT */
static bool assembly;                           /* (62e) -asm: a .S file, preprocessed for as */
static const char *tools_dir;                   /* -B: as, ld (and cc itself) */
static const char *self;

static void set_target(const char *name)
{
    /* (62d) a triple (x86_64-unknown-none-elf) by its first part, as clang */
    if (strncmp(name, "x86_64-", 7) == 0)
        name = "x86_64";
    else if (strncmp(name, "aarch64-", 8) == 0)
        name = "aarch64";
    if (strcmp(name, "x86_64") == 0)
        target = (struct target){ ARCH_X86_64, "x86_64", true, 16, 16 };
    else if (strcmp(name, "aarch64") == 0)
        target = (struct target){ ARCH_AARCH64, "aarch64", false, 16, 16 };
    else
        fatal("unknown target '%s' (x86_64 or aarch64)", name);
}

static void usage(void)
{
    fprintf(stderr, "usage: cc [-c|-S|-E] [-o FILE] [-I DIR] [-D NAME[=VALUE]] [-U NAME] [-L DIR] [-lNAME] FILE...\n");
    exit(2);
}

/* --- running the other programs ------------------------------------------ */

extern char **environ;

static int run(char **argv)
{
    if (verbose) {
        for (char **a = argv; *a; a++)
            fprintf(stderr, "%s%s", a == argv ? "" : " ", *a);
        fputc('\n', stderr);
    }
    pid_t pid;
    int e = posix_spawn(&pid, argv[0], 0, 0, argv, environ);
    if (e)
        fatal("cannot run %s: %s", argv[0], strerror(e));
    int status;
    while (waitpid(pid, &status, 0) < 0)
        if (errno != EINTR)
            fatal("waiting for %s: %s", argv[0], strerror(errno));
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    return 1;
}

static char *tool(const char *name)
{
    if (tools_dir)
        return format("%s/%s", tools_dir, name);
    return format(CC_ROOT "/%s/bin/%s", target.name, name);
}

static char **temps;
static int ntemps;

static void remove_temps(void)
{
    for (int i = 0; i < ntemps; i++)
        unlink(temps[i]);
}

static char *temp_file(const char *suffix)
{
    static int count;
    char *p = format("/tmp/cc-%d-%d%s", (int)getpid(), count++, suffix);
    temps = realloc(temps, sizeof(char *) * (size_t)(ntemps + 1));
    temps[ntemps++] = p;
    return p;
}

static const char *suffix(const char *path)
{
    const char *dot = strrchr(path, '.'), *slash = strrchr(path, '/');
    return dot && (!slash || dot > slash) ? dot : "";
}

static char *replace_suffix(const char *path, const char *new_suffix)
{
    const char *slash = strrchr(path, '/');
    const char *base = slash ? slash + 1 : path;
    const char *dot = strrchr(base, '.');
    int n = dot ? (int)(dot - base) : (int)strlen(base);
    return format("%.*s%s", n, base, new_suffix);
}

/* path with its suffix replaced, in its own folder (an object's .d file). */
static char *beside(const char *path, const char *new_suffix)
{
    const char *dot = strrchr(path, '.'), *slash = strrchr(path, '/');
    int n = dot && (!slash || dot > slash) ? (int)(dot - path) : (int)strlen(path);
    return format("%.*s%s", n, path, new_suffix);
}

/* (63g) The make rule of what input was built from. */
static void write_dependencies(const char *input)
{
    FILE *f = fopen(dep_file, "w");
    if (!f)
        fatal("cannot write '%s'", dep_file);
    const char *deps = cpp_dependencies.data ? cpp_dependencies.data : "";
    fprintf(f, "%s: %s%.*s\n", dep_target, input, (int)cpp_dependencies.len, deps);
    if (dep_phony)                              /* each header a target of its own, with nothing */
        for (size_t i = 0; i < cpp_dependencies.len;) {
            while (i < cpp_dependencies.len && deps[i] == ' ')
                i++;
            size_t start = i;
            while (i < cpp_dependencies.len && deps[i] != ' ')
                i++;
            if (i > start)
                fprintf(f, "\n%.*s:\n", (int)(i - start), deps + start);
        }
    if (fclose(f) != 0)
        fatal("cannot write '%s'", dep_file);
}

/* --- compiling one file (in this process) -------------------------------- */

static int compile(const char *input, const char *output, bool preprocess_only, bool syntax_only)
{
    types_init();
    struct source *src = source_read(input);
    if (!src)
        fatal("cannot read '%s'", input);
    struct token *t = preprocess(tokenize(src));
    FILE *out = output && strcmp(output, "-") ? fopen(output, "w") : stdout;
    if (!out)
        fatal("cannot write '%s'", output);
    if (dep_mode && dep_file && dep_target)
        write_dependencies(input);              /* (everything is included by now) */
    if (preprocess_only) {
        print_preprocessed(out, t, assembly);
        return out != stdout && fclose(out) != 0;
    }
    t = convert_pp_tokens(t);
    struct obj *program = parse(t);
#ifdef CC_TESTING
    extern void dump_layouts(FILE * out);
    if (getenv("CC_DUMP_LAYOUT"))
        dump_layouts(out);
#endif
    if (syntax_only)
        return 0;
    if (target.arch == ARCH_X86_64)
        emit_x86_64(out, program);
    else
        emit_aarch64(out, program);
    if (out != stdout && fclose(out) != 0)
        fatal("cannot write '%s'", output);
    return 0;
}

/* --- the driver ---------------------------------------------------------- */

int main(int argc, char **argv)
{
#if defined(__aarch64__) || defined(__arm64__)
    set_target("aarch64");
#else
    set_target("x86_64");
#endif
    self = argv[0];
    const char *output = 0;
    enum { LINK, OBJECT, ASSEMBLY, PREPROCESS, SYNTAX } mode = LINK;
    bool std_includes = true, std_libs = true;
    char **inputs = calloc((size_t)argc + 1, sizeof(char *));
    int ninputs = 0;
    /* options for the compilations (passed on to cc -S) and for ld */
    char **cpp_opts = calloc((size_t)argc * 2 + 1, sizeof(char *));
    int ncpp = 0;
    char **ld_opts = calloc((size_t)argc * 2 + 1, sizeof(char *));
    int nld = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *next = i + 1 < argc ? argv[i + 1] : 0;
        if (strcmp(a, "-c") == 0)
            mode = OBJECT;
        else if (strcmp(a, "-S") == 0)
            mode = ASSEMBLY;
        else if (strcmp(a, "-E") == 0)
            mode = PREPROCESS;
        else if (strcmp(a, "-fsyntax-only") == 0)
            mode = SYNTAX;
        else if (strcmp(a, "-o") == 0 && next)
            output = argv[++i];
        else if (strcmp(a, "-target") == 0 && next) {
            set_target(argv[++i]);
            cpp_opts[ncpp++] = "-target";
            cpp_opts[ncpp++] = argv[i];
        } else if (strcmp(a, "-v") == 0)
            verbose = true;
        else if (strcmp(a, "-MD") == 0 || strcmp(a, "-MMD") == 0)
            dep_mode = a[2] == 'M' ? 1 : 2;
        else if (strcmp(a, "-MP") == 0)
            dep_phony = true;
        else if ((strcmp(a, "-MF") == 0 || strcmp(a, "-MT") == 0 || strcmp(a, "-MQ") == 0) && next) {
            if (a[2] == 'F')
                dep_file = argv[++i];
            else
                dep_target = argv[++i];
        }
        else if (strcmp(a, "-nostdinc") == 0) {
            std_includes = false;
            cpp_opts[ncpp++] = (char *)a;
        } else if (strcmp(a, "-nostdlib") == 0)
            std_libs = false;
        else if (strncmp(a, "-B", 2) == 0) {
            tools_dir = a[2] ? a + 2 : (next ? argv[++i] : 0);
            if (!tools_dir)
                usage();
        } else if ((a[1] == 'I' || a[1] == 'D' || a[1] == 'U') && a[0] == '-') {
            const char *arg = a[2] ? a + 2 : next;
            if (!arg)
                usage();
            if (!a[2])
                i++;
            cpp_opts[ncpp++] = (char *)format("-%c%s", a[1], arg);
        } else if (strcmp(a, "-isystem") == 0 && next) {
            cpp_opts[ncpp++] = "-isystem";
            cpp_opts[ncpp++] = argv[++i];
        } else if (strncmp(a, "-L", 2) == 0) {
            const char *arg = a[2] ? a + 2 : next;
            if (!arg)
                usage();
            if (!a[2])
                i++;
            ld_opts[nld++] = format("-L%s", arg);
        } else if (strncmp(a, "-l", 2) == 0 && a[2]) {
            inputs[ninputs++] = (char *)a;      /* (in order with the files) */
        } else if (strcmp(a, "-O0") == 0) {
            opt_level = 0;                      /* (every value on the stack, as before 61) */
            cpp_opts[ncpp++] = (char *)a;
        } else if (strcmp(a, "-fno-pic") == 0 || strcmp(a, "-fno-pie") == 0 || strcmp(a, "-fno-PIC") == 0 ||
                   strcmp(a, "-fno-PIE") == 0 || strcmp(a, "-fPIC") == 0 || strcmp(a, "-fpic") == 0 ||
                   strcmp(a, "-fPIE") == 0 || strcmp(a, "-fpie") == 0) {
            if (strncmp(a, "-fno-", 5) == 0)
                opt_no_pic = true;              /* (62f) */
            else if (strcmp(a, "-fPIC") == 0 || strcmp(a, "-fpic") == 0)
                opt_no_pic = false;
            cpp_opts[ncpp++] = (char *)a;
        } else if (strcmp(a, "-mgeneral-regs-only") == 0) {
            opt_general_regs = true;            /* (62f) */
            cpp_opts[ncpp++] = (char *)a;
        } else if (strcmp(a, "-mavx") == 0 || strcmp(a, "-mavx2") == 0 || strcmp(a, "-mfma") == 0) {
            /* (32d) only their macros: cc writes no vector code itself, and
             * <immintrin.h>'s instructions are there with or without them */
            cpp_opts[ncpp++] = (char *)a;
        } else if (strcmp(a, "-mcmodel=kernel") == 0 || strcmp(a, "-mcmodel=small") == 0 ||
                   strcmp(a, "-mno-red-zone") == 0 || strcmp(a, "-mno-stack-arg-probe") == 0) {
            /* (62f) taken: every address is PC-relative or a 64-bit word, and
             * nothing is kept below the stack pointer; (62g) no stack probes
             * are ever made */
        } else if (strncmp(a, "-W", 2) == 0 || strncmp(a, "-O", 2) == 0 || strcmp(a, "-g") == 0 ||
                   strncmp(a, "-std=", 5) == 0 || strncmp(a, "-f", 2) == 0 || strcmp(a, "-pipe") == 0 ||
                   strcmp(a, "-pedantic") == 0) {
            /* warnings, optimization and debugging: not done (yet) */
        } else if (strcmp(a, "-cc1") == 0)
            ;                                   /* (see below) */
        else if (strcmp(a, "-asm") == 0)
            assembly = lex_assembly = true;     /* (internal: what cc passes for .S files) */
        else if (a[0] == '-' && a[1])
            fatal("unknown option '%s'", a);
        else
            inputs[ninputs++] = (char *)a;
    }
    if (!ninputs)
        usage();
    /* -cc1 FILE: one compilation in this process (what cc runs for each C file) */
    for (int i = 1; i < argc; i++)
        if (strcmp(argv[i], "-cc1") == 0) {
            if (std_includes)
                cpp_add_include(format(CC_ROOT "/include"), true);
            cpp_dependencies_all = dep_mode == 2;
            cpp_predefine();
            if (assembly)
                cpp_define("__ASSEMBLER__");    /* (as clang for .S files) */
            for (int k = 0; k < ncpp; k++) {
                char *o = cpp_opts[k];
                if (strcmp(o, "-target") == 0 || strcmp(o, "-nostdinc") == 0 || strcmp(o, "-O0") == 0 ||
                    strncmp(o, "-f", 2) == 0 || strcmp(o, "-mgeneral-regs-only") == 0) {
                    if (strcmp(o, "-target") == 0)
                        k++;
                    continue;
                }
                if (strncmp(o, "-m", 2) == 0) {
                    if (target.arch != ARCH_X86_64)
                        fatal("'%s' is x86-64's", o);
                    if (strcmp(o, "-mfma") == 0)
                        cpp_define("__FMA__");
                    else
                        cpp_define("__AVX__");
                    if (strcmp(o, "-mavx2") == 0)
                        cpp_define("__AVX2__");
                    continue;
                }
                if (strcmp(o, "-isystem") == 0)
                    cpp_add_include(cpp_opts[++k], true);
                else if (o[1] == 'I')
                    cpp_add_include(o + 2, false);
                else if (o[1] == 'D')
                    cpp_define(o + 2);
                else
                    cpp_undefine(o + 2);
            }
            return compile(inputs[0], output, mode == PREPROCESS, mode == SYNTAX);
        }
    atexit(remove_temps);
    bool several = 0;
    {
        int sources = 0;
        for (int i = 0; i < ninputs; i++)
            if (inputs[i][0] != '-' || inputs[i][1] != 'l')
                sources++;
        several = sources > 1;
    }
    if (output && several && mode != LINK)
        fatal("-o with -c, -S or -E takes one input file");
    char *me = strchr(self, '/') ? (char *)self : tool("cc");
    char **objects = calloc((size_t)ninputs + 1, sizeof(char *));
    int nobjects = 0;
    for (int i = 0; i < ninputs; i++) {
        char *in = inputs[i];
        const char *sfx = suffix(in);
        if (in[0] == '-' && in[1] == 'l') {
            objects[nobjects++] = in;
            continue;
        }
        bool c = strcmp(sfx, ".c") == 0, s = strcmp(sfx, ".s") == 0, S = strcmp(sfx, ".S") == 0;
        if (!c && !s && !S) {
            if (mode != LINK)
                fatal("%s: not a C or assembly file", in);
            objects[nobjects++] = in;           /* an object, archive or library */
            continue;
        }
        /* C (and .S, preprocessed only) through cc -cc1 */
        char *asm_file = in;
        if (c || S) {
            if (mode == PREPROCESS || mode == SYNTAX || (mode == ASSEMBLY && c)) {
                char *o = output ? (char *)output : mode == ASSEMBLY ? replace_suffix(in, ".s") : "-";
                char **args = calloc((size_t)ncpp + 8, sizeof(char *));
                int n = 0;
                args[n++] = me;
                args[n++] = "-cc1";
                args[n++] = mode == PREPROCESS ? "-E" : mode == SYNTAX ? "-fsyntax-only" : "-S";
                if (S)
                    args[n++] = "-asm";
                for (int k = 0; k < ncpp; k++)
                    args[n++] = cpp_opts[k];
                args[n++] = "-o";
                args[n++] = o;
                args[n++] = in;
                if (run(args))
                    return 1;
                continue;
            }
            asm_file = temp_file(".s");
            char **args = calloc((size_t)ncpp + 16, sizeof(char *));
            int n = 0;
            args[n++] = me;
            args[n++] = "-cc1";
            args[n++] = S ? "-E" : "-S";
            if (S)
                args[n++] = "-asm";
            if (dep_mode) {                     /* (63g) the rule beside the object */
                char *object = mode == OBJECT && output ? (char *)output : replace_suffix(in, ".o");
                args[n++] = dep_mode == 2 ? "-MD" : "-MMD";
                if (dep_phony)
                    args[n++] = "-MP";
                args[n++] = "-MF";
                args[n++] = dep_file ? (char *)dep_file : beside(object, ".d");
                args[n++] = "-MT";
                args[n++] = dep_target ? (char *)dep_target : object;
            }
            for (int k = 0; k < ncpp; k++)
                args[n++] = cpp_opts[k];
            args[n++] = "-o";
            args[n++] = asm_file;
            args[n++] = in;
            if (run(args))
                return 1;
        }
        if (mode == ASSEMBLY) {
            /* (an .s given to -S: nothing to do) */
            continue;
        }
        char *obj = mode == OBJECT ? (output ? (char *)output : replace_suffix(in, ".o")) : temp_file(".o");
        char *as_args[] = { tool("as"), "-target", (char *)target.name, "-o", obj, asm_file, 0 };
        if (run(as_args))
            return 1;
        objects[nobjects++] = obj;
    }
    if (mode != LINK)
        return 0;
    /* ld: the start code, the files and -l libraries in order, then the C library */
    char **args = calloc((size_t)(nobjects + nld + 16), sizeof(char *));
    int n = 0;
    args[n++] = tool("ld");
    args[n++] = "-o";
    args[n++] = (char *)(output ? output : "a.out");
    char *lib_dir = format(CC_ROOT "/%s/lib", target.name);
    for (int k = 0; k < nld; k++)
        args[n++] = ld_opts[k];
    if (std_libs) {
        args[n++] = format("-L%s", lib_dir);
        args[n++] = format("%s/start.o", lib_dir);
    }
    for (int k = 0; k < nobjects; k++)
        args[n++] = objects[k];
    if (std_libs) {
        args[n++] = "--as-needed";
        args[n++] = format("%s/libritteros.so.1", lib_dir);
        args[n++] = "--no-as-needed";
        args[n++] = format("%s/libritteros.a", lib_dir);
    }
    return run(args);
}
