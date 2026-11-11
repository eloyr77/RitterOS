/* Reading, writing and closing file descriptors. */
#ifndef RITTEROS_UNISTD_H
#define RITTEROS_UNISTD_H

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
pid_t tcgetpgrp(int fd);
int tcsetpgrp(int fd, pid_t group);

#endif
