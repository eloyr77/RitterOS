/*
 * (43a-3) What ported programs ask about the system and themselves:
 * sysconf, pathconf, confstr; getrlimit/setrlimit, getrusage, priorities;
 * times; sysinfo and get_nprocs; the host and login names; ttyname;
 * getrandom and getentropy. Fixed limits are RitterOS's own (a program's
 * 256 descriptors and threads, 8 MiB stacks, 128 KiB of arguments); the
 * cores come from the kernel's cores report (the program cpus's lines:
 * each core a line of its index and id, the started ones "running" or
 * "online"), memory from SYS_SYSINFO.
 */
#include <errno.h>
#include <limits.h>
#include <pwd.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/resource.h>
#include <sys/sysinfo.h>
#include <sys/times.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>
#include "ritteros.h"

/* The cores: all the firmware lists (*all) and those started (*online). */
static void cores(long *all, long *online)
{
    static long cached_all, cached_online;
    if (!cached_all) {
        char text[8192];
        int64_t n = ritteros_report(REPORT_CPUS, text, sizeof(text) - 1);
        long a = 0, o = 0;
        if (n > 0) {
            text[n < (int64_t)sizeof(text) - 1 ? n : (int64_t)sizeof(text) - 1] = 0;
            for (char *line = text; line && *line;) {
                char *next = strchr(line, '\n');
                if (next)
                    *next++ = 0;
                char *p = line;
                while (*p == ' ')
                    p++;
                if ((*p == '-' || (*p >= '0' && *p <= '9')) && strstr(p, "  0x")) {
                    a++;
                    if (strstr(p, "running") || strstr(p, "online"))
                        o++;
                }
                line = next;
            }
        }
        cached_all = a > 0 ? a : 1;
        cached_online = o > 0 ? o : 1;
    }
    *all = cached_all;
    *online = cached_online;
}

static bool memory(uint64_t *pages, uint64_t *free_pages, uint64_t *page_size)
{
    struct sys_info info;
    if (syscall6(SYS_SYSINFO, (int64_t)(uintptr_t)&info, 0, 0, 0, 0, 0) < 0)
        return false;
    *pages = info.usable_pages;
    *free_pages = info.free_pages;
    *page_size = info.page_size ? info.page_size : 4096;
    return true;
}

long sysconf(int name)
{
    long all, online;
    uint64_t pages, free_pages, page_size;
    switch (name) {
    case _SC_ARG_MAX: return ARG_MAX;
    case _SC_CHILD_MAX: return 1024;
    case _SC_CLK_TCK: return 100;
    case _SC_NGROUPS_MAX: return NGROUPS_MAX;
    case _SC_OPEN_MAX: return OPEN_MAX;
    case _SC_STREAM_MAX: return OPEN_MAX;
    case _SC_TZNAME_MAX: return 9;
    case _SC_JOB_CONTROL: case _SC_SAVED_IDS: case _SC_VERSION: case _SC_THREADS:
    case _SC_THREAD_SAFE_FUNCTIONS: case _SC_MAPPED_FILES: case _SC_MEMORY_PROTECTION:
    case _SC_MONOTONIC_CLOCK: case _SC_FSYNC: case _SC_SEMAPHORES: case _SC_TIMERS:
        return 200809L;
    case _SC_PAGESIZE: return 4096;
    case _SC_LINE_MAX: return LINE_MAX;
    case _SC_RE_DUP_MAX: return RE_DUP_MAX;
    case _SC_IOV_MAX: return IOV_MAX;
    case _SC_GETGR_R_SIZE_MAX: case _SC_GETPW_R_SIZE_MAX: return 1024;
    case _SC_LOGIN_NAME_MAX: return LOGIN_NAME_MAX;
    case _SC_TTY_NAME_MAX: return TTY_NAME_MAX;
    case _SC_HOST_NAME_MAX: return HOST_NAME_MAX;
    case _SC_SYMLOOP_MAX: return SYMLOOP_MAX;
    case _SC_THREAD_KEYS_MAX: return PTHREAD_KEYS_MAX;
    case _SC_THREAD_STACK_MIN: return PTHREAD_STACK_MIN;
    case _SC_THREAD_THREADS_MAX: return PTHREAD_THREADS_MAX;
    case _SC_SEM_NSEMS_MAX: return SEM_NSEMS_MAX;
    case _SC_SEM_VALUE_MAX: return SEM_VALUE_MAX;
    case _SC_LEVEL1_DCACHE_LINESIZE: return 64;
    case _SC_NPROCESSORS_CONF:
        cores(&all, &online);
        return all;
    case _SC_NPROCESSORS_ONLN:
        cores(&all, &online);
        return online;
    case _SC_PHYS_PAGES:
        return memory(&pages, &free_pages, &page_size) ? (long)pages : -1;
    case _SC_AVPHYS_PAGES:
        return memory(&pages, &free_pages, &page_size) ? (long)free_pages : -1;
    default:
        errno = EINVAL;
        return -1;
    }
}

int getpagesize(void)
{
    return 4096;
}

int getdtablesize(void)
{
    return OPEN_MAX;
}

long fpathconf(int fd, int name)
{
    (void)fd;
    return pathconf(0, name);
}

long pathconf(const char *path, int name)
{
    (void)path;
    switch (name) {
    case _PC_LINK_MAX: return 65000;
    case _PC_MAX_CANON: case _PC_MAX_INPUT: return 255;
    case _PC_NAME_MAX: return NAME_MAX;
    case _PC_PATH_MAX: return PATH_MAX;
    case _PC_PIPE_BUF: return PIPE_BUF;
    case _PC_CHOWN_RESTRICTED: case _PC_NO_TRUNC: return 1;
    case _PC_VDISABLE: return 0;
    case _PC_FILESIZEBITS: return 64;
    case _PC_SYMLINK_MAX: return PATH_MAX - 1;
    default:
        errno = EINVAL;
        return -1;
    }
}

size_t confstr(int name, char *buf, size_t size)
{
    const char *value;
    if (name == _CS_PATH)
        value = "/bin:/usr/bin";
    else {
        errno = EINVAL;
        return 0;
    }
    size_t n = strlen(value) + 1;
    if (buf && size) {
        size_t k = n < size ? n : size;
        memcpy(buf, value, k - 1);
        buf[k - 1] = 0;
    }
    return n;
}

int gethostname(char *name, size_t size)
{
    struct utsname u;
    if (uname(&u) < 0)
        return -1;
    size_t n = strlen(u.nodename);
    if (n + 1 > size) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(name, u.nodename, n + 1);
    return 0;
}

gid_t getegid(void)
{
    return getgid();                    /* (a program runs as its file's user, never its group) */
}

int getresuid(uid_t *real, uid_t *effective, uid_t *saved)
{
    *real = getuid();
    *effective = *saved = geteuid();
    return 0;
}

int getresgid(gid_t *real, gid_t *effective, gid_t *saved)
{
    *real = *effective = *saved = getgid();
    return 0;
}

int getgroups(int size, gid_t list[])
{
    if (size == 0)
        return 1;
    if (size < 1) {
        errno = EINVAL;
        return -1;
    }
    list[0] = getgid();
    return 1;
}

int issetugid(void)
{
    return getuid() != geteuid();
}

static char login_name[LOGIN_NAME_MAX];

char *getlogin(void)
{
    return getlogin_r(login_name, sizeof(login_name)) ? 0 : login_name;
}

int getlogin_r(char *name, size_t size)
{
    struct passwd *pw = getpwuid(getuid());
    if (!pw)
        return ENXIO;
    if (strlen(pw->pw_name) + 1 > size)
        return ERANGE;
    strcpy(name, pw->pw_name);
    return 0;
}

/* A terminal's name: every terminal a program has is its /dev/tty (the
 * console or its session's pseudo-terminal). */
int ttyname_r(int fd, char *name, size_t size)
{
    if (!isatty(fd))
        return errno ? errno : ENOTTY;
    if (size < sizeof("/dev/tty"))
        return ERANGE;
    strcpy(name, "/dev/tty");
    return 0;
}

char *ttyname(int fd)
{
    static char name[TTY_NAME_MAX];
    int r = ttyname_r(fd, name, sizeof(name));
    if (r) {
        errno = r;
        return 0;
    }
    return name;
}

int nice(int increment)
{
    (void)increment;
    return 0;                           /* (the scheduler has no priorities) */
}

/* Limits: the system's fixed ones; a program may ask for less (it is
 * believed, nothing is enforced beyond the system's) but not for more. */
static const struct rlimit *fixed_limit(int resource)
{
    static const struct rlimit unlimited = { RLIM_INFINITY, RLIM_INFINITY };
    static const struct rlimit files = { OPEN_MAX, OPEN_MAX };
    static const struct rlimit stack = { 8 * 1024 * 1024, 8 * 1024 * 1024 };
    static const struct rlimit threads = { PTHREAD_THREADS_MAX, PTHREAD_THREADS_MAX };
    static const struct rlimit none = { 0, 0 };
    switch (resource) {
    case RLIMIT_NOFILE: return &files;
    case RLIMIT_STACK: return &stack;
    case RLIMIT_NPROC: return &threads;
    case RLIMIT_CORE: case RLIMIT_MEMLOCK: return &none;
    case RLIMIT_CPU: case RLIMIT_FSIZE: case RLIMIT_DATA: case RLIMIT_AS: case RLIMIT_RSS:
    case RLIMIT_LOCKS: case RLIMIT_SIGPENDING: case RLIMIT_MSGQUEUE: case RLIMIT_NICE:
    case RLIMIT_RTPRIO: case RLIMIT_RTTIME:
        return &unlimited;
    default:
        return 0;
    }
}

static struct rlimit asked[RLIMIT_NLIMITS];
static bool asked_set[RLIMIT_NLIMITS];

int getrlimit(int resource, struct rlimit *limit)
{
    const struct rlimit *fixed = fixed_limit(resource);
    if (!fixed) {
        errno = EINVAL;
        return -1;
    }
    *limit = asked_set[resource] ? asked[resource] : *fixed;
    return 0;
}

int setrlimit(int resource, const struct rlimit *limit)
{
    const struct rlimit *fixed = fixed_limit(resource);
    if (!fixed || limit->rlim_cur > limit->rlim_max) {
        errno = EINVAL;
        return -1;
    }
    if (limit->rlim_max > fixed->rlim_max && getuid() != 0) {
        errno = EPERM;
        return -1;
    }
    asked[resource] = *limit;
    asked_set[resource] = true;
    return 0;
}

int getrusage(int who, struct rusage *usage)
{
    if (who != RUSAGE_SELF && who != RUSAGE_CHILDREN && who != RUSAGE_THREAD) {
        errno = EINVAL;
        return -1;
    }
    memset(usage, 0, sizeof(*usage));
    if (who == RUSAGE_CHILDREN)
        return 0;                       /* (the children's time is not kept) */
    int64_t ns = syscall6(SYS_CPUTIME, 0, 0, 0, 0, 0, 0);
    if (ns > 0) {
        usage->ru_utime.tv_sec = ns / 1000000000;
        usage->ru_utime.tv_usec = ns % 1000000000 / 1000;
    }
    return 0;
}

int getpriority(int which, id_t who)
{
    (void)which, (void)who;
    errno = 0;
    return 0;
}

int setpriority(int which, id_t who, int priority)
{
    (void)which, (void)who, (void)priority;
    return 0;
}

clock_t times(struct tms *t)
{
    int64_t ns = syscall6(SYS_CPUTIME, 0, 0, 0, 0, 0, 0);
    if (t) {
        t->tms_utime = ns > 0 ? (clock_t)(ns / 10000000) : 0;
        t->tms_stime = t->tms_cutime = t->tms_cstime = 0;
    }
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (clock_t)(now.tv_sec * 100 + now.tv_nsec / 10000000);
}

int sysinfo(struct sysinfo *info)
{
    struct sys_info s;
    if (syscall6(SYS_SYSINFO, (int64_t)(uintptr_t)&s, 0, 0, 0, 0, 0) < 0) {
        errno = EIO;
        return -1;
    }
    memset(info, 0, sizeof(*info));
    info->uptime = s.ticks_per_second ? (long)(s.ticks / s.ticks_per_second) : 0;
    info->totalram = s.usable_pages;
    info->freeram = s.free_pages;
    info->mem_unit = (unsigned)(s.page_size ? s.page_size : 4096);
    long all, online;
    cores(&all, &online);
    info->procs = 0;
    return 0;
}

int get_nprocs_conf(void)
{
    long all, online;
    cores(&all, &online);
    return (int)all;
}

int get_nprocs(void)
{
    long all, online;
    cores(&all, &online);
    return (int)online;
}

long get_phys_pages(void)
{
    return sysconf(_SC_PHYS_PAGES);
}

long get_avphys_pages(void)
{
    return sysconf(_SC_AVPHYS_PAGES);
}

/* Random bytes from the kernel (SYS_RANDOM gives 256 a call). */
ssize_t getrandom(void *buf, size_t size, unsigned flags)
{
    (void)flags;
    unsigned char *p = buf;
    size_t done = 0;
    while (done < size) {
        size_t n = size - done < 256 ? size - done : 256;
        if (ritteros_random(p + done, n) < 0) {
            if (done)
                break;
            errno = EIO;
            return -1;
        }
        done += n;
    }
    return (ssize_t)done;
}

int getentropy(void *buf, size_t size)
{
    if (size > 256) {
        errno = EIO;
        return -1;
    }
    return getrandom(buf, size, 0) == (ssize_t)size ? 0 : -1;
}
