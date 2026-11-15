/* Reading, writing and closing file descriptors. */
#ifndef RITTEROS_UNISTD_H
#define RITTEROS_UNISTD_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define STDIN_FILENO    0
#define STDOUT_FILENO   1
#define STDERR_FILENO   2

/* Bytes transferred (read: 0 at the end), or -1 with errno set. */
ssize_t read(int fd, void *buffer, size_t size);
ssize_t write(int fd, const void *buffer, size_t size);
int close(int fd);

/* (52a) Another descriptor for fd's open file (the two share its position):
 * dup takes the lowest free number, dup2 newfd (closing what was open
 * there; newfd == fd does nothing). The new descriptor, or -1 with errno
 * set. */
int dup(int fd);
int dup2(int fd, int newfd);

/* (52a) A pipe: fds[0] reads what fds[1] writes (a read gives 0 once every
 * write end is closed, a write fails with EPIPE once every read end is).
 * 0, or -1 with errno set. */
int pipe(int fds[2]);

/* Move a file's position (SEEK_SET: from the start, SEEK_CUR, SEEK_END);
 * the new position, or -1 with errno set. */
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
off_t lseek(int fd, off_t offset, int whence);

pid_t getpid(void);

/* The program's user and group (0: root); root may change them (setuid
 * sets both users). getuid is the real user, who started it; geteuid the
 * effective one, whose rights apply: they differ only in a setuid program
 * (a root-owned one with mode 4755 runs as root for anyone). */
uid_t getuid(void);
uid_t geteuid(void);
gid_t getgid(void);
int setuid(uid_t uid);
int setgid(gid_t gid);

/* Pause the program; other programs run meanwhile. */
unsigned int sleep(unsigned int seconds);
int usleep(useconds_t microseconds);

/* RitterOS: start the program at path with the arguments argv (argv[0]:
 * its name; a null pointer ends the list) as a child process. Returns its
 * ID, or -1 with errno set. The child shares standard input and output and
 * the current directory, and joins the caller's process group;
 * spawn_new_group makes it lead a new one (a job, for a shell). */
/* (22t) The program's environment: "NAME=value" strings, ended by a null
 * pointer (environ is a call underneath: a library exports no data). */
char ***__ritteros_environ(void);
#define environ (*__ritteros_environ())

pid_t spawn(const char *path, char *const argv[]);

/* (55b) Run another program in this process: same process number,
 * parent, user, folder, umask and open descriptors (FD_CLOEXEC ones
 * close), the blocked and ignored signals kept, handled ones back to
 * SIG_DFL, the other threads ended. They return only on an error (-1,
 * errno set; the program goes on). execve: envp the environment; execv
 * and execl: this one's; execvp and execlp look a name without '/' up in
 * PATH (none: /system/<arch>/bin, then /apps) and run a script without #!
 * through sh; execle takes the environment after the list's null pointer. */
int execve(const char *path, char *const argv[], char *const envp[]);
int execv(const char *path, char *const argv[]);
int execvp(const char *name, char *const argv[]);
int execl(const char *path, const char *arg, ...);
int execlp(const char *name, const char *arg, ...);
int execle(const char *path, const char *arg, ...);
/* (56a) A new process that is a copy of this one: its memory (copied when
 * either writes a page; MAP_SHARED memory stays shared), open descriptors
 * (the same open files), folder, umask, signal handlers and blocked set,
 * but only the calling thread, and no signals pending. The child's process
 * number, and 0 in the child; -1 (errno: ENOMEM) if it cannot be made.
 * pthread_atfork's handlers run around it. */
pid_t fork(void);
/* (56a) End at once, as C's _Exit: no atexit functions, no flushing (a
 * forked child that must not write its parent's buffered output again). */
__attribute__((noreturn)) void _exit(int code);
pid_t spawn_new_group(const char *path, char *const argv[]);

int chdir(const char *path);

/* Remove an empty directory, or a file; 0, or -1 with errno set. */
int rmdir(const char *path);
int unlink(const char *path);
/* (57a) Links, on RitterFS (EPERM on FAT and exFAT). symlink: a symbolic
 * link at path naming target (kept as given: relative to the link's
 * folder when used; 1-255 bytes); paths through it lead to target (40 at
 * most in one path, then ELOOP); unlink and rename act on the link itself.
 * readlink: the bytes it names, not zero-terminated, at most size; how
 * many (EINVAL: not a link). link: a second name to for the file at from
 * (a symbolic link itself, not what it names), on the same volume (EXDEV),
 * not for a folder (EPERM); the file goes when its last name does. */
int symlink(const char *target, const char *path);
ssize_t readlink(const char *path, char *buffer, size_t size);
int link(const char *from, const char *to);
char *getcwd(char *buffer, size_t size);

/* 1 if fd is a terminal (a console or pseudo-terminal), else 0. */
int isatty(int fd);

/* Process groups and the terminal's foreground job (fd: a terminal). */
pid_t getpgrp(void);
/* (56b) The parent's process number (0: started by the kernel, or its
 * parent has ended). setpgid: pid (0: this one, or a child that has not
 * run another program: EACCES) joins group (0: its own number, a new group)
 * of this session (EPERM otherwise, and for a session's leader); getpgid:
 * a process's group (ESRCH: none); setsid: a new session and group led by
 * this process, with no terminal (EPERM if it leads a group already). */
pid_t getppid(void);
int setpgid(pid_t pid, pid_t group);
pid_t getpgid(pid_t pid);
pid_t setsid(void);
/* (43b-6) pid's session (0: this process's): its leader's number; ESRCH. */
pid_t getsid(pid_t pid);
pid_t tcgetpgrp(int fd);
int tcsetpgrp(int fd, pid_t group);

/* (43a) getopt: the next option letter of argv (options: the letters,
 * a ':' after one that takes an argument, "::" an optional one written in
 * the same word); -1 at the first word that is not an option, or after
 * "--"; '?' for an unknown option or a missing argument (':' instead for
 * the latter when options starts with ':'), with a message unless opterr
 * is 0. As GNU's: words that are not options are moved after the options
 * (unless options starts with '+' or POSIXLY_CORRECT is set; '-' returns
 * each as option 1). optarg is the argument, optind the next word's
 * index (0 starts over), optopt the letter that failed. (46b) The four
 * are the library's variables, the one kind of data it exports (before,
 * macros onto the calls below, which programs built then still use). */
extern char *optarg;
extern int optind;
extern int opterr;
extern int optopt;
char **__ritteros_optarg(void);
int *__ritteros_optind(void);
int *__ritteros_opterr(void);
int *__ritteros_optopt(void);
int getopt(int argc, char *const argv[], const char *options);

/* (43a) access: whether the program's real user (not a setuid
 * program's) may read (R_OK), write (W_OK) or run (X_OK) path, or (F_OK)
 * whether it exists: 0, or -1 with errno set. Root may read and write
 * anything, and run what has any x bit; a group's rights go by the user's
 * group. */
#define F_OK    0
#define X_OK    1
#define W_OK    2
#define R_OK    4
int access(const char *path, int mode);

/* (43a-3) The system and the program:
 * - sysconf: a limit or option by name (_SC_*: RitterOS's limits, the
 *   cores the firmware lists and those started, the memory's pages), -1
 *   and EINVAL for a name it has not; pathconf/fpathconf a file system's
 *   (_PC_*); confstr: _CS_PATH, where the standard programs are;
 * - getpagesize (4096), getdtablesize (256 descriptors), gethostname
 *   (uname's nodename);
 * - getegid (the group: a program never runs as its file's group),
 *   getresuid/getresgid, getgroups (the user's one group), issetugid (a
 *   setuid program), getlogin/getlogin_r (the real user's name), ttyname
 *   and ttyname_r ("/dev/tty" for any terminal), nice (no priorities: 0);
 * - alarm/ualarm (SIGALRM after the seconds or microseconds: a thread of
 *   the library's waits for it), pause (until a signal is handled);
 * - daemon (a child in a new session, the parent gone; nochdir 0: to /,
 *   noclose 0: 0-2 to /dev/null), execvpe (execvp with an environment),
 *   vfork (fork: copy-on-write is as cheap), setpgrp (setpgid(0, 0));
 * - getentropy: up to 256 random bytes. */
#define _SC_ARG_MAX             0
#define _SC_CHILD_MAX           1
#define _SC_CLK_TCK             2
#define _SC_NGROUPS_MAX         3
#define _SC_OPEN_MAX            4
#define _SC_STREAM_MAX          5
#define _SC_TZNAME_MAX          6
#define _SC_JOB_CONTROL         7
#define _SC_SAVED_IDS           8
#define _SC_TIMERS              11
#define _SC_FSYNC               15
#define _SC_MAPPED_FILES        16
#define _SC_MEMORY_PROTECTION   19
#define _SC_SEMAPHORES          21
#define _SC_VERSION             29
#define _SC_PAGESIZE            30
#define _SC_PAGE_SIZE           _SC_PAGESIZE
#define _SC_SEM_NSEMS_MAX       32
#define _SC_SEM_VALUE_MAX       33
#define _SC_LINE_MAX            43
#define _SC_RE_DUP_MAX          44
#define _SC_IOV_MAX             60
#define _SC_THREADS             67
#define _SC_THREAD_SAFE_FUNCTIONS 68
#define _SC_GETGR_R_SIZE_MAX    69
#define _SC_GETPW_R_SIZE_MAX    70
#define _SC_LOGIN_NAME_MAX      71
#define _SC_TTY_NAME_MAX        72
#define _SC_THREAD_KEYS_MAX     74
#define _SC_THREAD_STACK_MIN    75
#define _SC_THREAD_THREADS_MAX  76
#define _SC_NPROCESSORS_CONF    83
#define _SC_NPROCESSORS_ONLN    84
#define _SC_PHYS_PAGES          85
#define _SC_AVPHYS_PAGES        86
#define _SC_MONOTONIC_CLOCK     149
#define _SC_SYMLOOP_MAX         173
#define _SC_HOST_NAME_MAX       180
#define _SC_LEVEL1_DCACHE_LINESIZE 190
#define _PC_LINK_MAX            0
#define _PC_MAX_CANON           1
#define _PC_MAX_INPUT           2
#define _PC_NAME_MAX            3
#define _PC_PATH_MAX            4
#define _PC_PIPE_BUF            5
#define _PC_CHOWN_RESTRICTED    6
#define _PC_NO_TRUNC            7
#define _PC_VDISABLE            8
#define _PC_FILESIZEBITS        13
#define _PC_SYMLINK_MAX         19
#define _CS_PATH                0
#define _POSIX_VERSION          200809L
#define _POSIX2_VERSION         200809L
#define _XOPEN_VERSION          700
#define _POSIX_THREADS          200809L
#define _POSIX_SEMAPHORES       200809L
#define _POSIX_TIMEOUTS         200809L
#define _POSIX_MONOTONIC_CLOCK  200809L
#define _POSIX_MAPPED_FILES     200809L
#define _POSIX_FSYNC            200809L
long sysconf(int name);
long pathconf(const char *path, int name);
long fpathconf(int fd, int name);
size_t confstr(int name, char *buf, size_t size);
int getpagesize(void);
int getdtablesize(void);
int gethostname(char *name, size_t size);
gid_t getegid(void);
int getresuid(uid_t *real, uid_t *effective, uid_t *saved);
int getresgid(gid_t *real, gid_t *effective, gid_t *saved);
int getgroups(int size, gid_t list[]);
int issetugid(void);
char *getlogin(void);
int getlogin_r(char *name, size_t size);
char *ttyname(int fd);
int ttyname_r(int fd, char *name, size_t size);
int nice(int increment);
unsigned int alarm(unsigned int seconds);
useconds_t ualarm(useconds_t microseconds, useconds_t interval);
int pause(void);
int daemon(int nochdir, int noclose);
int execvpe(const char *name, char *const argv[], char *const envp[]);
pid_t vfork(void);
int setpgrp(void);
int getentropy(void *buf, size_t size);

/* (43b-1) Files: pread/pwrite (at an offset, the position unmoved;
 * ESPIPE for a pipe or socket), truncate/ftruncate (a file's length;
 * longer: zeros), fsync/fdatasync (written data committed now), sync
 * (nothing waits uncommitted past a close), fchown/lchown/fchownat,
 * fchdir, dup3 (dup2 with O_CLOEXEC), pipe2 (O_CLOEXEC, O_NONBLOCK), and
 * the *at forms of access, readlink, unlink (rmdir with AT_REMOVEDIR),
 * link and symlink. */
ssize_t pread(int fd, void *buffer, size_t size, off_t offset);
ssize_t pwrite(int fd, const void *buffer, size_t size, off_t offset);
int truncate(const char *path, off_t length);
int ftruncate(int fd, off_t length);
int fsync(int fd);
/* (43b-4) lockf: fcntl's write locks from the descriptor's position over
 * length bytes (0: to the end; negative: before it): F_LOCK waits, F_TLOCK
 * does not (EAGAIN), F_ULOCK lets go, F_TEST says whether another
 * process holds one there (EACCES). */
#define F_ULOCK     0
#define F_LOCK      1
#define F_TLOCK     2
#define F_TEST      3
int lockf(int fd, int command, off_t length);
int fdatasync(int fd);
void sync(void);
int fchown(int fd, uid_t uid, gid_t gid);
int lchown(const char *path, uid_t uid, gid_t gid);
int fchownat(int dirfd, const char *path, uid_t uid, gid_t gid, int flags);
int fchdir(int fd);
int dup3(int fd, int newfd, int flags);
int pipe2(int fds[2], int flags);
int faccessat(int dirfd, const char *path, int mode, int flags);
ssize_t readlinkat(int dirfd, const char *path, char *buffer, size_t size);
int unlinkat(int dirfd, const char *path, int flags);
int linkat(int fromfd, const char *from, int tofd, const char *to, int flags);
int symlinkat(const char *target, int dirfd, const char *path);

#ifdef __cplusplus
}
#endif

#endif
