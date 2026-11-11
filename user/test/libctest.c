/*
 * Test: the C library for ports (22m). Plain standard C, so the test builds
 * it on the Mac as well and compares the outputs line by line: the Mac's C
 * library is the reference. libctest FOLDER (a folder it may write a file in).
 */
#include <assert.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fenv.h>
#include <inttypes.h>
#include <locale.h>
#include <math.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utime.h>
#include <wchar.h>
#include <wctype.h>

#define T "libctest: "

/* As POSIX programs declare it (on RitterOS the macro makes this a
 * declaration of the function behind it). */
extern char **environ;

static int sign(int v)
{
    return v < 0 ? -1 : v > 0 ? 1 : 0;
}

static jmp_buf jump;

/* Calls itself n times deep, then jumps back with value. */
static int deep(int n, int value)
{
    volatile char pad[64];
    pad[0] = (char)n;
    if (n > 0)
        return deep(n - 1, value) + pad[0];
    longjmp(jump, value);
}

/* A line's number through fgets and sscanf (C's way to read numbers). */
static int fscanf_like(FILE *f, int *v)
{
    char line[64];
    return fgets(line, sizeof(line), f) ? sscanf(line, "scratch %d", v) : -1;
}

static void said_exit1(void) { printf(T "  atexit: the first given, called last\n"); }
static void said_exit2(void) { printf(T "  atexit: the second given, called first\n"); }
/* (quick_exit flushes no streams: straight to the descriptor) */
static void said_quick(void) { write(1, T "  at_quick_exit called\n", sizeof(T) + 22); }

/* Signals (22r) */
static volatile sig_atomic_t usr1_seen;
static char order[64];

static void on_usr1(int sig)
{
    usr1_seen += sig == SIGUSR1;
}

static void note(const char *what)
{
    strcat(order, what);
}

static void on_usr2_outer(int sig)
{
    (void)sig;
    note("[outer");
    raise(SIGUSR2 == sig ? SIGUSR1 : SIGUSR2);  /* (a nested one: USR1 is handled by on_nested) */
    note(" outer]");
}

static void on_nested(int sig)
{
    (void)sig;
    note(" inner");
}

static jmp_buf fault_jump;

static void on_segv(int sig)
{
    longjmp(fault_jump, sig == SIGSEGV ? 1 : 2);
}

static void on_abort(int sig)
{
    write(1, T "  SIGABRT handler ran\n", sizeof(T) + 21);
    (void)sig;
}

/* libctest FOLDER exit|quick|_Exit: ends early that way (22p; the main
 * run starts itself again with these and looks at what happened). Since
 * 22r also reset (SA_RESETHAND), abort and segv. */
static int end_early(const char *folder, const char *how)
{
    char path[512];
    if (!strcmp(how, "reset")) {
        struct sigaction a = { .sa_handler = on_usr1, .sa_flags = SA_RESETHAND };
        sigemptyset(&a.sa_mask);
        sigaction(SIGUSR1, &a, 0);
        raise(SIGUSR1);
        printf(T "  reset: handled once (%d), now the default\n", (int)usr1_seen);
        fflush(stdout);
        raise(SIGUSR1);
        printf(T "  reset: still here (wrong)\n");
        return 0;
    }
    if (!strcmp(how, "abort")) {
        signal(SIGABRT, on_abort);
        abort();
    }
    if (!strcmp(how, "segv")) {
        signal(SIGSEGV, on_segv);
        int jumped = setjmp(fault_jump);
        if (jumped == 0) {
            volatile int *nowhere = (volatile int *)(uintptr_t)16;
            *nowhere = 1;
            printf(T "  segv: no fault (wrong)\n");
        }
        printf(T "  segv: the handler jumped back (%d)\n", jumped);
        return 0;
    }
    atexit(said_exit1);
    atexit(said_exit2);
    at_quick_exit(said_quick);
    if (!strcmp(how, "exit"))
        exit(6);
    if (!strcmp(how, "quick"))
        quick_exit(5);
    snprintf(path, sizeof(path), "%s/libctest-unflushed.txt", folder);
    FILE *f = fopen(path, "w");
    fputs("never written", f);              /* (still in its buffer) */
    _Exit(7);
}

static int compare_ints(const void *a, const void *b)
{
    int x = *(const int *)a, y = *(const int *)b;
    return x < y ? -1 : x > y;
}

static int compare_names(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        printf(T "usage: libctest FOLDER\n");
        return 2;
    }
    if (argc > 2)
        return end_early(argv[1], argv[2]);
    /* printf's integers: precision, flags, sizes */
    printf(T "%.3d|%5.3d|%-6.2d|%.0d|%+d|% d|%#x|%#o|%#X|%05d|%-4d|\n", 33, 7, 5, 0, 12, 34,
           255, 8, 171, -42, 9);
    printf(T "%hhd|%hd|%lld|%llu|%lx|%o|%5s|%-5s|%.2s|%c|%%|%3c|\n", (signed char)200,
           (short)70000, -1234567890123LL, 18446744073709551615ULL, 0xdeadbeefUL, 511, "ab",
           "cd", "efgh", 'Z', 'q');
    printf(T "%.2f|%8.3f|%-8.1f|%.0f|\n", 3.14159, -2.71828, 0.75, 41.6);
    char lump[16];
    snprintf(lump, sizeof(lump), "STCFN%.3d", 33);
    printf(T "%s %s\n", lump, (snprintf(lump, sizeof(lump), "WIA%.3d%.2d", 1, 2), lump));

    /* sscanf */
    int a = 0, b = 0, c = 0, d = 0, n = 0;
    char word[16] = "", three[4] = "", set[16] = "";
    int got = sscanf("42 0x1F 017 -9 abc defg", "%d %x %o %i %s %3c%n", &a, &b, &c, &d, word,
                     three, &n);
    three[3] = 0;
    printf(T "sscanf %d: %d %d %d %d %s %s n=%d\n", got, a, b, c, d, word, three, n);
    got = sscanf("0x10 010 12", "%i %i %i", &a, &b, &c);
    printf(T "sscanf %d: %d %d %d\n", got, a, b, c);
    got = sscanf("abc123", "%[a-z]%d", set, &a);
    printf(T "sscanf %d: %s %d\n", got, set, a);
    printf(T "sscanf empty %d, no number %d, skipped %d\n", sscanf("", "%d", &a),
           sscanf("x", "%d", &a), sscanf("7 8", "%*d %d", &b));
    printf(T "sscanf b=%d\n", b);
    long long big = 0;
    unsigned u = 0;
    sscanf("-9000000000 4000000000", "%lld %u", &big, &u);
    printf(T "sscanf %lld %u\n", big, u);

    /* numbers from text */
    char *end;
    long l = strtol("  -123abc", &end, 10);
    printf(T "strtol %ld rest '%s'\n", l, end);
    printf(T "strtol %ld %ld %ld %lu\n", strtol("0x1f", 0, 0), strtol("077", 0, 0),
           strtol("z", 0, 36), strtoul("ff", 0, 16));
    double v = strtod("1.5e3xyz", &end);
    printf(T "strtod %.3f rest '%s' atof %.2f %.4f %.1f\n", v, end, atof("-2.25"), atof(".5"),
           atof("1e2"));
    strtod("abc", &end);
    printf(T "strtod none: rest '%s'\n", end);
    printf(T "abs %d %ld %lld\n", abs(-5), labs(-6L), llabs(-7LL));

    /* strings */
    char *dup = strdup("hello"), *ndup = strndup("hello", 3);
    char pad[8];
    memset(pad, 'x', sizeof(pad));
    strncpy(pad, "ab", 5);
    char cat[32] = "foo";
    strcat(cat, "bar");
    strncat(cat, "bazqux", 3);
    printf(T "strdup %s %s strncpy %d%d%d%d%d%c strcat %s\n", dup, ndup, pad[0], pad[1], pad[2],
           pad[3], pad[4], pad[5], cat);
    free(dup);
    free(ndup);
    printf(T "strcasecmp %d %d %d strncasecmp %d %d\n", sign(strcasecmp("Hello", "hELLO")),
           sign(strcasecmp("abc", "ABD")), sign(strcasecmp("b", "A")),
           sign(strncasecmp("STCFN033", "stcfn044", 5)), sign(strncasecmp("ab", "AC", 2)));

    /* ctype */
    int counts[12] = { 0 };
    for (int ch = 0; ch < 128; ch++) {
        counts[0] += !!isalnum(ch), counts[1] += !!isalpha(ch), counts[2] += !!iscntrl(ch);
        counts[3] += !!isdigit(ch), counts[4] += !!isgraph(ch), counts[5] += !!islower(ch);
        counts[6] += !!isprint(ch), counts[7] += !!ispunct(ch), counts[8] += !!isspace(ch);
        counts[9] += !!isupper(ch), counts[10] += !!isxdigit(ch), counts[11] += !!isblank(ch);
    }
    printf(T "ctype");
    for (int i = 0; i < 12; i++)
        printf(" %d", counts[i]);
    char up[8] = "doom2!";
    for (char *p = up; *p; p++)
        *p = (char)toupper((unsigned char)*p);
    printf(" %s %c\n", up, tolower('Q'));

    /* assert, math, inttypes */
    assert(argc >= 2);
    printf(T "math %.3f %.6f %.1f %.1f %.1f %.1f %.1f %.3f %.3f %.1f\n", fabs(-2.5), sqrt(2.0),
           floor(-1.5), ceil(-1.5), trunc(-1.7), round(2.5), round(-2.5), fmod(7.5, 2.0),
           fmod(-7.5, 2.0), floor(3.0));
    printf(T "inttypes %" PRId64 " %" PRIx64 " %" PRIu32 "\n", (int64_t)-5, (uint64_t)255,
           (uint32_t)4000000000u);

    /* streams */
    char path[512];
    snprintf(path, sizeof(path), "%s/libctest.txt", argv[1]);
    FILE *f = fopen(path, "w");
    if (!f) {
        printf(T "fopen w failed\n");
        return 1;
    }
    fprintf(f, "line one %d\n", 1);
    int put = fputs("line two\n", f);
    printf(T "fputs succeeded %d, one character %d\n", put >= 0, fputs("", f) >= 0 && fputs("-", f) >= 0);
    fputc('X', f);
    unsigned char bytes[300];
    for (int i = 0; i < 300; i++)
        bytes[i] = (unsigned char)(i * 7);
    printf(T "fwrite %zu\n", fwrite(bytes, 3, 100, f));
    printf(T "fclose %d\n", fclose(f));
    f = fopen(path, "rb");
    char line[64];
    fgets(line, sizeof(line), f);
    printf(T "fgets '%.8s' ftell %ld", line, ftell(f));
    fgets(line, sizeof(line), f);
    int ch1 = fgetc(f);
    ungetc(ch1, f);
    printf(" '%.8s' fgetc %c again %c", line, ch1, fgetc(f));
    unsigned char back[300];
    size_t r = fread(back, 1, 300, f);
    printf(" fread %zu same %d\n", r, memcmp(back, bytes, 300) == 0);
    printf(T "eof %d fread more %zu eof %d\n", feof(f), fread(back, 1, 10, f), feof(f) != 0);
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 5, SEEK_SET);
    printf(T "size %ld at %ld char %c\n", size, ftell(f), fgetc(f));
    rewind(f);
    printf(T "rewind %ld %c\n", ftell(f), fgetc(f));
    fclose(f);
    f = fopen(path, "a");
    fputs("tail\n", f);
    fclose(f);
    f = fopen(path, "r+");
    fseek(f, 0, SEEK_SET);
    fputs("LINE", f);
    fseek(f, 0, SEEK_END);
    printf(T "append and overwrite: size %ld", ftell(f));
    fseek(f, 0, SEEK_SET);
    fgets(line, sizeof(line), f);
    line[strcspn(line, "\n")] = 0;
    printf(" first '%s'\n", line);
    fclose(f);
    printf(T "remove %d fopen after: %s errno ENOENT %d\n", remove(path),
           fopen(path, "r") ? "found" : "gone", errno == ENOENT);
    {   /* (63k) 40 nested folders open at once (the library kept 8 until 63k) */
        char deep[4096];
        int made = 0, opened = 0, read_back = 0;
        DIR *open_dirs[40];
        snprintf(deep, sizeof(deep), "%s/deep", argv[1]);
        for (int i = 0; i < 40 && mkdir(deep, 0755) == 0; i++) {
            made++;
            strcat(deep, "/d");
        }
        char at[4096];                          /* (all made first: then each opened) */
        snprintf(at, sizeof(at), "%s/deep", argv[1]);
        for (int i = 0; i < made && (open_dirs[i] = opendir(at)); i++) {
            opened++;
            strcat(at, "/d");
        }
        for (int i = 0; i < opened; i++) {
            struct dirent *e;
            while ((e = readdir(open_dirs[i])))
                read_back += !strcmp(e->d_name, "d");
            closedir(open_dirs[i]);
        }
        for (int i = made; i > 0; i--) {         /* (from the deepest) */
            deep[strlen(deep) - 2] = 0;
            rmdir(deep);
        }
        printf(T "folders: %d made, %d open at once, %d entries read\n", made, opened, read_back);
        char file[4096];
        snprintf(file, sizeof(file), "%s/not-a-folder", argv[1]);
        FILE *nf = fopen(file, "w");
        if (nf)
            fclose(nf);
        errno = 0;
        DIR *none = opendir(file);
        printf(T "opendir of a file: %s, ENOTDIR %d\n", none ? "opened" : "refused", errno == ENOTDIR);
        if (none)
            closedir(none);
        remove(file);
    }
    {   /* (63m) a file's times: utimensat, UTIME_OMIT, futimens, utime, utimes, a link itself */
        char tf[4096], tl[4096];
        snprintf(tf, sizeof(tf), "%s/times.txt", argv[1]);
        snprintf(tl, sizeof(tl), "%s/times.link", argv[1]);
        FILE *w = fopen(tf, "w");
        if (w) {
            fputs("x", w);
            fclose(w);
        }
        struct stat s;
        long m[8] = { 0 };
        int r[8];
        struct timespec ts[2] = { { 1000000000, 0 }, { 1234567890, 500000000 } };
        r[0] = utimensat(AT_FDCWD, tf, ts, 0);
        m[0] = stat(tf, &s) == 0 ? (long)s.st_mtime : -1;
        struct timespec omit[2] = { { 0, UTIME_OMIT }, { 0, UTIME_OMIT } };
        r[1] = utimensat(AT_FDCWD, tf, omit, 0);
        m[1] = stat(tf, &s) == 0 ? (long)s.st_mtime : -1;
        int fd = open(tf, O_RDONLY);
        struct timespec f2[2] = { { 0, UTIME_OMIT }, { 1500000000, 0 } };
        r[2] = futimens(fd, f2);
        close(fd);
        m[2] = stat(tf, &s) == 0 ? (long)s.st_mtime : -1;
        struct utimbuf ub = { 1600000000, 1600000001 };
        r[3] = utime(tf, &ub);
        m[3] = stat(tf, &s) == 0 ? (long)s.st_mtime : -1;
        struct timeval tv[2] = { { 1700000000, 0 }, { 1700000002, 999999 } };
        r[4] = utimes(tf, tv);
        m[4] = stat(tf, &s) == 0 ? (long)s.st_mtime : -1;
        r[5] = symlink("times.txt", tl);
        struct timespec l2[2] = { { 0, UTIME_OMIT }, { 1100000000, 0 } };
        r[6] = utimensat(AT_FDCWD, tl, l2, AT_SYMLINK_NOFOLLOW);
        m[5] = lstat(tl, &s) == 0 ? (long)s.st_mtime : -1;
        m[6] = stat(tf, &s) == 0 ? (long)s.st_mtime : -1;
        struct timespec bad[2] = { { 0, UTIME_OMIT }, { 5, 2000000000 } };
        errno = 0;
        r[7] = utimensat(AT_FDCWD, tf, bad, 0);
        int einval = errno == EINVAL;
        int now = utimensat(AT_FDCWD, tf, 0, 0);
        m[7] = stat(tf, &s) == 0 ? (long)s.st_mtime : -1;
        printf(T "times: utimensat %d %ld, omit %d %ld, futimens %d %ld, utime %d %ld, utimes %d %ld\n",
               r[0], m[0], r[1], m[1], r[2], m[2], r[3], m[3], r[4], m[4]);
        printf(T "times: a link itself %d %d %ld (its file %ld), now %d %d\n",
               r[5], r[6], m[5], m[6], now, m[7] > 1700000002);
        /* (POSIX's EINVAL: the Mac takes a tv_nsec of 2e9, so not compared) */
        printf("libctest-own: times: bad nsec %d EINVAL %d\n", r[7], einval);
        remove(tl);
        remove(tf);
    }

    /* setjmp, locale, temporary files (22n) */
    volatile int steps = 0;
    int jumped = setjmp(jump);
    steps++;
    if (jumped == 0)
        deep(10, 0);
    else if (jumped == 1)
        deep(20, 7);
    printf(T "setjmp: back %d times, last value %d, steps %d\n", steps - 1, jumped, steps);
    char *loc = setlocale(LC_ALL, "C");
    char *now = setlocale(LC_NUMERIC, 0);
    char *other = setlocale(LC_ALL, "xx_YY.nothing");
    struct lconv *lc = localeconv();
    printf(T "locale %s %s %s, '%s' '%s' '%s' %d\n", loc, now, other ? other : "refused",
           lc->decimal_point, lc->thousands_sep, lc->grouping, lc->frac_digits);
    char name[L_tmpnam];
    struct stat ts;
    char *t1 = tmpnam(name), *t2 = tmpnam(0);
    printf(T "tmpnam %d %d, apart %d, free %d\n", t1 == name, t2 != 0, t1 && t2 && strcmp(t1, t2),
           t1 && stat(t1, &ts) < 0);
    FILE *tf = tmpfile();
    fputs("scratch 123\n", tf);
    rewind(tf);
    int scratch = 0;
    printf(T "tmpfile %d read %d", tf != 0, fscanf_like(tf, &scratch));
    printf(" %d\n", scratch);
    fclose(tf);
    snprintf(path, sizeof(path), "%s/libctest-XXXXXX", argv[1]);
    int tfd = mkstemp(path);
    size_t plen = strlen(path);
    printf(T "mkstemp %d, changed %d, exists %d, mode %o", tfd >= 0,
           strcmp(path + plen - 6, "XXXXXX") != 0, stat(path, &ts) == 0, (unsigned)(ts.st_mode & 0777));
    printf(", wrote %zd", write(tfd, "abc", 3));
    close(tfd);
    printf(", removed %d\n", remove(path) == 0);   /* (no XXXXXX: undefined, the Mac makes the file) */

    /* time.h (22o): fixed times, so both systems must print the same */
    static const long long when[] = { 0, 1700000000LL, -1, 951782400LL, 951868800LL,
                                       4102444800LL, -2147483648LL, 1104537600LL, 1230508800LL,
                                       1262476800LL, 1609459199LL };
    char text[160];
    for (unsigned i = 0; i < sizeof(when) / sizeof(when[0]); i++) {
        time_t t = (time_t)when[i];
        struct tm g, l;
        gmtime_r(&t, &g);
        localtime_r(&t, &l);
        strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S %a %j %w %U %W %u %V %G %g", &g);
        printf(T "gm %lld %s", when[i], text);
        strftime(text, sizeof(text), "%F %T %z %Z yday %j", &l);
        printf(" | local %s\n", text);
    }
    time_t fixed = 1700000000;
    struct tm *lt = localtime(&fixed);
    strftime(text, sizeof(text), "%c|%x|%X|%D|%R|%r|%I %p|%e|%h %B|%A|%C|%y|%t|%%|%Ey %Od", lt);
    printf(T "strftime '%s'\n", text);
    printf(T "strftime small %zu, asctime %s", strftime(text, 5, "%Y-%m-%d", lt), asctime(lt));
    printf(T "ctime %s", ctime(&fixed));
    struct tm odd = { .tm_year = 124, .tm_mon = 13, .tm_mday = 31, .tm_hour = 25, .tm_min = 61,
                      .tm_sec = -1, .tm_isdst = 0 };
    time_t made = mktime(&odd);
    strftime(text, sizeof(text), "%F %T %a yday %j", &odd);
    printf(T "mktime %lld -> %s", (long long)made, text);
    struct tm round;
    localtime_r(&fixed, &round);
    round.tm_isdst = -1;
    printf(", round trip %d", mktime(&round) == fixed);
    struct tm before = { .tm_year = 60, .tm_mon = 6, .tm_mday = 4, .tm_hour = 12 };
    printf(", 1960 %lld, difftime %.1f\n", (long long)mktime(&before), difftime(fixed, 1600000000));
    time_t now_t, now_s = time(&now_t);
    printf(T "time now %d, same %d, CLOCKS_PER_SEC %ld\n", now_s > 1700000000, now_s == now_t,
           (long)CLOCKS_PER_SEC);
    clock_t c0 = clock();
    volatile unsigned long spin = 0;
    while (clock() - c0 < CLOCKS_PER_SEC / 20 && spin < 4000000000UL)
        spin++;
    clock_t c1 = clock();
    sleep(1);
    clock_t c2 = clock();
    printf(T "clock started %d, counts busy work %d, not a second of sleep %d\n", c0 >= 0,
           c1 - c0 >= CLOCKS_PER_SEC / 20, c2 - c1 < CLOCKS_PER_SEC / 5);

    /* stdlib.h and string.h (22p) */
    static int numbers[2000];
    uint32_t seed = 12345;
    for (int i = 0; i < 2000; i++) {
        seed = seed * 1103515245u + 12345u;
        numbers[i] = (int)(seed >> 8) % 500 - 250;      /* (many equal ones) */
    }
    qsort(numbers, 2000, sizeof(int), compare_ints);
    int sorted = 1;
    long long weighted = 0;
    for (int i = 0; i < 2000; i++) {
        if (i && numbers[i - 1] > numbers[i])
            sorted = 0;
        weighted += (long long)numbers[i] * (i + 1);
    }
    int key = numbers[777], missing = 9999;
    int *hit = bsearch(&key, numbers, 2000, sizeof(int), compare_ints);
    printf(T "qsort sorted %d, first %d last %d weighted %lld; bsearch %d %d\n", sorted,
           numbers[0], numbers[1999], weighted, hit && *hit == key,
           bsearch(&missing, numbers, 2000, sizeof(int), compare_ints) == 0);
    char *names[] = { "pear", "apple", "fig", "banana", "kiwi", "cherry", "date", "apricot",
                      "mango", "lime", "grape", "plum" };
    qsort(names, 12, sizeof(char *), compare_names);
    printf(T "qsort names");
    for (int i = 0; i < 12; i++)
        printf(" %s", names[i]);
    qsort(names, 0, sizeof(char *), compare_names);
    qsort(names, 1, sizeof(char *), compare_names);
    printf(" (and 0, 1 elements: %s)\n", names[0]);
    srand(42);
    int r1 = rand(), r2 = rand();
    srand(42);
    int same = rand() == r1 && rand() == r2;
    int in_range = 1;
    for (int i = 0; i < 1000; i++) {
        int v = rand();
        if (v < 0 || v > RAND_MAX)
            in_range = 0;
    }
    printf(T "rand repeats %d, in range %d, RAND_MAX %d\n", same, in_range, RAND_MAX);
    div_t dv = div(-7, 2);
    ldiv_t lv = ldiv(7L, -2L);
    lldiv_t llv = lldiv(-9000000000LL, 7);
    printf(T "div %d %d, ldiv %ld %ld, lldiv %lld %lld, atoll %lld\n", dv.quot, dv.rem, lv.quot,
           lv.rem, llv.quot, llv.rem, atoll("  -9000000000x"));
    char *fend;
    float fv = strtof("3.14159xyz", &fend);
    strtold("2.5e3!", &end);
    printf(T "strtof %.6f rest '%s', strtold rest '%s'\n", (double)fv, fend, end);
    /* (sizes that are multiples of the alignment: C11 asked for that, and
     * the Mac's refuses others) */
    void *a64 = aligned_alloc(64, 128), *a4k = aligned_alloc(4096, 8192);
    void *abig = aligned_alloc(65536, 327680), *abad = aligned_alloc(48, 96);
    printf(T "aligned_alloc 64:%d 4096:%d 65536:%d, 48 refused %d",
           a64 && (uintptr_t)a64 % 64 == 0, a4k && (uintptr_t)a4k % 4096 == 0,
           abig && (uintptr_t)abig % 65536 == 0, abad == 0);
    if (!a64 || !a4k || !abig)
        return 1;
    memset(a64, 1, 128);
    memset(a4k, 2, 8192);
    memset(abig, 3, 300000);
    char *grown = realloc(abig, 400000);
    printf(", kept by realloc %d\n", grown && grown[299999] == 3);
    free(a64);
    free(a4k);
    free(grown);
    char hi[2] = { (char)0xE9, 0 };
    wchar_t wc = 0, wide[8];
    char narrow[8], one;
    int m1 = mbtowc(&wc, hi, 1);
    errno = 0;
    int w300 = wctomb(&one, 300);
    printf(T "mblen %d %d %d, mbtowc e9 %d %x, wctomb 200 %d, 300 %d EILSEQ %d", mblen("a", 1),
           mblen("", 1), mblen("a", 0), m1, (unsigned)wc, wctomb(&one, 200), w300,
           errno == EILSEQ);
    size_t nw = mbstowcs(wide, "abc", 8), nb = wcstombs(narrow, wide, 8);
    printf(", mbstowcs %zu %x, wcstombs %zu %s, MB_CUR_MAX %d\n", nw, (unsigned)wide[2], nb, narrow,
           (int)MB_CUR_MAX);
    char words[] = "  alpha, beta;;gamma ,delta  ";
    printf(T "strtok");
    for (char *w = strtok(words, " ,;"); w; w = strtok(0, " ,;"))
        printf(" [%s]", w);
    printf(" then %s\n", strtok(0, " ,;") ? "more" : "none");
    char copy[16];
    memset(copy, 'x', sizeof(copy));
    char *after = memccpy(copy, "abc:def", ':', 7);
    printf(T "strspn %zu %zu, strpbrk '%s' %d, strnlen %zu %zu, memccpy %ld '%.4s'",
           strspn("aabbcx", "abc"), strspn("xyz", "abc"), strpbrk("hello, world", " ,"),
           strpbrk("abc", "xyz") == 0, strnlen("abcdef", 3), strnlen("ab", 10),
           (long)(after - copy), copy);
    printf(" %d", memccpy(copy, "abc", 'z', 3) == 0);
    size_t xl = strxfrm(copy, "longer text", 5);
    printf(", strcoll %d %d, strxfrm %zu '%s'\n", sign(strcoll("apple", "banana")),
           strcoll("same", "same"), xl, copy);
    char joined[32];
    char *at = stpcpy(stpcpy(joined, "join"), "ed!");     /* (32e) */
    printf(T "stpcpy '%s' %ld %d\n", joined, (long)(at - joined), *at);

    /* the ways to end (22p, 22r): this program again, ending early */
    for (int k = 0; k < 6; k++) {
        static const char *const ways[] = { "exit", "quick", "_Exit", "reset", "abort", "segv" };
        char command[1200];
        snprintf(command, sizeof(command), "%s %s %s", argv[0], argv[1], ways[k]);
        fflush(stdout);
        int st = system(command);
        /* (a signal: the Mac's sh hands over to the command, RitterOS's
         * exits with 128 + it, as Unix shells that wait do) */
        int code = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
        int by = WIFSIGNALED(st) ? WTERMSIG(st) : code > 128 ? code - 128 : 0;
        if (by)
            printf(T "%s: ended by %s\n", ways[k], by == SIGUSR1 ? "SIGUSR1" : by == SIGABRT
                   ? "SIGABRT" : by == SIGSEGV ? "SIGSEGV" : "another signal");
        else
            printf(T "%s: exit code %d\n", ways[k], code);
    }
    snprintf(path, sizeof(path), "%s/libctest-unflushed.txt", argv[1]);
    FILE *uf = fopen(path, "r");
    char ufbuf[32] = "";
    if (uf) {
        fgets(ufbuf, sizeof(ufbuf), uf);
        fclose(uf);
    }
    printf(T "_Exit left the stream unwritten: '%s'\n", ufbuf);
    remove(path);

    /* stdio.h (22q): exact floating point, scanf, the rest */
    static const double fv_list[] = { 0.1, -2.5, 1e100, 123456.789, 5e-324, 2.2250738585072014e-308,
                                      1.7976931348623157e308, 0.0, -0.0, 1.0 / 3, 9.995, 1e21,
                                      0.000123456, 65536.0 };
    for (unsigned i = 0; i < sizeof(fv_list) / sizeof(fv_list[0]); i++) {
        double x = fv_list[i];
        printf(T "fp %.17g|%e|%.3e|%E|%f|%.2f|%g|%.10g|%G|%a|%.13a|%A|%+.4e|% g|%#.0f|%#g\n", x, x,
               x, x, x, x, x, x, x, x, x, x, x, x, x, x);
    }
    printf(T "fp wide %.40f|%.25e|%.0e|%.0f|%.0f|%.0f|%.1f|%.2f\n", 0.1, 1.0 / 3, 25.0, 0.5, 1.5, 2.5,
           0.25, 1.005);
    printf(T "fp max %f\n", 1.7976931348623157e308);
    printf(T "fp pad [%010.3f] [%-12.2e] [%+08.2f] [%08.3g] [%12a] [%-10g] [%010a] [%-+9.1f]\n",
           -3.14159, 1234.5, 1.5, 0.000012345, 1.0, 100.0, 1.0, 2.25);
    double inf = 1.0 / 0.0, qnan = 0.0 / 0.0;
    printf(T "fp special [%f] [%e] [%g] [%a] [%F] [%E] [%G] [%A] [%8f] [%-8f] [%08f] [%+f]\n", inf, -inf,
           qnan, inf, inf, -inf, qnan, inf, inf, -inf, inf, inf);
    printf(T "fp long double %Lf %.3Le %Lg\n", (long double)2.5, (long double)1e10,
           (long double)0.0001);
    static const char *const texts[] = { "0x1.8p3", "-0x.8p1", "0x1p-1074", "0x1.fffffffffffff8p1023",
                                         "1e-320", "4.9e-324", "2.4703282292062327e-324",
                                         "2.4703282292062328e-324", "1.7976931348623157e308",
                                         "1.7976931348623158e308", "1e309", "1e-400", "  -0.0",
                                         "infinity", "-INF", "nan(abc)", "123.456e-2x",
                                         "9007199254740993", "9007199254740993.0000000001",
                                         "0.1000000000000000055511151231257827021181583404541015625",
                                         "1234567890123456789012345678901234567890123456789012345678901234567890e-60",
                                         ".", "e5", "1e", "0x" };
    for (unsigned i = 0; i < sizeof(texts) / sizeof(texts[0]); i++) {
        char *tend;
        errno = 0;
        double x = strtod(texts[i], &tend);
        int range = errno == ERANGE;
        errno = 0;
        float fx = strtof(texts[i], 0);
        printf(T "strtod '%s': %a rest '%s'%s; strtof %a\n", texts[i], x, tend,
               range && (x == 0 || x == inf || x == -inf) ? " (ERANGE)" : "", (double)fx);
    }
    static const char *const ftexts[] = { "3.4028235e38", "3.4028236e38", "1.17549435e-38", "1e-46",
                                          "1.4e-45", "0.1", "16777217" };
    for (unsigned i = 0; i < sizeof(ftexts) / sizeof(ftexts[0]); i++) {
        errno = 0;
        float fx = strtof(ftexts[i], 0);
        printf(T "strtof '%s': %a%s\n", ftexts[i], (double)fx,
               errno == ERANGE && (fx == 0 || fx == inf) ? " (ERANGE)" : "");
    }
    float sf;
    double sd, sd2;
    long double sl;
    int sn = 0, si;
    got = sscanf("3.25 -1e3 2.5e-1 0x1p-2 7", "%f %lf %Le %la%n %d", &sf, &sd, &sl, &sd2, &sn, &si);
    printf(T "sscanf floats %d: %g %g %Lg %g n=%d %d\n", got, (double)sf, sd, sl, sd2, sn, si);
    got = sscanf("12345.678", "%3f%lf", &sf, &sd);
    printf(T "sscanf widths %d: %g %g\n", got, (double)sf, sd);
    /* (not "1e+": C calls it a matching failure, as glibc and we do; the Mac
     * takes it as 1) */
    got = sscanf("-inf nan INFINITY", "%lf %lf %lf", &sd, &sd2, &inf);
    printf(T "sscanf special %d: %f %f %f\n", got, sd, sd2, inf);
    inf = 1.0 / 0.0;
    char sprinted[64];
    int sp = sprintf(sprinted, "%s-%05.1f-%x", "sp", 3.14159, 255);
    printf(T "sprintf %d '%s'\n", sp, sprinted);
    snprintf(path, sizeof(path), "%s/libctest-scan.txt", argv[1]);
    f = fopen(path, "w");
    fputs("  42 3.5 word 0x1Fp0\n-7 rest\n", f);
    fclose(f);
    f = fopen(path, "r");
    int fi1 = 0, fi2 = 0;
    double fd1 = 0, fd2 = 0;
    char fw[16] = "";
    got = fscanf(f, "%d %lf %15s %la", &fi1, &fd1, fw, &fd2);
    int got2 = fscanf(f, "%d", &fi2);
    fpos_t mark;
    fgetpos(f, &mark);
    char rest[16] = "";
    fscanf(f, "%15s", rest);
    fsetpos(f, &mark);
    char again[16] = "";
    fscanf(f, "%15s", again);
    printf(T "fscanf %d: %d %g %s %g, then %d: %d, rest '%s' again '%s', end %d\n", got, fi1, fd1,
           fw, fd2, got2, fi2, rest, again, fscanf(f, "%d", &fi2));
    fclose(f);
    FILE *lb = fopen(path, "w");
    setvbuf(lb, 0, _IOLBF, 0);
    fputs("ab", lb);
    stat(path, &ts);
    long size_before = (long)ts.st_size;
    fputs("c\n", lb);
    stat(path, &ts);
    printf(T "setvbuf line: %ld then %ld", size_before, (long)ts.st_size);
    setvbuf(lb, 0, _IONBF, 0);
    fputs("x", lb);
    stat(path, &ts);
    printf(", unbuffered %ld", (long)ts.st_size);
    setvbuf(lb, 0, _IOFBF, 0);
    fputs("y", lb);
    stat(path, &ts);
    printf(", full %ld", (long)ts.st_size);
    fclose(lb);
    stat(path, &ts);
    printf(", closed %ld, bad mode %d\n", (long)ts.st_size, setvbuf(stdout, 0, 99, 0) != 0);
    FILE *lk = fopen(path, "w+");                   /* (65e-53) a stream's lock */
    flockfile(lk);
    int nested = ftrylockfile(lk);
    putc_unlocked('a', lk);
    putc_unlocked('b', lk);
    funlockfile(lk);
    funlockfile(lk);
    fputs("c\n", lk);
    rewind(lk);
    flockfile(lk);
    int lc1 = getc_unlocked(lk), lc2 = getc_unlocked(lk), lc3 = getc(lk);
    funlockfile(lk);
    printf(T "flockfile: trylock while this thread holds it %d, read back %c%c%c, then %d\n", nested, lc1,
           lc2, lc3, ftrylockfile(lk));
    funlockfile(lk);
    fclose(lk);
    printf(T "putchar_unlocked: ");
    putchar_unlocked('o');
    putchar_unlocked('k');
    putchar_unlocked('\n');
    char path2[512];
    snprintf(path2, sizeof(path2), "%s/libctest-reopen.txt", argv[1]);
    FILE *ro = fopen(path, "w");
    fputs("old", ro);
    ro = freopen(path2, "w", ro);
    fputs("new 99\n", ro);
    fclose(ro);
    FILE *chk = fopen(path2, "r");
    char line2[32] = "";
    fgets(line2, sizeof(line2), chk);
    fclose(chk);
    line2[strcspn(line2, "\n")] = 0;
    stat(path, &ts);
    printf(T "freopen: first %ld bytes, second '%s'", (long)ts.st_size, line2);
    errno = ENOENT;
    fflush(stdout);
    perror("\nlibctest: perror");
    fflush(stderr);
    if (freopen(path2, "r", stdin)) {             /* scanf reads stdin: this file now */
        char sw[8] = "";
        int sv = 0;
        int sgot = scanf("%7s %d", sw, &sv);
        printf(T "scanf from stdin %d: '%s' %d\n", sgot, sw, sv);
    }
    remove(path);
    remove(path2);

    /* signal.h (22r) */
    {
    void (*was)(int) = signal(SIGUSR1, on_usr1);
    raise(SIGUSR1);
    raise(SIGUSR1);
    printf(T "signal: old default %d, handled %d times", was == SIG_DFL, (int)usr1_seen);
    signal(SIGUSR1, SIG_IGN);
    raise(SIGUSR1);
    void (*ign)(int) = signal(SIGUSR1, on_usr1);
    printf(", ignored then %d, old was ignore %d, SIG_ERR for SIGKILL %d\n", (int)usr1_seen,
           ign == SIG_IGN, signal(SIGKILL, on_usr1) == SIG_ERR);
    struct sigaction sa = { .sa_handler = on_usr2_outer }, old_sa;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR2, &sa, &old_sa);
    signal(SIGUSR1, on_nested);
    order[0] = 0;
    raise(SIGUSR2);
    printf(T "sigaction: old default %d, nested %s", old_sa.sa_handler == SIG_DFL, order);
    sigaddset(&sa.sa_mask, SIGUSR1);                /* now USR1 waits for the handler's end */
    sigaction(SIGUSR2, &sa, &old_sa);
    order[0] = 0;
    raise(SIGUSR2);
    printf(", masked %s", order);
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_NODEFER;
    sa.sa_handler = on_nested;
    sigaction(SIGUSR2, &sa, 0);
    order[0] = 0;
    raise(SIGUSR2);
    printf(", nodefer %s, old handler kept %d\n", order, old_sa.sa_handler == on_usr2_outer);
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    int m1 = sigismember(&set, SIGINT), m2 = sigismember(&set, SIGUSR1);
    sigdelset(&set, SIGINT);
    sigfillset(&set);
    printf(T "sigset: %d %d %d, filled %d\n", m1, m2, sigismember(&set, SIGINT) == 0 ? 0 : 1,
           sigismember(&set, SIGTERM));
    signal(SIGUSR1, SIG_DFL);
    signal(SIGUSR2, SIG_DFL);

    }

    /* time.h's timespecs (22r) */
    {
    struct timespec t0, t1, t2, cpu;
    int base = timespec_get(&t0, TIME_UTC);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    struct timespec nap = { 0, 60000000 }, left = { 9, 9 };
    int slept = nanosleep(&nap, &left);
    clock_gettime(CLOCK_MONOTONIC, &t2);
    long long gap = (long long)(t2.tv_sec - t1.tv_sec) * 1000000000LL + (t2.tv_nsec - t1.tv_nsec);
    struct timespec bad = { 0, 1000000000 };
    errno = 0;
    int refused = nanosleep(&bad, 0);
    int bad_errno = errno == EINVAL;
    printf(T "timespec_get %d, now %d, nanosleep %d slept enough %d, invalid %d %d, cputime %d, "
             "unknown clock %d\n", base == TIME_UTC, t0.tv_sec > 1700000000, slept, gap >= 60000000,
           refused, bad_errno, clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &cpu) == 0 && cpu.tv_nsec >= 0,
           clock_gettime((clockid_t)99, &cpu));
    }

    /* the rest of math.h (22s), to 15 digits (the Mac's libm is not always
     * the nearest double: mathtest checks every bit) */
    {
        static const double xs[] = { 0.5, -0.75, 1.25, 3.0, 10.5, 100.0, 1e-5, -2.5e-3 };
        for (unsigned i = 0; i < sizeof(xs) / sizeof(xs[0]); i++) {
            double x = xs[i], a = fabs(x);
            printf(T "math %g: %.15g %.15g %.15g %.15g %.15g %.15g %.15g %.15g %.15g\n", x, sin(x),
                   cos(x), tan(x), atan(x), sinh(x), cosh(x), tanh(x), asinh(x), cbrt(x));
            printf(T "math %g: %.15g %.15g %.15g %.15g %.15g %.15g %.15g %.15g\n", x, exp(x),
                   exp2(x), expm1(x), log(a), log2(a), log10(a), log1p(a), acosh(1 + a));
            printf(T "math %g: %.15g %.15g %.15g %.15g %.15g %.15g %.15g\n", x,
                   a <= 1 ? asin(x) : 0.0, a <= 1 ? acos(x) : 0.0, a < 1 ? atanh(x) : 0.0,
                   pow(a, 2.5), pow(x, 3.0), hypot(x, 2.0), atan2(x, -1.5));
        }
        int e;
        double whole;
        double m = frexp(-48.0, &e), frac = modf(-3.75, &whole);
        printf(T "math parts: frexp %g %d, ldexp %g %g, modf %g %g, scalbn %g, copysign %g %g\n",
               m, e, ldexp(0.75, 4), ldexp(1.0, -1074), frac, whole, scalbn(3.0, -2),
               copysign(2.0, -0.0), copysign(-2.0, 1.0));
        printf(T "math special: %g %g %g %g %g %g %g %g %g %g %g\n", pow(0.0, -1.0), pow(-8.0, 1.0 / 3),
               pow(-2.0, 3.0), pow(-1.0, 1.0 / 0.0), pow(2.0, -1075.0), log(0.0), log(-1.0),
               sqrt(-1.0), atan2(0.0, -0.0), atan2(-0.0, -1.0), exp(1000.0));
        printf(T "math edges: %g %g %g %g %g %g %g %g %.17g %.17g\n", fmin(1.0 / 0.0, 3.0),
               fmax(0.0 / 0.0, -1.0), fmod(-7.5, 2.0), fmod(1e300, 3.0), hypot(1e300, 1e300),
               sinh(800.0), tanh(-50.0), atanh(1.0), fmod(5.0, 0x1p-1060) / 0x1p-1074,
               cbrt(-27.0));
        printf(T "math float: %.6g %.6g %.6g %.6g %.6g %.6g %.6g %.6g %.6g\n", (double)sinf(0.5f),
               (double)cosf(2.0f), (double)expf(3.0f), (double)logf(10.0f), (double)powf(2.0f, 0.5f),
               (double)atan2f(1.0f, 1.0f), (double)tanhf(0.3f), (double)floorf(-1.5f),
               (double)hypotf(3.0f, 4.0f));
        printf(T "math classify: %d %d %d %d %d, signbit %d %d, isnormal %d %d\n",
               fpclassify(0.0) == FP_ZERO, fpclassify(1.0) == FP_NORMAL,
               fpclassify(0x1p-1074) == FP_SUBNORMAL, fpclassify(1.0 / 0.0) == FP_INFINITE,
               fpclassify(0.0 / 0.0) == FP_NAN, signbit(-0.0) != 0, signbit(1.0) != 0,
               isnormal(1.0) != 0, isnormal(0x1p-1074) != 0);
    }

    /* environment variables (22t) */
    {
        int had = getenv("LIBCTEST_A") != 0;
        int s1 = setenv("LIBCTEST_A", "one", 1);
        int s2 = setenv("LIBCTEST_A", "two", 0);    /* (not overwritten) */
        char a1[16];                                /* (a later setenv may free it) */
        snprintf(a1, sizeof(a1), "%s", getenv("LIBCTEST_A"));
        setenv("LIBCTEST_A", "three", 1);
        static char put[] = "LIBCTEST_B=bee";
        int p1 = putenv(put);
        int seen = 0;
        for (char **e = environ; *e; e++)
            seen += !strncmp(*e, "LIBCTEST_", 9);
        errno = 0;
        int bad = setenv("BAD=NAME", "x", 1), bad_errno = errno == EINVAL;
        printf(T "env: unset %d, setenv %d %d '%s' then '%s', putenv %d '%s', in environ %d, bad %d %d",
               had, s1, s2, a1, getenv("LIBCTEST_A"), p1, getenv("LIBCTEST_B"), seen, bad, bad_errno);
        unsetenv("LIBCTEST_B");
        printf(", unsetenv %d %d\n", getenv("LIBCTEST_B") == 0, getenv("LIBCTEST_A") != 0);
        setenv("LIBCTEST_CODE", "7", 1);
        fflush(stdout);
        int st = system("exit $LIBCTEST_CODE");     /* (the shell expands it) */
        printf(T "env: system passes it: exit %d\n", WIFEXITED(st) ? WEXITSTATUS(st) : -1);
        /* (41g-3; the Mac's build is libctest-host: the name's start) */
        printf(T "getprogname: libctest %d, no slash %d\n", !strncmp(getprogname(), "libctest", 8),
               strchr(getprogname(), '/') == 0);
        unsetenv("LIBCTEST_A");
        unsetenv("LIBCTEST_CODE");
    }

    /* wide characters (22v): strings, conversions, classes, formatting, streams */
    {
        wchar_t w[64], *wend;
        wcscpy(w, L"alpha");
        wcscat(w, L"-beta");
        wcsncat(w, L"-gammadelta", 6);
        wchar_t tok[] = L"  one, two,,three ", *place = 0, *t1 = wcstok(tok, L" ,", &place);
        wchar_t *t2 = wcstok(0, L" ,", &place), *t3 = wcstok(0, L" ,", &place);
        printf(T "wcs: '%ls' len %zu cmp %d %d %d ncmp %d chr %d rchr %d spn %zu cspn %zu pbrk '%ls' str '%ls' tok %ls %ls %ls %d\n",
               w, wcslen(w), sign(wcscmp(L"abc", L"abd")), sign(wcscmp(L"b", L"a")),
               wcscmp(L"same", L"same"), wcsncmp(L"abcx", L"abcy", 3),
               (int)(wcschr(w, L'-') - w), (int)(wcsrchr(w, L'-') - w), wcsspn(L"aabbc", L"ab"),
               wcscspn(L"hello, world", L",;"), wcspbrk(w, L"bg"), wcsstr(w, L"beta"), t1, t2, t3,
               wcstok(0, L" ,", &place) == 0);
        wchar_t m[8];
        wmemset(m, L'x', 8);
        wmemcpy(m, L"abc", 3);
        wmemmove(m + 1, m, 3);
        printf(T "wmem: %lc%lc%lc%lc%lc chr %d cmp %d ncpy %d%d\n", (wint_t)m[0], (wint_t)m[1],
               (wint_t)m[2], (wint_t)m[3], (wint_t)m[4], (int)(wmemchr(m, L'c', 8) - m),
               sign(wmemcmp(L"ab", L"ac", 2)), wcsncpy(m, L"z", 3)[1] == 0, m[2] == 0);

        long lv = wcstol(L"  -123abc", &wend, 10);
        printf(T "wcsto: %ld rest '%ls',", lv, wend);
        const wchar_t *dtext = L"1.5e3\x3b1";
        double dv = wcstod(dtext, &wend);
        printf(" %lld %lu %llu, %.17g rest at %d, %.9g %.17Lg\n", wcstoll(L"0x7fffffffffffffff", 0, 0),
               wcstoul(L"ff", 0, 16), wcstoull(L"18446744073709551615", 0, 10), dv,
               (int)(wend - dtext), (double)wcstof(L"0.1", 0), wcstold(L"2.5", 0));

        mbstate_t st;
        memset(&st, 0, sizeof(st));
        wchar_t wc = 0;
        char b[8];
        size_t r1 = mbrtowc(&wc, "\xe9z", 2, &st), r2 = mbrtowc(&wc, "", 1, &st);
        size_t r3 = mbrtowc(&wc, "a", 0, &st), r4 = wcrtomb(b, L'Q', &st);
        errno = 0;
        size_t r5 = wcrtomb(b, 0x3b1, &st);
        int e5 = errno == EILSEQ;
        printf(T "conv: mbrtowc %d %d %d, wcrtomb %d %c %d %d, mbsinit %d, btowc %x %d, wctob %d %d, mbrlen %d\n",
               (int)r1, (int)r2, (int)(r3 == (size_t)-2), (int)r4, b[0], (int)(r5 == (size_t)-1), e5,
               mbsinit(&st), (unsigned)btowc(0xe9), btowc(EOF) == WEOF, wctob(L'A'), wctob(0x3b1),
               (int)mbrlen("x", 1, &st));
        const char *src = "bytes\xe9";
        wchar_t wide[16];
        size_t n1 = mbsrtowcs(wide, &src, 16, &st);
        const wchar_t *wsrc = L"back";
        size_t n2 = wcsrtombs(b, &wsrc, sizeof(b), &st);
        printf(T "conv strings: %d '%.5ls' last %x src %d, %d '%s' src %d", (int)n1, wide,
               (unsigned)wide[5], src == 0, (int)n2, b, wsrc == 0);
        const wchar_t *badtext = L"ok\x3b1", *bad = badtext;
        errno = 0;
        size_t n3 = wcsrtombs(b, &bad, sizeof(b), &st);
        printf(", bad %d %d at %d\n", n3 == (size_t)-1, errno == EILSEQ, (int)(bad - badtext));

        int classes[12] = { 0 };
        for (wint_t ch = 0; ch < 300; ch++) {
            classes[0] += !!iswalnum(ch), classes[1] += !!iswalpha(ch), classes[2] += !!iswcntrl(ch);
            classes[3] += !!iswdigit(ch), classes[4] += !!iswgraph(ch), classes[5] += !!iswlower(ch);
            classes[6] += !!iswprint(ch), classes[7] += !!iswpunct(ch), classes[8] += !!iswspace(ch);
            classes[9] += !!iswupper(ch), classes[10] += !!iswxdigit(ch), classes[11] += !!iswblank(ch);
        }
        printf(T "wctype");
        for (int i = 0; i < 12; i++)
            printf(" %d", classes[i]);
        printf(" %lc%lc %x %d %d %d %lc %d\n", (wint_t)towupper(L'q'), (wint_t)towlower(L'Q'),
               (unsigned)towupper(0xe9), iswctype(L'7', wctype("digit")) != 0,
               iswctype(L'x', wctype("digit")) != 0, wctype("nonsense") == 0,
               (wint_t)towctrans(L'm', wctrans("toupper")), wctrans("nonsense") == 0);

        wchar_t out[64];
        int wn = 0;
        int sw = swprintf(out, 64, L"%d|%5.2f|%-4ls|%s|%lc%c|%x%n|%%", 42, 3.14159, L"ab", "cd",
                          (wint_t)0xe9, 'e', 255, &wn);
        printf(T "swprintf %d '", sw);
        for (int i = 0; out[i]; i++)
            printf(out[i] < 128 ? "%c" : "<%x>", (unsigned)out[i]);
        printf("' n=%d, too small %d '%ls', narrow %%ls %d, unencodable %d\n", wn,
               swprintf(out, 4, L"%s", "abcdef"), out, snprintf((char *)b, sizeof(b), "[%ls]", L"\xe9z"),
               snprintf((char *)b, sizeof(b), "[%ls]", L"\x3b1"));

        int x = 0;
        wchar_t ws[16], wc1 = 0;
        char nb[16];
        float fv = 0;
        int sc = swscanf(L"17 w\xe9rd \xff 2.5 bytes", L"%d %ls %lc %f %s", &x, ws, &wc1, &fv, nb);
        printf(T "swscanf %d: %d", sc, x);
        for (int i = 0; ws[i]; i++)
            printf(" %x", (unsigned)ws[i]);
        printf(" %x %g %s\n", (unsigned)wc1, (double)fv, nb);
        wchar_t lsw[8] = L"";
        wchar_t lcw = 0;
        int ns = sscanf("word z", "%ls %lc", lsw, &lcw);
        wchar_t set[8] = L"";
        int ss = swscanf(L"\xe9\xe9z", L"%l[\xea\xe9]", set);
        printf(T "sscanf %%ls %d '%ls' %lc, swscanf set %d %d\n", ns, lsw, (wint_t)lcw, ss,
               (int)wcslen(set));

        struct tm when = { .tm_year = 123, .tm_mon = 10, .tm_mday = 14, .tm_hour = 17,
                           .tm_min = 13, .tm_sec = 20, .tm_wday = 2, .tm_yday = 317 };
        wchar_t date[64];
        size_t dn = wcsftime(date, 64, L"%Y-%m-%d %H:%M:%S %A", &when);
        printf(T "wcsftime %d '%ls', small %d\n", (int)dn, date, (int)wcsftime(date, 4, L"%Y-%m", &when));

        char wpath[512];
        snprintf(wpath, sizeof(wpath), "%s/libctest-wide.txt", argv[1]);
        FILE *wf = fopen(wpath, "w");
        int o0 = fwide(wf, 0), o1 = fwide(wf, 1), o2 = fwide(wf, -1);
        fputwc(L'W', wf);
        fputws(L"ide\n", wf);
        int fw = fwprintf(wf, L"%d %ls\n", 99, L"wide line");
        errno = 0;
        wint_t badput = fputwc(0x3b1, wf);
        int badputerr = errno == EILSEQ;
        fclose(wf);
        FILE *rf = fopen(wpath, "r");
        wchar_t line[32];
        fgetws(line, 32, rf);
        int num = 0;
        wchar_t word2[16];
        int fs = fwscanf(rf, L"%d %ls", &num, word2);
        wint_t c1 = fgetwc(rf);
        wint_t un = ungetwc(L'Z', rf);
        wint_t c2 = fgetwc(rf);
        fgetwc(rf);
        wint_t last = fgetwc(rf);
        fclose(rf);
        remove(wpath);
        if (wcschr(line, L'\n'))
            *wcschr(line, L'\n') = 0;
        printf(T "wide stream: fwide %d %d %d, fwprintf %d, bad %d %d, fgetws '%ls' fwscanf %d %d '%ls', fgetwc %d ungetwc %lc %lc eof %d\n",
               o0, o1, o2, fw, badput == WEOF, badputerr, line, fs, num, word2, (int)c1, (wint_t)un,
               (wint_t)c2, last == WEOF);
    }

    /* the floating-point environment (22u): rounding directions, flags */
    {
        volatile double one = 1, three = 3, zero = 0, huge = 0x1p1023, tiny = 0x1p-1022;
        volatile double two = 2, minus = -1, third = 0x1.5555555555555p-2;
        static const int modes[] = { FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO };
        double got_d[4][4];
        float got_f[4];
        int set_ok = 1, read_back = 1;
        for (int i = 0; i < 4; i++) {
            set_ok &= fesetround(modes[i]) == 0;
            read_back &= fegetround() == modes[i];
            got_d[i][0] = one / three;
            got_d[i][1] = minus / three;
            got_d[i][2] = sqrt(two);
            got_d[i][3] = huge * two;
            got_f[i] = (float)third;                /* (converted in this direction) */
        }
        fesetround(FE_TONEAREST);
        printf(T "fenv: set %d read back %d, bad direction refused %d\n", set_ok, read_back,
               fesetround(12345) != 0);
        for (int i = 0; i < 4; i++)
            printf(T "fenv: direction %d: %a %a %a %a %a\n", i, got_d[i][0], got_d[i][1],
                   got_d[i][2], got_d[i][3], (double)got_f[i]);

        static const struct { const char *name; int bit; } flag[] = {
            { "invalid", FE_INVALID }, { "divbyzero", FE_DIVBYZERO }, { "overflow", FE_OVERFLOW },
            { "underflow", FE_UNDERFLOW }, { "inexact", FE_INEXACT },
        };
        volatile double r;
        for (int op = 0; op < 6; op++) {
            feclearexcept(FE_ALL_EXCEPT);
            switch (op) {
            case 0: r = one + one; break;
            case 1: r = one / three; break;
            case 2: r = one / zero; break;
            case 3: r = zero / zero; break;
            case 4: r = huge * two; break;
            case 5: r = tiny * tiny; break;
            }
            int raised = fetestexcept(FE_ALL_EXCEPT);
            printf(T "fenv: operation %d raised", op);
            for (int k = 0; k < 5; k++)
                if (raised & flag[k].bit)
                    printf(" %s", flag[k].name);
            printf("\n");
        }
        (void)r;

        feclearexcept(FE_ALL_EXCEPT);
        feraiseexcept(FE_OVERFLOW | FE_INEXACT);
        fexcept_t saved;
        fegetexceptflag(&saved, FE_ALL_EXCEPT);
        int after_raise = fetestexcept(FE_ALL_EXCEPT) == (FE_OVERFLOW | FE_INEXACT);
        feclearexcept(FE_INEXACT);
        int after_clear = fetestexcept(FE_ALL_EXCEPT) == FE_OVERFLOW;
        fesetexceptflag(&saved, FE_INEXACT);
        int after_set = fetestexcept(FE_ALL_EXCEPT) == (FE_OVERFLOW | FE_INEXACT);

        fenv_t env;
        fesetround(FE_UPWARD);
        feholdexcept(&env);                     /* (saves upward and the two flags) */
        int held = fetestexcept(FE_ALL_EXCEPT) == 0 && fegetround() == FE_UPWARD;
        fesetround(FE_DOWNWARD);
        r = zero / zero;                        /* invalid, meanwhile */
        feupdateenv(&env);
        int updated = fegetround() == FE_UPWARD &&
                      fetestexcept(FE_ALL_EXCEPT) == (FE_OVERFLOW | FE_INEXACT | FE_INVALID);
        fesetenv(FE_DFL_ENV);
        int dfl = fegetround() == FE_TONEAREST && fetestexcept(FE_ALL_EXCEPT) == 0;
        fesetround(FE_TOWARDZERO);
        fegetenv(&env);
        fesetround(FE_TONEAREST);
        fesetenv(&env);
        int restored = fegetround() == FE_TOWARDZERO;
        fesetenv(FE_DFL_ENV);
        printf(T "fenv: raise %d clear %d set %d, hold %d update %d, default %d, env %d\n",
               after_raise, after_clear, after_set, held, updated, dfl, restored);

        /* (22z) rounding in each direction; nearbyint raises no inexact */
        static const double to_round[] = { 2.5, -2.5, 3.5, -0.4, 0.6, -1.5, 1e300, -0.0 };
        for (int i = 0; i < 4; i++) {
            fesetround(modes[i]);
            double got[8];
            long longs[8];
            for (int k = 0; k < 8; k++) {
                got[k] = rint(to_round[k]);
                longs[k] = k < 6 ? lrint(to_round[k]) : 0;
            }
            volatile double half = 2.5, third;
            feclearexcept(FE_ALL_EXCEPT);
            third = nearbyint(half);
            int quiet = fetestexcept(FE_INEXACT) == 0;
            fesetround(FE_TONEAREST);
            printf(T "rint: direction %d:", i);
            for (int k = 0; k < 8; k++)
                printf(" %g", got[k]);
            for (int k = 0; k < 6; k++)
                printf(" %ld", longs[k]);
            printf(" nearbyint quiet %d %g\n", quiet, third);
        }
    }

    /* (22z) the rest of math.h */
    {
        int q1, q2;
        double r1 = remquo(29, 3, &q1), r2 = remquo(-7.5, 2, &q2);
        printf(T "math exact: remainder %a %a %a %a, remquo %a %d %a %d, fdim %g %g\n",
               remainder(3, 2), remainder(5, 2), remainder(-1e300, 3), remainder(0x1p-1070, 0x1p-1073),
               r1, q1, r2, q2, fdim(5, 3), fdim(3, 5));
        printf(T "math exact: fma %a %a %a %a, fmaf %a\n", fma(0x1.0000000000001p0, 0x1.0000000000001p0, -1),
               fma(0.1, 10, -1), fma(1e308, 10, -1e308), fma(0x1p-1000, 0x1p-74, 0x1p-1074),
               (double)fmaf(0x1.000002p0f, 0x1.000002p0f, -1.0f));
        printf(T "math exact: nextafter %a %a %a %a, nexttoward %a, nextafterf %a\n", nextafter(1, 2),
               nextafter(1, 0), nextafter(0, -1), nextafter(0x1.fffffffffffffp1023, 1.0 / 0.0),
               nexttoward(1, 2.0L), (double)nextafterf(1.0f, 0.0f));
        printf(T "math exact: ilogb %d %d %d %d, logb %g %g %g, scalbln %g, lround %ld %ld %lld, nan %d\n",
               ilogb(1024), ilogb(0x1p-1074), ilogb(0.0) == FP_ILOGB0, ilogb(3.9), logb(1024), logb(0.1),
               logb(0.0), scalbln(3, 4L), lround(2.5), lround(-2.5), llround(-1e15 - 0.5), isnan(nan("")) != 0);
        printf(T "math compare: %d %d %d %d %d %d %d %d\n", isgreater(2.0, 1.0), isgreaterequal(1.0, 1.0),
               isless(1.0, 0.0 / 0.0), islessequal(-0.0, 0.0), islessgreater(1.0, 2.0),
               islessgreater(1.0, 1.0), isunordered(0.0 / 0.0, 1.0), isunordered(1.0, 2.0));
        printf(T "math special: erf %.15g %.15g %.15g erfc %.15g %.15g %.15g\n", erf(0.5), erf(-2), erf(1e-10),
               erfc(0.5), erfc(5), erfc(26));
        printf(T "math special: tgamma %.15g %.15g %.15g %.15g lgamma %.15g %.15g %.15g %.15g\n", tgamma(5),
               tgamma(0.5), tgamma(-2.5), tgamma(170.5), lgamma(1.0000001), lgamma(100), lgamma(-2.5),
               lgamma(0.001));
        printf(T "math special: tgamma %g %g %g lgamma %g %g, float %.6g %.6g %.6g\n", tgamma(0.0), tgamma(-0.0),
               tgamma(1.0 / 0.0), lgamma(0.0), lgamma(-3.0), (double)erff(0.5f), (double)tgammaf(4.5f),
               (double)lgammaf(10.0f));
    }

    /* system, stderr */
    int status = system("exit 3");
    printf(T "system %d exit %d\n", system(0) != 0, WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    fprintf(stdout, T "to stdout %d\n", 7);
    fflush(stdout);
    fprintf(stderr, T "to stderr\n");
    printf(T "done\n");
    return 0;
}
