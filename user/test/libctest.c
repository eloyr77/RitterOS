/*
 * Test: the C library for ports (22m). Plain standard C, so the test builds
 * it on the Mac as well and compares the outputs line by line: the Mac's C
 * library is the reference. libctest FOLDER (a folder it may write a file in).
 */
#include <assert.h>
#include <ctype.h>
#include <fnmatch.h>
#include <getopt.h>
#include <glob.h>
#include <iconv.h>
#include <langinfo.h>
#ifdef __APPLE__
#include <xlocale.h>                     /* (the Mac keeps the *_l calls there) */
#endif
#ifndef __APPLE__
#include <libintl.h>
#endif
#include <grp.h>
#include <libgen.h>
#include <limits.h>
#include <pwd.h>
#include <search.h>
#include <spawn.h>
#include <sys/statvfs.h>
#include <sys/uio.h>
#include <sys/file.h>
#ifndef __APPLE__
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <net/if.h>
#include <sys/auxv.h>
#endif
#include <sys/mman.h>
#include <sys/socket.h>
#include <pthread.h>
#include <sched.h>
#include <sys/select.h>
#include <dlfcn.h>
#include <semaphore.h>
#ifndef __APPLE__
#include <link.h>
#endif
#include <sys/resource.h>
#include <sys/times.h>
#include <syslog.h>
#ifdef __APPLE__
#include <sys/random.h>                  /* (the Mac keeps getentropy there) */
#endif
#ifndef __APPLE__
#include <sys/auxv.h>
#include <sys/random.h>
#include <sys/sysinfo.h>
#endif
#include <sys/ioctl.h>
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

/* (43a) The library's additions for ported code: what the Mac has too is
 * compared line by line; what only GNU has (or works otherwise on the Mac)
 * prints "libctest-own:" lines that the test expects as they are. */
static int by_int(const void *a, const void *b)
{
    return *(const int *)a - *(const int *)b;
}

static int walked[16], walked_count;

static void walk_action(const void *node, VISIT visit, int depth)
{
    (void)depth;
    if ((visit == postorder || visit == leaf) && walked_count < 16)
        walked[walked_count++] = **(int *const *)node;
}

#ifndef __APPLE__
static int by_int_r(const void *a, const void *b, void *arg)
{
    return (*(const int *)a - *(const int *)b) * *(int *)arg;
}
#endif

static void ported(const char *folder)
{
    /* strings */
    char buf[32], *save = 0, line[] = "a,b,,c";
    size_t lc = strlcpy(buf, "hello world", 6);
    printf(T "43a strlcpy %zu '%s'", lc, buf);
    lc = strlcat(buf, "XYZ", 8);
    printf(" strlcat %zu '%s'\n", lc, buf);
    char *p = line, *f, fields[64] = "";
    while ((f = strsep(&p, ",")))
        strcat(fields, "["), strcat(fields, f), strcat(fields, "]");
    char toks[] = " one  two three ";
    printf(T "43a strsep %s strtok_r %s", fields, strtok_r(toks, " ", &save));
    printf(" %s", strtok_r(0, " ", &save));
    printf(" %s", strtok_r(0, " ", &save));
    printf(" %d\n", strtok_r(0, " ", &save) == 0);
    const char *hay = "abcabcd";
    int sp = (int)(stpncpy(buf, "ab", 5) - buf);
    printf(T "43a memmem %d %d strcasestr %s stpncpy %d\n", (int)((const char *)memmem(hay, 7, "abcd", 4) - hay),
           memmem("abc", 3, "x", 1) == 0, strcasestr("Hello World", "WORLD"), sp);
    char err[8];
    int er1 = strerror_r(ENOENT, err, sizeof(err)), er2 = strerror_r(ENOENT, buf, sizeof(buf));
    printf(T "43a strerror_r %d %d isascii %d %d toascii %d\n", er1, er2, isascii(65), isascii(200), toascii(200));
    printf(T "45b strerror(EAGAIN) %s; EMSGSIZE %s; EPROTOTYPE %s\n", strerror(EAGAIN), strerror(EMSGSIZE),
           strerror(EPROTOTYPE));
    printf(T "43a imaxabs %jd strtoimax %jd strtoumax %ju ffs %d %d %d\n", imaxabs(-INTMAX_MAX),
           strtoimax("-123x", 0, 10), strtoumax("0x10", 0, 16), ffs(0), ffs(12), ffsll(1LL << 40));
    wchar_t *wd = wcsdup(L"Wide");
    printf(T "43a wcsdup %ls wcsnlen %zu wcscasecmp %d %d\n", wd, wcsnlen(wd, 2),
           wcscasecmp(L"ABC", L"abc"), wcsncasecmp(L"abX", L"ABy", 2));
    free(wd);
#ifndef __APPLE__
    const char *abc = "abc";
    int mp = (int)((char *)mempcpy(buf, "abc", 3) - buf);
    printf("libctest-own: 43a strchrnul %d mempcpy %d strverscmp %d %d %d %d %d\n",
           (int)(strchrnul(abc, 'x') - abc), mp,
           strverscmp("a9", "a10") < 0, strverscmp("000", "00") < 0, strverscmp("01", "010") < 0,
           strverscmp("09", "0") < 0, strverscmp("item10", "item10") == 0);
#endif

    /* numbers */
    srandom(1);
    long r1 = random();
    long r2 = random();
    srand48(7);
    double d1 = drand48();
    long l1 = lrand48();
    long m1 = mrand48();
    unsigned short x[3] = { 1, 2, 3 };
    double e1 = erand48(x);
    long n1x = nrand48(x);
    printf(T "43a random %ld %ld drand48 %.10f lrand48 %ld mrand48 %ld erand48 %.10f nrand48 %ld\n",
           r1, r2, d1, l1, m1, e1, n1x);
    char state[64];
    char *old = initstate(5, state, sizeof(state));
    long r3 = random();
    setstate(old);
    printf(T "43a initstate %ld setstate %d\n", r3, old != 0);
#ifndef __APPLE__
    unsigned seed = 1;
    int rr = rand_r(&seed);
    int nums[] = { 5, 3, 9, 1 }, down = -1;
    qsort_r(nums, 4, sizeof(int), by_int_r, &down);
    printf("libctest-own: 43a rand_r %d qsort_r %d %d %d %d\n", rr, nums[0], nums[1], nums[2], nums[3]);
#endif

    /* memory */
    void *aligned = 0;
    int pm = posix_memalign(&aligned, 64, 100);
    printf(T "43a posix_memalign %d %d einval %d\n", pm, ((uintptr_t)aligned & 63) == 0,
           posix_memalign(&aligned, 3, 10) == EINVAL);
    free(aligned);
#ifndef __APPLE__
    char *ra = reallocarray(0, 10, 10);
    printf("libctest-own: 43a reallocarray %d overflow %d\n", ra != 0,
           reallocarray(0, (size_t)-1 / 2, 3) == 0 && errno == ENOMEM);
    free(ra);
#endif

    /* streams */
    char *mem = 0;
    size_t size = 0;
    FILE *ms = open_memstream(&mem, &size);
    fprintf(ms, "memstream %d", 42);
    fflush(ms);
    printf(T "43a open_memstream '%s' %zu", mem, size);
    fprintf(ms, " more");
    fclose(ms);
    printf(" '%s' %zu\n", mem, size);
    free(mem);
    char fixed[16] = "one\ntwo\nthree";
    FILE *fm = fmemopen(fixed, strlen(fixed), "r");
    char *l = 0;
    size_t lsize = 0;
    ssize_t n1 = getline(&l, &lsize, fm);
    ssize_t n2 = getdelim(&l, &lsize, 'e', fm);
    printf(T "43a fmemopen getline %zd getdelim %zd", n1, n2);
    for (char *c = l; *c; c++)
        printf(" %d", *c);
    getline(&l, &lsize, fm);
    printf(" end %zd\n", getline(&l, &lsize, fm));
    fclose(fm);
    free(l);
    char *as = 0;
    int an = asprintf(&as, "%s-%04d", "asprintf", 7);
    printf(T "43a asprintf %d %s\n", an, as);
    free(as);

    /* searching */
    int keys[] = { 50, 20, 70, 10, 30, 60, 80 }, *k;
    void *root = 0;
    for (int i = 0; i < 7; i++)
        tsearch(&keys[i], &root, by_int);
    int forty = 40, thirty = 30;
    walked_count = 0;
    twalk(root, walk_action);
    printf(T "43a tsearch walk");
    for (int i = 0; i < walked_count; i++)
        printf(" %d", walked[i]);
    k = tfind(&thirty, &root, by_int) ? *(int **)tfind(&thirty, &root, by_int) : 0;
    printf(" tfind %d %d", k ? *k : -1, tfind(&forty, &root, by_int) == 0);
    tdelete(&thirty, &root, by_int);
    printf(" deleted %d\n", tfind(&thirty, &root, by_int) == 0);
    int table[8] = { 4, 8, 15 }, key = 16;
    size_t count = 3;
    lsearch(&key, table, &count, sizeof(int), by_int);
    printf(T "43a lsearch %zu %d lfind %d\n", count, table[3],
           *(int *)lfind(&table[1], table, &count, sizeof(int), by_int));
    hcreate(10);
    ENTRY e = { strdup("apple"), (void *)1 }, *found;
    hsearch(e, ENTER);
    e.key = strdup("pear"), e.data = (void *)2;
    hsearch(e, ENTER);
    char pear[] = "pear", plum[] = "plum";
    e.key = pear;
    found = hsearch(e, FIND);
    e.key = plum;
    printf(T "43a hsearch %ld %d\n", (long)(intptr_t)found->data, hsearch(e, FIND) == 0);
    hdestroy();

    /* patterns, paths, options */
    printf(T "43a fnmatch %d %d %d %d %d %d %d\n", fnmatch("*.c", "main.c", 0), fnmatch("*.c", "main.h", 0),
           fnmatch("a/*", "a/b/c", FNM_PATHNAME), fnmatch("*", ".hidden", FNM_PERIOD),
           fnmatch("[a-c]x[!0-9]", "bxz", 0), fnmatch("[[:digit:]]?", "7q", 0), fnmatch("\\*", "*", 0));
    char dir[512], name[600];
    snprintf(dir, sizeof(dir), "%s/globdir", folder);
    mkdir(dir, 0755);
    const char *files[] = { "b.txt", "a.txt", "c.dat", ".dot.txt" };
    for (int i = 0; i < 4; i++) {
        snprintf(name, sizeof(name), "%s/%s", dir, files[i]);
        FILE *g = fopen(name, "w");
        fclose(g);
    }
    glob_t gl;
    snprintf(name, sizeof(name), "%s/*.txt", dir);
    int gr = glob(name, 0, 0, &gl);
    printf(T "43a glob %d %zu", gr, gl.gl_pathc);
    for (size_t i = 0; i < gl.gl_pathc; i++)
        printf(" %s", strrchr(gl.gl_pathv[i], '/') + 1);
    globfree(&gl);
    snprintf(name, sizeof(name), "%s/*.none", dir);
    printf(" none %d\n", glob(name, 0, 0, &gl) == GLOB_NOMATCH);
    char real[PATH_MAX], base[PATH_MAX];
    realpath(folder, base);
    snprintf(name, sizeof(name), "%s/./globdir/../globdir/a.txt", folder);
    char *rp = realpath(name, real);
    printf(T "43a realpath %s missing %d\n", rp ? real + strlen(base) : "(none)",
           realpath("/no/such/thing", real) == 0 && errno == ENOENT);
    char target[PATH_MAX];                      /* (43e) a link to a file, absolute and relative */
    snprintf(name, sizeof(name), "%s/globdir/link-abs", folder);
    snprintf(target, sizeof(target), "%s/globdir/a.txt", base);
    int k1 = symlink(target, name);
    rp = realpath(name, real);
    printf(T "43e realpath of a link to a file %d %s", k1, rp ? real + strlen(base) : "(none)");
    snprintf(name, sizeof(name), "%s/globdir/link-rel", folder);
    int k2 = symlink("c.dat", name);
    rp = realpath(name, real);
    printf(" relative %d %s\n", k2, rp ? real + strlen(base) : "(none)");
    char b1[] = "/usr/lib/", b2[] = "/usr/lib/", b3[] = "file", b4[] = "file";
    printf(T "43a basename %s", basename(b1));
    printf(" dirname %s", dirname(b2));
    printf(" basename %s", basename(b3));
    printf(" dirname %s\n", dirname(b4));
    snprintf(name, sizeof(name), "%s/globdir/XXXXXXsuf", folder);
    int fd = mkstemps(name, 3);
    snprintf(dir, sizeof(dir), "%s/dirXXXXXX", folder);
    printf(T "43a mkstemps %d %s mkdtemp %d access %d %d %d\n", fd >= 0, name + strlen(name) - 3,
           mkdtemp(dir) != 0, access(name, R_OK | W_OK), access(name, X_OK) == -1 && errno == EACCES,
           access("/no/such", F_OK) == -1 && errno == ENOENT);
    char *args[] = { "prog", "-a", "-b", "value", "-cx", "--long", "--name=n1", "--", "-z", 0 };
    struct option longs[] = { { "long", no_argument, 0, 'L' }, { "name", required_argument, 0, 'N' }, { 0, 0, 0, 0 } };
    printf(T "43a getopt_long");
    int o;
    while ((o = getopt_long(9, args, "ab:c:", longs, 0)) != -1)
        printf(" %c%s%s", o, optarg ? "=" : "", optarg ? optarg : "");
    printf(" optind %d %s\n", optind, args[optind]);

    /* time */
    struct tm tm;
    memset(&tm, 0, sizeof(tm));
    char *rest = strptime("2026-11-11 14:05:09 Wednesday rest", "%Y-%m-%d %H:%M:%S %A", &tm);
    printf(T "43a strptime %d-%d-%d %d:%d:%d wday %d rest '%s' timegm %lld\n", tm.tm_year, tm.tm_mon,
           tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, tm.tm_wday, rest, (long long)timegm(&tm));
    memset(&tm, 0, sizeof(tm));
    strptime("Jan 5 99 3:04:05 PM", "%b %d %y %I:%M:%S %p", &tm);
    char at[32];
    time_t t0 = 86400 * 365;
    printf(T "43a strptime %d %d %d asctime_r %s", tm.tm_year, tm.tm_mon, tm.tm_hour, asctime_r(&tm, at));
    printf(T "43a ctime_r %s", ctime_r(&t0, at));
    struct timespec res;
    printf(T "43a clock_getres %d\n", clock_getres(CLOCK_MONOTONIC, &res));

#ifndef __APPLE__
    struct stat st;
    snprintf(name, sizeof(name), "%s/globdir/big", folder);
    FILE *big = fopen(name, "w");
    for (int i = 0; i < 5000; i++)
        fputc('x', big);
    fclose(big);
    stat(name, &st);
    printf("libctest-own: 43a stat %lld blocks %lld blksize %lld recent %d nsec %d\n", (long long)st.st_size,
           (long long)st.st_blocks, (long long)st.st_blksize, st.st_mtime > 1700000000 && st.st_atime == st.st_mtime,
           st.st_mtim.tv_nsec >= 0 && st.st_mtim.tv_nsec < 1000000000);
    struct passwd *pw = getpwnam("root");
    struct group *gp = getgrgid(0);
    printf("libctest-own: 43a getpwnam %s %s %u %u %s %s %s getgrgid %s %s\n", pw->pw_name, pw->pw_passwd,
           pw->pw_uid, pw->pw_gid, pw->pw_gecos, pw->pw_dir, pw->pw_shell, gp->gr_name, gp->gr_mem[0]);
    char pwbuf[16];
    struct passwd pwd, *pr;
    printf("libctest-own: 43a getpwuid_r %d %d\n", getpwuid_r(0, &pwd, pwbuf, 4, &pr), pr == 0);
#endif
}

/* (43a-2) Locales: C.UTF-8's characters, classes and widths, a thread's
 * own locale, and iconv; compared with the Mac where it does the same. */
static void locales(void)
{
    printf(T "43a-2 setlocale %s", setlocale(LC_CTYPE, "en_US.UTF-8"));
    printf(" MB_CUR_MAX %d codeset %s\n", (int)MB_CUR_MAX, nl_langinfo(CODESET));
    const char *text = "a\xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80";       /* a, e acute, euro, a face */
    mbstate_t st;
    memset(&st, 0, sizeof(st));
    printf(T "43a-2 mbrtowc");
    for (const char *s = text; *s;) {
        wchar_t wc;
        size_t n = mbrtowc(&wc, s, strlen(s), &st);
        printf(" %x/%zu", (unsigned)wc, n);
        s += n;
    }
    size_t part = mbrtowc(0, "\xe2\x82", 2, &st);
    wchar_t rest;
    size_t done = mbrtowc(&rest, "\xac", 1, &st);
    printf(" split %ld %zu %x\n", (long)part, done, (unsigned)rest);
    const char *bad[] = { "\xc0\x80", "\xed\xa0\x80", "\xf4\x90\x80\x80", "\xe0\x80\x80", "\x80" };
    printf(T "43a-2 refused");
    for (int i = 0; i < 5; i++) {
        memset(&st, 0, sizeof(st));
        errno = 0;
        size_t r = mbrtowc(0, bad[i], strlen(bad[i]), &st);
        printf(" %ld/%d", (long)r, errno == EILSEQ);
    }
    char out[16];
    memset(&st, 0, sizeof(st));
    size_t w1 = wcrtomb(out, 0x20AC, &st);
    printf(" wcrtomb %zu %02x%02x%02x", w1, (unsigned char)out[0], (unsigned char)out[1], (unsigned char)out[2]);
    errno = 0;
    printf(" surrogate %ld/%d\n", (long)wcrtomb(out, 0xD800, &st), errno == EILSEQ);
    wchar_t wide[8];
    size_t mw = mbstowcs(wide, text, 8);
    char back[32];
    size_t wm = wcstombs(back, wide, sizeof(back));
    printf(T "43a-2 mbstowcs %zu wcstombs %zu same %d mblen %d %d btowc %d wctob %d\n", mw, wm,
           strcmp(back, text) == 0, mblen("\xe2\x82\xac", 3), mblen("\xe2\x82", 2), btowc(0xe9) == WEOF,
           wctob(0xe9) == EOF);
    printf(T "43a-2 classes alpha %d %d upper %d lower %d towupper %x %x towlower %x space %d punct %d"
           " print %d\n", iswalpha(0xe9), iswalpha(0x4e2d), iswupper(0xc9), iswlower(0x3c3),
           towupper(0xe9), towupper(0x3c3), towlower(0x3a3), iswspace(0x3000), iswpunct(0xbf), iswprint(0x4e2d));
#ifndef __APPLE__
    /* (glibc's C.UTF-8: a no-break space is no space, the line separator
     * a control; the Mac says the other way for both) */
    printf("libctest-own: 43a-2 nbsp %d separator %d\n", iswspace(0xa0), iswcntrl(0x2028));
#endif
    printf(T "43a-2 wcwidth %d %d %d %d %d wcswidth %d\n", wcwidth(L'a'), wcwidth(0xe9), wcwidth(0x4e2d),
           wcwidth(0x301), wcwidth(0x1f600), wcswidth(L"a\x4e2d\x301", 3));
    printf(T "43a-2 printf %ls %lc|%5ls|\n", L"\xe9t\xe9", (wint_t)0x20ac, L"\x4e2d");
    locale_t utf8 = newlocale(LC_CTYPE_MASK, "en_US.UTF-8", 0), c = newlocale(LC_ALL_MASK, "C", 0);
    printf(T "43a-2 _l alpha %d %d upper %x %x", iswalpha_l(0xe9, utf8), iswalpha_l(0xe9, c),
           towupper_l(0xe9, utf8), towupper_l(0xe9, c));
    setlocale(LC_CTYPE, "C");
    locale_t before = uselocale(utf8);
    printf(" uselocale %d %d", (int)MB_CUR_MAX, iswalpha(0xe9));
    uselocale(before);
    printf(" back %d %d\n", (int)MB_CUR_MAX, iswalpha(0xe9));
    freelocale(utf8);
    freelocale(c);
    printf(T "43a-2 C locale alpha %d width %d wcrtomb %ld\n", iswalpha(0xe9), wcwidth(0xe9),
           (long)wcrtomb(out, 0x20ac, 0));

    iconv_t cd = iconv_open("UTF-16LE", "UTF-8");
    char in[] = "A\xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80", *ip = in, *op = out;
    size_t il = strlen(in), ol = sizeof(out);
    size_t r = iconv(cd, &ip, &il, &op, &ol);
    printf(T "43a-2 iconv %ld left %zu:", (long)r, il);
    for (char *q = out; q < op; q++)
        printf(" %02x", (unsigned char)*q);
    iconv_close(cd);
    cd = iconv_open("ISO-8859-1", "UTF-8");
    ip = in, il = 3, op = out, ol = sizeof(out);
    r = iconv(cd, &ip, &il, &op, &ol);
    printf(" | latin1 %ld %02x %02x", (long)r, (unsigned char)out[0], (unsigned char)out[1]);
    ip = in + 3, il = 3, op = out, ol = sizeof(out);
    errno = 0;
    r = iconv(cd, &ip, &il, &op, &ol);
    printf(" euro %ld/%d left %zu", (long)r, errno == EILSEQ, il);
    iconv_close(cd);
    cd = iconv_open("UTF-8", "CP1252");
    char in2[] = "\x80\x93x\x94", *ip2 = in2;
    il = 4, op = out, ol = sizeof(out);
    iconv(cd, &ip2, &il, &op, &ol);
    printf(" | cp1252");
    for (char *q = out; q < op; q++)
        printf(" %02x", (unsigned char)*q);
    iconv_close(cd);
    cd = iconv_open("UTF-8", "UTF-16LE");
    char odd[] = "A\0B", *ip3 = odd;
    il = 3, op = out, ol = sizeof(out);
    errno = 0;
    r = iconv(cd, &ip3, &il, &op, &ol);
    printf(" | incomplete %ld/%d left %zu", (long)r, errno == EINVAL, il);
    ip3 = odd, il = 2, op = out, ol = 0;
    errno = 0;
    r = iconv(cd, &ip3, &il, &op, &ol);
    printf(" full %ld/%d", (long)r, errno == E2BIG);
    iconv_close(cd);
    printf(" unknown %d\n", iconv_open("UTF-8", "KLINGON") == (iconv_t)-1 && errno == EINVAL);
#ifndef __APPLE__
    cd = iconv_open("UTF-16", "UTF-8");
    ip = in, il = 1, op = out, ol = sizeof(out);
    iconv(cd, &ip, &il, &op, &ol);
    printf("libctest-own: 43a-2 utf16 mark %02x %02x %02x %02x", (unsigned char)out[0], (unsigned char)out[1],
           (unsigned char)out[2], (unsigned char)out[3]);
    iconv_close(cd);
    cd = iconv_open("ASCII//TRANSLIT", "UTF-8");
    ip = in, il = 3, op = out, ol = sizeof(out);
    r = iconv(cd, &ip, &il, &op, &ol);
    printf(" translit %ld %.*s", (long)r, (int)(op - out), out);
    iconv_close(cd);
    printf(" codeset %s gettext %s %s %s\n", nl_langinfo(CODESET), gettext("hello"),
           ngettext("one file", "files", 1), ngettext("one file", "files", 2));
#endif
}

/* (43a-3) The system and processes. */
static volatile sig_atomic_t alarms;

static void on_alarm(int sig)
{
    (void)sig;
    alarms++;
}

static void system_calls(const char *folder)
{
    signal(SIGALRM, on_alarm);
    unsigned left5 = (alarm(5), alarm(0));
    ualarm(50000, 0);
    int paused = pause();
    printf(T "43a-3 alarm left %u ualarm handled %d pause %d EINTR %d\n", left5, (int)alarms, paused, errno == EINTR);
    signal(SIGALRM, SIG_DFL);
    unsigned char rnd[16];
    printf(T "43a-3 getentropy %d too many %d\n", getentropy(rnd, sizeof(rnd)), getentropy(rnd, 300) == -1);
    int fds[2];
    pipe(fds);
    errno = 0;
    char tty[32];
    printf(T "43a-3 ttyname pipe %d ENOTTY %d ttyname_r %d\n", ttyname(fds[0]) != 0, errno == ENOTTY,
           ttyname_r(fds[0], tty, sizeof(tty)) == ENOTTY);
    /* (46b) ioctl's FIONBIO and FIOCLEX/FIONCLEX on a pipe, a closed descriptor */
    int on = 1, off = 0;
    int nb_on = ioctl(fds[0], FIONBIO, &on), nb_flag = (fcntl(fds[0], F_GETFL) & O_NONBLOCK) != 0;
    int nb_off = ioctl(fds[0], FIONBIO, &off), nb_cleared = (fcntl(fds[0], F_GETFL) & O_NONBLOCK) == 0;
    int cx = ioctl(fds[1], FIOCLEX), cx_flag = (fcntl(fds[1], F_GETFD) & FD_CLOEXEC) != 0;
    int ncx = ioctl(fds[1], FIONCLEX), ncx_flag = (fcntl(fds[1], F_GETFD) & FD_CLOEXEC) == 0;
    errno = 0;
    int bad = ioctl(99, FIONBIO, &on);
    printf(T "46b ioctl FIONBIO %d %d %d %d FIOCLEX %d %d FIONCLEX %d %d closed %d EBADF %d\n", nb_on, nb_flag,
           nb_off, nb_cleared, cx, cx_flag, ncx, ncx_flag, bad, errno == EBADF);
    close(fds[0]);
    close(fds[1]);
    pid_t child = fork();
    if (child == 0)
        _exit(3);
    int status = 0;
    struct rusage use;
    pid_t got = wait4(child, &status, 0, &use);
    printf(T "43a-3 wait4 %d exit %d\n", got == child, WEXITSTATUS(status));
    char marker[512];
    snprintf(marker, sizeof(marker), "%s/lctmarker", folder);
    FILE *m = fopen(marker, "w");
    fclose(m);
    posix_spawn_file_actions_t acts;
    posix_spawn_file_actions_init(&acts);
    int added = posix_spawn_file_actions_addchdir_np(&acts, folder);
    char *args[] = { "sh", "-c", "test -f lctmarker", 0 };
    pid_t sp;
    int r = posix_spawnp(&sp, "sh", &acts, 0, args, environ);
    waitpid(sp, &status, 0);
    printf(T "43a-3 addchdir %d spawn %d status %d\n", added, r, WEXITSTATUS(status));
    posix_spawn_file_actions_destroy(&acts);
    struct rlimit rl;
    printf(T "43a-3 getrlimit %d setrlimit lower %d", getrlimit(RLIMIT_NOFILE, &rl), setrlimit(RLIMIT_NOFILE, &rl));
    struct tms tm;
    printf(" times %d\n", times(&tm) != (clock_t)-1);
    openlog("libctest", LOG_NDELAY, LOG_USER);
    syslog(LOG_INFO, "libctest's line %d", 1);
    closelog();
#ifndef __APPLE__
    long online = sysconf(_SC_NPROCESSORS_ONLN);
    struct sysinfo si;
    sysinfo(&si);
    printf("libctest-own: 43a-3 sysconf page %ld cores %ld %ld %d %d open %ld clk %ld phys %d ram %d unit %u\n",
           sysconf(_SC_PAGESIZE), online, sysconf(_SC_NPROCESSORS_CONF), get_nprocs(), get_nprocs_conf(),
           sysconf(_SC_OPEN_MAX), sysconf(_SC_CLK_TCK), sysconf(_SC_PHYS_PAGES) > 1000,
           si.totalram > 1000 && si.freeram > 0, si.mem_unit);
    char cs[64], host[64], login[32], many[300];
    confstr(_CS_PATH, cs, sizeof(cs));
    uid_t ru, eu, su;
    getresuid(&ru, &eu, &su);
    printf("libctest-own: 43a-3 confstr %s gethostname %d getlogin_r %d %s getresuid %u %u getauxval %lu %d"
           " getrandom %ld nofile %lu pathconf %ld\n", cs, gethostname(host, sizeof(host)),
           getlogin_r(login, sizeof(login)), login, ru, eu, getauxval(AT_PAGESZ), getauxval(AT_PHNUM) > 0,
           (long)getrandom(many, sizeof(many), 0), (unsigned long)rl.rlim_cur, pathconf("/", _PC_NAME_MAX));
    child = fork();
    if (child == 0) {
        char *sh[] = { "sh", "-c", "exit $LCT_CODE", 0 }, *env[] = { "LCT_CODE=9", 0 };
        execvpe("sh", sh, env);
        _exit(1);
    }
    waitpid(child, &status, 0);
    printf("libctest-own: 43a-3 execvpe exit %d\n", WEXITSTATUS(status));
#endif
}

/* (43a-4) What C++ and runtimes need: exit's lists, the loaded objects,
 * sigsetjmp, threads' attributes and semaphores. */
int __cxa_atexit(void (*fn)(void *), void *arg, void *dso);
void __cxa_finalize(void *dso);
static char finalized[8];
static int finalized_n;

static void note_final(void *arg)
{
    finalized[finalized_n++] = *(char *)arg;
}

static sigjmp_buf sjb;

static void on_usr1_jump(int sig)
{
    (void)sig;
    siglongjmp(sjb, 5);
}

#ifndef __APPLE__
static int objects_seen, phdr_ok, lib_seen;

static int each_object(struct dl_phdr_info *info, size_t size, void *data)
{
    (void)size, (void)data;
    objects_seen++;
    if (objects_seen == 1 && info->dlpi_name[0] == 0)
        for (int i = 0; i < info->dlpi_phnum; i++)
            if (info->dlpi_phdr[i].p_type == PT_LOAD)
                phdr_ok = 1;
    if (strstr(info->dlpi_name, "libritteros"))
        lib_seen = 1;
    return 0;
}

static int dtor_ran;
int __cxa_thread_atexit_impl(void (*fn)(void *), void *obj, void *dso);

static void thread_dtor(void *obj)
{
    *(int *)obj = 1;
}

static void *with_dtor(void *arg)
{
    (void)arg;
    __cxa_thread_atexit_impl(thread_dtor, &dtor_ran, 0);
    return 0;
}

static void *stack_inside(void *arg)
{
    pthread_attr_t a;
    void *addr;
    size_t size;
    pthread_getattr_np(pthread_self(), &a);
    pthread_attr_getstack(&a, &addr, &size);
    char here;
    *(int *)arg = (char *)&here > (char *)addr && (char *)&here < (char *)addr + size && size == 262144;
    return 0;
}
#endif

static void runtimes(void)
{
    char a = 'a', b = 'b', c = 'c', other;
    __cxa_atexit(note_final, &a, &other);
    __cxa_atexit(note_final, &b, &finalized);
    __cxa_atexit(note_final, &c, &other);
    __cxa_finalize(&other);
    printf(T "43a-4 __cxa_finalize %s", finalized);
    __cxa_finalize(&finalized);
    printf(" then %s\n", finalized);

    sigset_t usr2, now;
    sigemptyset(&usr2);
    sigaddset(&usr2, SIGUSR2);
    signal(SIGUSR1, on_usr1_jump);
    int jumped = sigsetjmp(sjb, 1);
    if (jumped == 0) {
        sigprocmask(SIG_BLOCK, &usr2, 0);       /* (changed after: siglongjmp puts it back) */
        raise(SIGUSR1);
    }
    sigprocmask(SIG_BLOCK, 0, &now);
    printf(T "43a-4 sigsetjmp %d usr2 blocked %d", jumped, sigismember(&now, SIGUSR2));
    jmp_buf plain;
    int again = _setjmp(plain);
    if (!again)
        _longjmp(plain, 7);
    printf(" _setjmp %d\n", again);
    signal(SIGUSR1, SIG_DFL);

    int fds[2];
    pipe(fds);
    write(fds[1], "x", 1);
    fd_set r;
    FD_ZERO(&r);
    FD_SET(fds[0], &r);
    struct timespec zero = { 0, 0 };
    sigset_t none;
    sigemptyset(&none);
    printf(T "43a-4 pselect %d", pselect(fds[0] + 1, &r, 0, 0, &zero, &none));
    close(fds[0]);
    close(fds[1]);
    printf("\n");
#ifndef __APPLE__
    struct sched_param sp;
    printf("libctest-own: 43a-4 sched_getparam %d %d policy %d max %d\n", sched_getparam(0, &sp), sp.sched_priority,
           sched_getscheduler(0), sched_get_priority_max(SCHED_OTHER));
    dl_iterate_phdr(each_object, 0);
    Dl_info info;
    int found = dladdr((void *)printf, &info);
    printf("libctest-own: 43a-4 dl_iterate_phdr %d %d %d dladdr %d %s %s\n", objects_seen >= 2, phdr_ok, lib_seen,
           found, found ? strrchr(info.dli_fname, '/') ? strrchr(info.dli_fname, '/') + 1 : info.dli_fname : "-",
           found && info.dli_sname ? info.dli_sname : "-");
    pthread_t t;
    pthread_create(&t, 0, with_dtor, 0);
    pthread_join(t, 0);
    int inside = 0;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 262144);
    pthread_create(&t, &attr, stack_inside, &inside);
    pthread_join(t, 0);
    void *addr;
    size_t size;
    pthread_getattr_np(pthread_self(), &attr);
    pthread_attr_getstack(&attr, &addr, &size);
    char here;
    printf("libctest-own: 43a-4 thread dtor %d stack %d main %d %zu\n", dtor_ran, inside,
           &here > (char *)addr && &here < (char *)addr + size, size);
    sem_t s;
    sem_init(&s, 0, 2);
    int v1, v2;
    sem_wait(&s);
    sem_getvalue(&s, &v1);
    sem_trywait(&s);
    errno = 0;
    int busy = sem_trywait(&s);
    int eagain = errno == EAGAIN;
    struct timespec soon;
    clock_gettime(CLOCK_REALTIME, &soon);
    soon.tv_nsec += 20000000;
    if (soon.tv_nsec >= 1000000000)
        soon.tv_sec++, soon.tv_nsec -= 1000000000;
    errno = 0;
    int timed = sem_timedwait(&s, &soon);
    int etimedout = errno == ETIMEDOUT;
    sem_post(&s);
    sem_getvalue(&s, &v2);
    cpu_set_t cores;
    sched_getaffinity(0, sizeof(cores), &cores);
    printf("libctest-own: 43a-4 sem %d %d/%d %d/%d %d cores %d\n", v1, busy, eagain, timed, etimedout, v2,
           CPU_COUNT(&cores));
#endif
}

/* (43b-1) Files: at an offset, lengths, the *at calls, folders. */
static void files_at(const char *folder)
{
    char path[512], dir[512], link[512];
    snprintf(dir, sizeof(dir), "%s/at", folder);
    mkdir(dir, 0755);
    int d = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    printf(T "43b-1 open dir %d cloexec %d", d >= 0, fcntl(d, F_GETFD) & FD_CLOEXEC);
    int f = openat(d, "data", O_RDWR | O_CREAT | O_TRUNC, 0644);
    write(f, "0123456789", 10);
    char buf[32] = "";
    ssize_t n = pread(f, buf, 4, 3);
    printf(" pread %zd %.4s pos %lld", n, buf, (long long)lseek(f, 0, SEEK_CUR));
    pwrite(f, "AB", 2, 1);
    pread(f, buf, 10, 0);
    printf(" pwrite %.10s\n", buf);
    ftruncate(f, 4);
    struct stat st;
    fstat(f, &st);
    memset(buf, 'x', sizeof(buf));
    pread(f, buf, 10, 0);
    printf(T "43b-1 ftruncate %lld %.4s", (long long)st.st_size, buf);
    ftruncate(f, 6000);
    fstat(f, &st);
    char tail[4];
    pread(f, tail, 4, 5996);
    printf(" grown %lld %d%d%d%d fsync %d\n", (long long)st.st_size, tail[0], tail[1], tail[2], tail[3],
           fsync(f));
    struct iovec parts[3] = { { "one ", 4 }, { "two ", 4 }, { "three", 5 } };
    lseek(f, 0, SEEK_SET);
    ssize_t w = writev(f, parts, 3);
    char a[5] = "", b[9] = "";
    struct iovec back[2] = { { a, 4 }, { b, 9 } };
    lseek(f, 0, SEEK_SET);
    ssize_t r = readv(f, back, 2);
    printf(T "43b-1 writev %zd readv %zd '%.4s' '%.9s'", w, r, a, b);
    fchmod(f, 0600);
    fstatat(d, "data", &st, 0);
    printf(" fchmod %o", (unsigned)(st.st_mode & 0777));
    close(f);
    truncate(strcat(strcpy(path, dir), "/data"), 3);
    stat(path, &st);
    printf(" truncate %lld\n", (long long)st.st_size);
    int pipes[2];
    pipe(pipes);
    errno = 0;
    printf(T "43b-1 pread pipe %zd ESPIPE %d", pread(pipes[0], buf, 1, 0), errno == ESPIPE);
    close(pipes[0]);
    close(pipes[1]);
    mkdirat(d, "sub", 0755);
    symlinkat("data", d, "link");
    snprintf(link, sizeof(link), "%s/link", dir);
    errno = 0;
    int nf = open(link, O_RDONLY | O_NOFOLLOW);
    printf(" O_NOFOLLOW %d ELOOP %d", nf, errno == ELOOP);
    errno = 0;
    int nd = open(path, O_RDONLY | O_DIRECTORY);
    printf(" O_DIRECTORY %d ENOTDIR %d", nd, errno == ENOTDIR);
    n = readlinkat(d, "link", buf, sizeof(buf));
    printf(" readlinkat %.*s faccessat %d\n", (int)n, buf, faccessat(d, "data", R_OK, 0));
    renameat(d, "data", d, "moved");
    linkat(d, "moved", d, "hard", 0);
    struct stat hs;
    fstatat(d, "hard", &hs, 0);
    printf(T "43b-1 renameat %d linkat %d nlink %d unlinkat %d %d", faccessat(d, "moved", F_OK, 0),
           fstatat(d, "hard", &st, 0), (int)hs.st_nlink, unlinkat(d, "hard", 0), unlinkat(d, "sub", AT_REMOVEDIR));
    struct stat ls;
    printf(" fstatat nofollow %d\n", fstatat(d, "link", &ls, AT_SYMLINK_NOFOLLOW) == 0 && S_ISLNK(ls.st_mode));
    DIR *dd = fdopendir(dup(d));
    long first = telldir(dd);
    int entries = 0;
    while (readdir(dd))
        entries++;
    rewinddir(dd);
    int again = 0;
    while (readdir(dd))
        again++;
    seekdir(dd, first);
    printf(T "43b-1 fdopendir %d again %d dirfd %d", entries >= 2, again == entries, dirfd(dd) >= 0);
    closedir(dd);
    struct dirent **list;
    int listed = scandir(dir, &list, 0, alphasort);
    printf(" scandir %d:", listed > 0);
    for (int i = 0; i < listed; i++) {
        if (list[i]->d_name[0] != '.')
            printf(" %s", list[i]->d_name);
        free(list[i]);
    }
    free(list);
    char here[512];
    getcwd(here, sizeof(here));
    fchdir(d);
    char now_at[512];
    getcwd(now_at, sizeof(now_at));
    chdir(here);
    struct statvfs vfs;
    printf(" fchdir %d statvfs %d %d\n", strcmp(strrchr(now_at, '/'), "/at") == 0, statvfs(dir, &vfs),
           vfs.f_bsize > 0 && vfs.f_blocks >= vfs.f_bfree);
    unlinkat(d, "link", 0);
    unlinkat(d, "moved", 0);
    close(d);
    rmdir(dir);
#ifndef __APPLE__
    int p2[2];
    printf("libctest-own: 43b-1 pipe2 %d cloexec %d %d", pipe2(p2, O_CLOEXEC | O_NONBLOCK), fcntl(p2[0], F_GETFD),
           (fcntl(p2[1], F_GETFL) & O_NONBLOCK) != 0);
    errno = 0;
    printf(" empty read %zd EAGAIN %d", read(p2[0], buf, 1), errno == EAGAIN);
    int d3 = dup3(p2[0], 30, O_CLOEXEC);
    printf(" dup3 %d %d", d3, fcntl(30, F_GETFD));
    int s = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    printf(" socket cloexec %d statvfs block %lu\n", fcntl(s, F_GETFD), vfs.f_bsize);
    close(s);
    close(30);
    close(p2[0]);
    close(p2[1]);
#endif
}

/* (43b-2) Descriptors passed over local sockets (SCM_RIGHTS). */
static int send_fds(int sock, const char *bytes, const int *fds, int count)
{
    char control[CMSG_SPACE(sizeof(int) * 4)];
    memset(control, 0, sizeof(control));
    struct iovec part = { (void *)bytes, strlen(bytes) };
    struct msghdr m = { 0 };
    m.msg_iov = &part;
    m.msg_iovlen = 1;
    if (count) {
        m.msg_control = control;
        m.msg_controllen = CMSG_SPACE(sizeof(int) * (size_t)count);
        struct cmsghdr *c = CMSG_FIRSTHDR(&m);
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = CMSG_LEN(sizeof(int) * (size_t)count);
        memcpy(CMSG_DATA(c), fds, sizeof(int) * (size_t)count);
    }
    return (int)sendmsg(sock, &m, 0);
}

/* bytes into buf (size), the descriptors received into fds (room for
 * room): how many, -1 when cut off */
static int recv_fds(int sock, char *buf, size_t size, int *fds, int room, ssize_t *n)
{
    char control[CMSG_SPACE(sizeof(int) * 4)];
    struct iovec part = { buf, size - 1 };
    struct msghdr m = { 0 };
    m.msg_iov = &part;
    m.msg_iovlen = 1;
    m.msg_control = control;
    m.msg_controllen = room ? CMSG_LEN(sizeof(int) * (size_t)room) : 0;  /* (exactly room: SPACE's rounding fits more) */
    *n = recvmsg(sock, &m, 0);
    buf[*n > 0 ? *n : 0] = 0;
    int got = 0;
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&m); c; c = CMSG_NXTHDR(&m, c))
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
            int k = (int)((c->cmsg_len - CMSG_LEN(0)) / sizeof(int));
            memcpy(fds + got, CMSG_DATA(c), sizeof(int) * (size_t)k);
            got += k;
        }
    return m.msg_flags & MSG_CTRUNC ? -1 - got : got;
}

static void passing(const char *folder)
{
    int sv[2];
    printf(T "43b-2 socketpair %d", socketpair(AF_UNIX, SOCK_STREAM, 0, sv));
    write(sv[0], "ping", 4);
    char buf[64];
    ssize_t n = read(sv[1], buf, sizeof(buf));
    printf(" %zd %.4s\n", n, buf);
    char path[512];
    snprintf(path, sizeof(path), "%s/passed", folder);
    int f = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    write(f, "file contents", 13);
    lseek(f, 5, SEEK_SET);
    int pipes[2];
    pipe(pipes);
    pid_t child = fork();
    if (child == 0) {
        close(sv[0]);
        int got[4];
        ssize_t k;
        int c = recv_fds(sv[1], buf, sizeof(buf), got, 4, &k);
        char line[160];
        char from_file[16] = "";
        ssize_t fr = c >= 2 ? read(got[0], from_file, 8) : -1;
        if (c >= 2)
            write(got[1], "through the pipe", 16);
        snprintf(line, sizeof(line), "%d %zd %s %zd '%.*s'", c, k, buf, fr, (int)(fr > 0 ? fr : 0), from_file);
        write(sv[1], line, strlen(line));
        _exit(0);
    }
    close(sv[1]);
    int two[2] = { f, pipes[1] };
    printf(T "43b-2 sendmsg %d", send_fds(sv[0], "here", two, 2));
    close(f);                                   /* (the passed copies keep them) */
    close(pipes[1]);
    n = read(sv[0], buf, sizeof(buf) - 1);
    buf[n > 0 ? n : 0] = 0;
    char piped[32] = "";
    ssize_t pn = read(pipes[0], piped, sizeof(piped) - 1);
    piped[pn > 0 ? pn : 0] = 0;
    int status;
    waitpid(child, &status, 0);
    printf(" child got: %s; pipe: %s\n", buf, piped);
    close(pipes[0]);
    close(sv[0]);

#ifndef __APPLE__
    /* (Linux's: a read stops before a message that carries descriptors;
     * the Mac reads on into it) */
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    int one[2], fds3[3];
    pipe(one);
    fds3[0] = one[0];
    fds3[1] = one[1];
    send_fds(sv[0], "first", fds3, 2);
    send_fds(sv[0], "second", 0, 0);
    int got[4];
    ssize_t k1, k2;
    int c1 = recv_fds(sv[1], buf, sizeof(buf), got, 1, &k1);
    printf("libctest-own: 43b-2 cut %d %zd %s", c1, k1, buf);
    if (c1 < 0)
        for (int i = 0; i < -1 - c1; i++)
            close(got[i]);
    int c2 = recv_fds(sv[1], buf, sizeof(buf), got, 4, &k2);
    printf(" then %d %zd %s\n", c2, k2, buf);
    close(one[0]);
    close(one[1]);
    close(sv[0]);
    close(sv[1]);
#endif
    unlink(path);
}

/* (43b-3) Shared memory by name and named semaphores between programs:
 * the parts the Mac does as well (its objects are only sized once and
 * mapped, never read or written, and fstat calls them no regular file:
 * those are ipctest's). */
static void named(void)
{
    shm_unlink("/libctest43b3");
    int fd = shm_open("/libctest43b3", O_RDWR | O_CREAT | O_EXCL, 0600);
    errno = 0;
    int again = shm_open("/libctest43b3", O_RDWR | O_CREAT | O_EXCL, 0600);
    int again_errno = errno;
    int sized = ftruncate(fd, 4096);
    char *p = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    sem_unlink("/libctest43b3");
    sem_t *s = sem_open("/libctest43b3", O_CREAT | O_EXCL, 0600, 0);
    int pid = fork();
    if (pid == 0) {
        int c = shm_open("/libctest43b3", O_RDWR, 0);
        char *q = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, c, 0);
        sem_t *cs = sem_open("/libctest43b3", 0);
        if (q != MAP_FAILED)
            strcpy(q, "the child's");
        _exit(q != MAP_FAILED && cs != SEM_FAILED && sem_post(cs) == 0 ? 0 : 1);
    }
    int waited = s == SEM_FAILED ? -1 : sem_wait(s);
    int status = 0;
    waitpid(pid, &status, 0);
    printf(T "43b-3 shm_open %d again %d/%d sized %d mapped %d; sem_open %d waited %d child %d: %s\n",
           fd >= 0, again, again_errno == EEXIST, sized, p != MAP_FAILED, s != SEM_FAILED, waited,
           WIFEXITED(status) ? WEXITSTATUS(status) : -1, p != MAP_FAILED ? p : "");
    struct stat st;
    printf(T "43b-3 fstat %d; unlink %d", fstat(fd, &st), shm_unlink("/libctest43b3"));
    errno = 0;
    int after = shm_open("/libctest43b3", O_RDWR, 0);
    printf(" reopened %d/%d mapping kept %d", after, errno == ENOENT, p != MAP_FAILED && !strcmp(p, "the child's"));
    errno = 0;
    printf(" unlink again %d/%d", shm_unlink("/libctest43b3"), errno == ENOENT);
    printf(" sem_close %d sem_unlink %d", s != SEM_FAILED ? sem_close(s) : -1, sem_unlink("/libctest43b3"));
    errno = 0;
    printf(" reopened %d/%d\n", sem_open("/libctest43b3", 0) == SEM_FAILED, errno == ENOENT);
    if (p != MAP_FAILED)
        munmap(p, 4096);
    close(fd);
}

/* (43b-4) File locks, as the Mac does them too: flock between two open
 * files of one program, fcntl's record locks against a forked child
 * (refused, found by F_GETLK, waited for), POSIX's rule that closing any
 * descriptor of a file lets the process's locks on it go, lockf. */
static void locking(const char *folder)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/locked", folder);
    int fd1 = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    write(fd1, "0123456789abcdefghij", 20);
    int fd2 = open(path, O_RDWR);
    int a = flock(fd1, LOCK_EX);
    errno = 0;
    int b = flock(fd2, LOCK_EX | LOCK_NB);
    int b_errno = errno == EWOULDBLOCK;
    errno = 0;
    int c = flock(fd2, LOCK_SH | LOCK_NB);
    int fd3 = dup(fd1);
    int d = flock(fd3, LOCK_UN);                /* (the same open file: fd1's goes) */
    int e = flock(fd2, LOCK_SH | LOCK_NB);
    int f = flock(fd1, LOCK_SH | LOCK_NB);
    errno = 0;
    int g = flock(fd1, LOCK_EX | LOCK_NB);      /* (fd2 holds a shared one) */
    printf(T "43b-4 flock %d %d/%d %d %d %d %d %d", a, b, b_errno, c, d, e, f, g);
    close(fd2);                                 /* (its lock goes with it) */
    int h = flock(fd3, LOCK_EX | LOCK_NB);
    printf(" after close %d\n", h);
    flock(fd3, LOCK_UN);
    close(fd3);

    int up[2], down[2];
    pipe(up);
    pipe(down);
    int pid = fork();
    if (pid == 0) {
        int fd = open(path, O_RDWR);
        struct flock l = { 0 };
        l.l_type = F_WRLCK;
        l.l_whence = SEEK_SET;
        l.l_start = 0;
        l.l_len = 10;
        char ok = fcntl(fd, F_SETLK, &l) == 0 ? 'y' : 'n';
        write(up[1], &ok, 1);
        read(down[0], &ok, 1);                  /* (the parent has looked) */
        usleep(200000);
        _exit(0);                               /* (its locks go with it) */
    }
    char got = 0;
    read(up[0], &got, 1);
    struct flock l = { 0 };
    l.l_type = F_WRLCK;
    l.l_whence = SEEK_SET;
    l.l_start = 5;
    l.l_len = 10;
    errno = 0;
    int refused = fcntl(fd1, F_SETLK, &l);
    int refused_errno = errno == EAGAIN || errno == EACCES;
    struct flock q = { 0 };
    q.l_type = F_WRLCK;
    q.l_whence = SEEK_SET;
    q.l_start = 0;
    q.l_len = 0;
    int looked = fcntl(fd1, F_GETLK, &q);
    printf(T "43b-4 fcntl child %c refused %d/%d getlk %d: type %d start %lld len %lld pid %d",
           got, refused, refused_errno, looked, q.l_type == F_WRLCK, (long long)q.l_start,
           (long long)q.l_len, q.l_pid == pid);
    struct flock r = { 0 };
    r.l_type = F_RDLCK;
    r.l_whence = SEEK_END;
    r.l_start = -10;
    r.l_len = 10;
    printf(" beside it %d", fcntl(fd1, F_SETLK, &r));
    write(down[1], "g", 1);
    l.l_start = 0;
    int waited = fcntl(fd1, F_SETLKW, &l);
    int status = 0;
    waitpid(pid, &status, 0);
    printf(" waited %d\n", waited);

    /* Closing another descriptor of the file lets the locks go. */
    int other = open(path, O_RDONLY);
    close(other);
    pid = fork();
    if (pid == 0) {
        int fd = open(path, O_RDWR);
        struct flock t2 = { 0 };
        t2.l_type = F_WRLCK;
        t2.l_whence = SEEK_SET;
        int free_now = fcntl(fd, F_GETLK, &t2) == 0 && t2.l_type == F_UNLCK;
        int lockf_ok = lockf(fd, F_TLOCK, 0) == 0;
        _exit(free_now * 2 + lockf_ok);
    }
    waitpid(pid, &status, 0);
    printf(T "43b-4 close lets go %d, lockf in the child %d", WEXITSTATUS(status) >> 1, WEXITSTATUS(status) & 1);
    lseek(fd1, 0, SEEK_SET);
    printf(" lockf %d test %d unlock %d\n", lockf(fd1, F_TLOCK, 4), lockf(fd1, F_TEST, 4),
           lockf(fd1, F_ULOCK, 4));
    close(fd1);
    close(up[0]);
    close(up[1]);
    close(down[0]);
    close(down[1]);
    unlink(path);
}

/* (43b-5) Signals as POSIX has them, as the Mac does them too (its
 * numbers differ: only names are printed). */
static volatile sig_atomic_t got_signo, got_code, got_pid, got_uid, got_status, got_count;
static void *volatile got_addr;
static volatile int on_alt;
static char *alt_low;
static size_t alt_size;
static sigjmp_buf fault_back;
static volatile pthread_t handler_thread;

static void info_handler(int sig, siginfo_t *info, void *context)
{
    (void)context;
    got_signo = info->si_signo == sig;
    got_code = info->si_code;
    got_pid = info->si_pid;
    got_uid = info->si_uid;
    got_status = info->si_status;
    got_count++;
}

static void alt_handler(int sig)
{
    (void)sig;
    char here;
    stack_t now;
    sigaltstack(0, &now);
    on_alt = (&here >= alt_low && &here < alt_low + alt_size) * 2 + ((now.ss_flags & SS_ONSTACK) != 0);
}

static void fault_handler(int sig, siginfo_t *info, void *context)
{
    (void)context;
    got_signo = sig;
    got_code = info->si_code;
    got_addr = info->si_addr;
    siglongjmp(fault_back, 1);
}

static void thread_handler(int sig)
{
    (void)sig;
    handler_thread = pthread_self();
}

static volatile int thread_go;
static void *signalled_thread(void *arg)
{
    (void)arg;
    while (!thread_go)
        usleep(1000);
    return 0;
}

static void signals_43b5(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = info_handler;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGUSR1, &sa, 0);
    kill(getpid(), SIGUSR1);
    printf(T "43b-5 SA_SIGINFO signo %d pid %d uid %d\n", (int)got_signo, got_pid == getpid(),
           got_uid == (int)getuid());
#ifndef __APPLE__
    printf("libctest-own: 43b-5 kill's si_code SI_USER %d\n", got_code == SI_USER);    /* (the Mac's is 0) */
#endif
    struct sigaction back;
    sigaction(SIGUSR1, 0, &back);
    printf(T "43b-5 sigaction's old one: SA_SIGINFO %d same %d\n", (back.sa_flags & SA_SIGINFO) != 0,
           back.sa_sigaction == info_handler);

    /* SIGCHLD: the child's end, its code, pid and status. */
    sigaction(SIGCHLD, &sa, 0);
    sigset_t block, old;
    sigemptyset(&block);
    sigaddset(&block, SIGCHLD);
    sigprocmask(SIG_BLOCK, &block, &old);
    got_count = 0;
    int pid = fork();
    if (pid == 0)
        _exit(7);
    while (!got_count)
        sigsuspend(&old);
    int status;
    waitpid(pid, &status, 0);
    printf(T "43b-5 SIGCHLD exited %d pid %d status %d", got_code == CLD_EXITED, got_pid == pid, (int)got_status);
    got_count = 0;
    pid = fork();
    if (pid == 0) {
        pause();
        _exit(0);
    }
    kill(pid, SIGTERM);
    while (!got_count)
        sigsuspend(&old);
    waitpid(pid, &status, 0);
    printf(" killed %d status SIGTERM %d\n", got_code == CLD_KILLED, got_status == SIGTERM);
    sigprocmask(SIG_SETMASK, &old, 0);

    /* By default SIGCHLD is ignored: a read goes on while a child ends. */
    signal(SIGCHLD, SIG_DFL);
    int fds[2];
    pipe(fds);
    int quick = fork();
    if (quick == 0)
        _exit(0);
    int slow = fork();
    if (slow == 0) {
        usleep(300000);
        write(fds[1], "late", 4);
        _exit(0);
    }
    char buf[8] = { 0 };
    errno = 0;
    ssize_t r = read(fds[0], buf, 4);
    printf(T "43b-5 default SIGCHLD: read %ld %s errno %d", (long)r, buf, errno);
    waitpid(quick, &status, 0);
    waitpid(slow, &status, 0);
    close(fds[0]);
    close(fds[1]);
    raise(SIGWINCH);
    raise(SIGURG);
    raise(SIGCONT);
    printf("; WINCH, URG, CONT ignored: still here\n");

    /* kill's other targets. */
    errno = 0;
    int self = kill(getpid(), 0), group = kill(0, 0);
    int none = kill(999999, 0);
    int none_errno = errno;
    int child = fork();
    if (child == 0) {
        setpgid(0, 0);
        pause();
        _exit(0);
    }
    setpgid(child, child);
    usleep(100000);
    int pg = killpg(child, SIGTERM);
    waitpid(child, &status, 0);
    printf(T "43b-5 kill 0: self %d group %d none %d/%d; killpg %d: ended by SIGTERM %d\n", self, group, none,
           none_errno == ESRCH, pg, WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM);

    /* An alternate stack. */
    alt_size = SIGSTKSZ;
    alt_low = malloc(alt_size);
    stack_t ss, was;
    memset(&ss, 0, sizeof(ss));
    ss.ss_sp = alt_low;
    ss.ss_size = alt_size;
    int set = sigaltstack(&ss, &was);
    struct sigaction on;
    memset(&on, 0, sizeof(on));
    on.sa_handler = alt_handler;
    on.sa_flags = SA_ONSTACK;
    sigaction(SIGUSR2, &on, 0);
    raise(SIGUSR2);
    int there = on_alt;
    on.sa_flags = 0;
    sigaction(SIGUSR2, &on, 0);
    raise(SIGUSR2);
    stack_t off, now;
    memset(&off, 0, sizeof(off));
    off.ss_flags = SS_DISABLE;
    sigaltstack(&off, 0);
    sigaltstack(0, &now);
    errno = 0;
    stack_t tiny;
    memset(&tiny, 0, sizeof(tiny));
    tiny.ss_sp = alt_low;
    tiny.ss_size = 16;
    int small = sigaltstack(&tiny, 0);
    int small_errno = errno;
    printf(T "43b-5 sigaltstack %d (before: disabled %d): on it %d, without SA_ONSTACK %d, too small %d/%d\n",
           set, (was.ss_flags & SS_DISABLE) != 0, there, on_alt, small, small_errno == ENOMEM);
#ifndef __APPLE__
    printf("libctest-own: 43b-5 sigaltstack off: SS_DISABLE %d\n", (now.ss_flags & SS_DISABLE) != 0);
#endif
    free(alt_low);

#ifndef __APPLE__
    /* A fault's address and kind (the Mac's write to a read-only page is
     * SIGBUS, its codes 0: Linux's rules here). */
    long page = sysconf(_SC_PAGESIZE);
    char *m = mmap(0, (size_t)page * 3, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    mprotect(m, (size_t)page, PROT_READ);
    munmap(m + page * 2, (size_t)page);
    struct sigaction fs;
    memset(&fs, 0, sizeof(fs));
    fs.sa_sigaction = fault_handler;
    fs.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &fs, 0);
    sigaction(SIGBUS, &fs, 0);
    int k1 = -1, k2 = -1, a1 = 0, a2 = 0, s1 = 0, s2 = 0;
    if (!sigsetjmp(fault_back, 1))
        *(volatile char *)(m + 5) = 1;
    k1 = got_code, a1 = got_addr == m + 5, s1 = got_signo == SIGSEGV;
    if (!sigsetjmp(fault_back, 1))
        (void)*(volatile char *)(m + page * 2 + 9);
    k2 = got_code, a2 = got_addr == m + page * 2 + 9, s2 = got_signo == SIGSEGV;
    printf("libctest-own: 43b-5 SIGSEGV read-only: ACCERR %d at %d %d; unmapped: MAPERR %d at %d %d\n",
           k1 == SEGV_ACCERR, a1, s1, k2 == SEGV_MAPERR, a2, s2);
    signal(SIGSEGV, SIG_DFL);
    signal(SIGBUS, SIG_DFL);
    munmap(m, (size_t)page * 2);
#endif

    /* pthread_kill: the handler runs in that thread. */
    signal(SIGUSR2, thread_handler);
    pthread_t t;
    pthread_create(&t, 0, signalled_thread, 0);
    usleep(50000);
    int pk = pthread_kill(t, SIGUSR2);
    usleep(100000);
    thread_go = 1;
    pthread_join(t, 0);
    printf(T "43b-5 pthread_kill %d: in that thread %d; signal 0 %d\n", pk, pthread_equal(handler_thread, t) != 0,
           pthread_kill(pthread_self(), 0));
    signal(SIGUSR2, SIG_DFL);
    signal(SIGUSR1, SIG_DFL);

    /* SIGCHLD ignored: ended children are freed, wait finds none. */
    signal(SIGCHLD, SIG_IGN);
    int gone = fork();
    if (gone == 0)
        _exit(3);
    usleep(200000);
    errno = 0;
    int w = waitpid(gone, &status, 0);
    printf(T "43b-5 SIGCHLD ignored: waitpid %d ECHILD %d\n", w, errno == ECHILD);
    signal(SIGCHLD, SIG_DFL);
}

/* (43b-6) Threads and the system: a thread's CPU clock, getsid, a spawned
 * child's folder by descriptor; RitterOS's own (the Mac has another
 * pthread_setname_np, names its loopback lo0, has no getauxval): thread
 * names, getifaddrs, AT_HWCAP. */
static volatile int spin_stop;

static void *spinner(void *arg)
{
    (void)arg;
    while (!spin_stop) {
    }
    return 0;
}

static double cpu_seconds(clockid_t c)
{
    struct timespec t;
    clock_gettime(c, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

static void system_43b6(const char *folder)
{
    double thread0 = cpu_seconds(CLOCK_THREAD_CPUTIME_ID), process0 = cpu_seconds(CLOCK_PROCESS_CPUTIME_ID);
    pthread_t t;
    pthread_create(&t, 0, spinner, 0);
    struct timespec wait = { 0, 300000000 };
    nanosleep(&wait, 0);                        /* (this thread sleeps, the other spins) */
    spin_stop = 1;
    pthread_join(t, 0);
    double thread1 = cpu_seconds(CLOCK_THREAD_CPUTIME_ID), process1 = cpu_seconds(CLOCK_PROCESS_CPUTIME_ID);
    printf(T "43b-6 thread CPU clock: this thread's grew less than the program's %d, the program's grew %d\n",
           thread1 - thread0 < (process1 - process0) / 2, process1 - process0 > 0.1);

    int pipefd[2];
    pipe(pipefd);
    pid_t child = fork();
    if (child == 0) {
        setsid();
        char c = 's';
        write(pipefd[1], &c, 1);
        pause();
        _exit(0);
    }
    char c;
    read(pipefd[0], &c, 1);
    errno = 0;
    int none = (int)getsid(999999);
    printf(T "43b-6 getsid: own %d its own child %d none %d/%d\n", getsid(0) == getsid(getpid()),
           getsid(child) == child, none, errno == ESRCH);
    kill(child, SIGKILL);
    int status;
    waitpid(child, &status, 0);
    close(pipefd[0]);
    close(pipefd[1]);

    char marker[512];
    snprintf(marker, sizeof(marker), "%s/lctmarker6", folder);
    FILE *m = fopen(marker, "w");
    fclose(m);
    int dir = open(folder, O_RDONLY);
    posix_spawn_file_actions_t acts;
    posix_spawn_file_actions_init(&acts);
    int added = posix_spawn_file_actions_addfchdir_np(&acts, dir);
    char *args[] = { "sh", "-c", "test -f lctmarker6", 0 };
    pid_t sp;
    int r = posix_spawnp(&sp, "sh", &acts, 0, args, environ);
    waitpid(sp, &status, 0);
    printf(T "43b-6 addfchdir %d spawn %d status %d\n", added, r, WEXITSTATUS(status));
    posix_spawn_file_actions_destroy(&acts);
    close(dir);
    unlink(marker);

#ifndef __APPLE__
    char name[16] = "";
    int set = pthread_setname_np(pthread_self(), "libctest-main");
    int got = pthread_getname_np(pthread_self(), name, sizeof(name));
    int too_long = pthread_setname_np(pthread_self(), "a name of twenty bytes");
    char small[4];
    int too_small = pthread_getname_np(pthread_self(), small, sizeof(small));
    clockid_t own;
    printf("libctest-own: 43b-6 thread name %d %d %s, long %d, small %d; pthread_getcpuclockid %d\n", set, got,
           name, too_long == ERANGE, too_small == ERANGE, pthread_getcpuclockid(pthread_self(), &own) == 0 &&
           own == CLOCK_THREAD_CPUTIME_ID);

    struct ifaddrs *list = 0, *a;
    int lo4 = 0, lo6 = 0, entries = 0;
    int r6 = getifaddrs(&list);
    for (a = list; a; a = a->ifa_next) {
        entries++;
        if (strcmp(a->ifa_name, "lo") || !a->ifa_addr || !(a->ifa_flags & IFF_LOOPBACK))
            continue;
        if (a->ifa_addr->sa_family == AF_INET) {
            struct sockaddr_in *s = (struct sockaddr_in *)a->ifa_addr, *n = (struct sockaddr_in *)a->ifa_netmask;
            lo4 = s->sin_addr.s_addr == htonl(0x7F000001) && n->sin_addr.s_addr == htonl(0xFF000000);
        } else if (a->ifa_addr->sa_family == AF_INET6) {
            struct sockaddr_in6 *s = (struct sockaddr_in6 *)a->ifa_addr;
            lo6 = s->sin6_addr.s6_addr[15] == 1;
        }
    }
    freeifaddrs(list);
    char back[IF_NAMESIZE];
    unsigned lo = if_nametoindex("lo");
    struct if_nameindex *names = if_nameindex();
    printf("libctest-own: 43b-6 getifaddrs %d: lo 127.0.0.1/8 %d ::1 %d, %d entries; if_nametoindex lo %u, back %s,"
           " if_nameindex first %s, none %u\n", r6, lo4, lo6, entries, lo, if_indextoname(lo, back),
           names ? names[0].if_name : "-", if_nametoindex("nope0"));
    if_freenameindex(names);
#if defined(__x86_64__)
    unsigned eax = 1, ebx, ecx, edx;
    __asm__ volatile ("cpuid" : "+a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx));
    printf("libctest-own: 43b-6 AT_HWCAP %d\n", getauxval(AT_HWCAP) == edx && edx != 0);
#elif defined(__aarch64__)
    unsigned long hw = getauxval(AT_HWCAP);
    printf("libctest-own: 43b-6 AT_HWCAP %d\n", (hw & HWCAP_FP) && (hw & HWCAP_ASIMD));
#endif
#endif
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

    ported(argv[1]);
    locales();
    system_calls(argv[1]);
    runtimes();
    files_at(argv[1]);
    passing(argv[1]);
    named();
    locking(argv[1]);
    signals_43b5();
    system_43b6(argv[1]);

    /* system, stderr */
    int status = system("exit 3");
    printf(T "system %d exit %d\n", system(0) != 0, WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    fprintf(stdout, T "to stdout %d\n", 7);
    fflush(stdout);
    fprintf(stderr, T "to stderr\n");
    printf(T "done\n");
    return 0;
}
