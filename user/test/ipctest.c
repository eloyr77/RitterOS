/*
 * Test: memory files and what goes with them (43b-3). `ipctest SCRATCH`
 * checks a memfd read, written, sized and mapped (the same bytes through
 * every way, two mappings, a forked child's writes, one passed over a
 * socket and mapped by the receiver); past its end SIGBUS, also once it is
 * cut down under a mapping, and a cut inside a page zeroes the rest; the
 * seals (none without MFD_ALLOW_SEALING; SHRINK, GROW, WRITE refused while
 * mapped writable, then writing and writable mappings refused; SEAL);
 * shm_open's names in /dev/shm (O_EXCL, a second opener's same bytes,
 * fstat, unlink, the mapping living on); madvise (anonymous memory zeroed,
 * shared memory kept, a private file mapping the file's again); named
 * semaphores across fork, an unnamed one in shared memory (the parent
 * sleeps in the kernel until the child posts: the futex keyed by the
 * memory), and a futex woken through another mapping of the same memory;
 * (43b-4) eventfd (counts, poll, its limit, EFD_SEMAPHORE, a read waiting
 * for a thread's write, a poll ended by a forked child's) and timerfd
 * (once: a poll wakes at its time; every 50 ms; disarmed; at a time, a
 * read waiting for it); and the pages given back when a memory file goes. SCRATCH is a path
 * prefix for the file it writes; `ipctest keep NAME` leaves one in
 * /dev/shm for the shell to see (ls, cat, rm); `ipctest ifaddrs` (43b-6)
 * prints every getifaddrs entry. (45b) SOCK_SEQPACKET: messages kept whole
 * (boundaries, a long one cut with MSG_TRUNC, 64 KiB the most: EMSGSIZE,
 * SO_TYPE, write and read whole, a named listener refusing a stream
 * client: EPROTOTYPE, a descriptor with its message, O_NONBLOCK never half
 * a message, the peer's close the end). (45e) Across programs, as a
 * browser's processes share them: a semaphore in a memory file and an
 * eventfd (EFD_SEMAPHORE) passed over a socket to `ipctest peer FD`, a
 * program of its own (posix_spawn, not fork: the memory at its own
 * address), which posts and writes; this one waits for both. Linux's
 * rules: no oracle on the Mac
 * (it has no memfd; its shm objects cannot be read or written).
 */
#include <errno.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "ritteros.h"

#define PAGE 4096

static int failures;

static void result(const char *name, int ok, const char *detail)
{
    if (ok) {
        printf("ipctest: %s ok%s%s\n", name, detail[0] ? " " : "", detail);
    } else {
        printf("ipctest: %s FAILED%s%s\n", name, detail[0] ? ": " : "", detail);
        failures++;
    }
}

static int all(const unsigned char *p, size_t n, unsigned char v)
{
    for (size_t i = 0; i < n; i++)
        if (p[i] != v)
            return 0;
    return 1;
}

static void basics(void)
{
    int fd = memfd_create("basics", MFD_CLOEXEC);
    char buf[64], detail[96];
    struct stat st;
    int cloexec = fd >= 0 && (fcntl(fd, F_GETFD) & FD_CLOEXEC);
    ssize_t w = write(fd, "hello, memory", 13);
    off_t at = lseek(fd, 0, SEEK_SET);
    ssize_t r = read(fd, buf, sizeof(buf));
    int ok = fd >= 0 && cloexec && w == 13 && at == 0 && r == 13 && !memcmp(buf, "hello, memory", 13);
    ok = ok && fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size == 13;
    snprintf(detail, sizeof(detail), "(fd %d, wrote %ld, read %ld, size %ld)", fd, (long)w, (long)r,
             (long)st.st_size);
    result("memfd read and write", ok, detail);

    ok = ftruncate(fd, 3 * PAGE) == 0 && fstat(fd, &st) == 0 && st.st_size == 3 * PAGE;
    memset(buf, 1, sizeof(buf));
    ok = ok && pread(fd, buf, 16, 2 * PAGE) == 16 && all((unsigned char *)buf, 16, 0);
    ok = ok && pwrite(fd, "at two", 6, 2 * PAGE + 100) == 6 && lseek(fd, 0, SEEK_CUR) == 13;
    ok = ok && lseek(fd, 0, SEEK_END) == 3 * PAGE && read(fd, buf, 1) == 0;
    result("ftruncate longer, pread, pwrite, the end", ok, "");

    unsigned char *a = mmap(0, 3 * PAGE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    unsigned char *b = mmap(0, 3 * PAGE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    ok = a != MAP_FAILED && b != MAP_FAILED && a != b && !memcmp(a, "hello, memory", 13) &&
         !memcmp(a + 2 * PAGE + 100, "at two", 6);
    if (ok) {
        memcpy(a + PAGE + 5, "through a", 9);
        ok = !memcmp(b + PAGE + 5, "through a", 9) && pread(fd, buf, 9, PAGE + 5) == 9 &&
             !memcmp(buf, "through a", 9);
        pwrite(fd, "written", 7, 50);
        ok = ok && !memcmp(a + 50, "written", 7) && !memcmp(b + 50, "written", 7);
    }
    result("mapped twice: one memory with read and write", ok, "");

    int pid = fork();
    if (pid == 0) {
        memcpy(b + 2 * PAGE, "the child", 9);
        _exit(0);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    result("a forked child's writes", pid > 0 && WIFEXITED(status) && !memcmp(a + 2 * PAGE, "the child", 9), "");

    unsigned char *view = mmap(0, PAGE, PROT_READ, MAP_PRIVATE, fd, 0);
    ok = view != MAP_FAILED && !memcmp(view, "hello", 5);
    errno = 0;
    void *refused = mmap(0, PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    snprintf(detail, sizeof(detail), "(writable: %s)", refused == MAP_FAILED ? strerror(errno) : "mapped");
    result("MAP_PRIVATE to read", ok && refused == MAP_FAILED && errno == ENOTSUP, detail);
    munmap(view, PAGE);
    munmap(a, 3 * PAGE);
    munmap(b, 3 * PAGE);
    close(fd);
}

/* A memfd made after the fork, passed to the child over a socket: it maps
 * it and writes; the parent sees the bytes. */
static void passing(void)
{
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
        result("passed over a socket and mapped", 0, strerror(errno));
        return;
    }
    int pid = fork();
    if (pid == 0) {
        close(sv[0]);
        char c;
        char room[CMSG_SPACE(sizeof(int))];
        struct iovec io = { &c, 1 };
        struct msghdr m = { 0 };
        m.msg_iov = &io;
        m.msg_iovlen = 1;
        m.msg_control = room;
        m.msg_controllen = sizeof(room);
        if (recvmsg(sv[1], &m, 0) != 1)
            _exit(2);
        struct cmsghdr *h = CMSG_FIRSTHDR(&m);
        if (!h || h->cmsg_type != SCM_RIGHTS)
            _exit(3);
        int fd;
        memcpy(&fd, CMSG_DATA(h), sizeof(fd));
        char *p = mmap(0, PAGE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (p == MAP_FAILED || memcmp(p, "from the parent", 15))
            _exit(4);
        memcpy(p + 100, "from the child", 14);
        _exit(0);
    }
    close(sv[1]);
    int fd = memfd_create("passed", 0);
    ftruncate(fd, PAGE);
    pwrite(fd, "from the parent", 15, 0);
    char c = 'x', room[CMSG_SPACE(sizeof(int))];
    memset(room, 0, sizeof(room));
    struct iovec io = { &c, 1 };
    struct msghdr m = { 0 };
    m.msg_iov = &io;
    m.msg_iovlen = 1;
    m.msg_control = room;
    m.msg_controllen = sizeof(room);
    struct cmsghdr *h = CMSG_FIRSTHDR(&m);
    h->cmsg_level = SOL_SOCKET;
    h->cmsg_type = SCM_RIGHTS;
    h->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(h), &fd, sizeof(fd));
    sendmsg(sv[0], &m, 0);
    int status = 0;
    waitpid(pid, &status, 0);
    char buf[16] = { 0 }, detail[48];
    pread(fd, buf, 14, 100);
    snprintf(detail, sizeof(detail), "(the child: %d)", WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    result("passed over a socket and mapped", WIFEXITED(status) && WEXITSTATUS(status) == 0 &&
                                              !memcmp(buf, "from the child", 14), detail);
    close(fd);
    close(sv[0]);
}

/* (45b) SOCK_SEQPACKET local sockets. */
static void packets(void)
{
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) < 0) {
        result("seqpacket: boundaries kept", 0, strerror(errno));
        return;
    }
    static char big[65537], got[65537];
    for (size_t i = 0; i < sizeof(big); i++)
        big[i] = (char)('a' + i % 26);
    ssize_t a = send(sv[0], big, 3, 0), b = send(sv[0], big, 5000, 0), c = send(sv[0], big + 7, 10, 0);
    ssize_t ra = recv(sv[1], got, sizeof(got), 0), rb = recv(sv[1], got + 3, sizeof(got), 0);
    ssize_t rc = recv(sv[1], got + 5003, sizeof(got), 0);
    char detail[96];
    snprintf(detail, sizeof(detail), "(sent %zd %zd %zd, received %zd %zd %zd)", a, b, c, ra, rb, rc);
    result("seqpacket: boundaries kept", a == 3 && b == 5000 && c == 10 && ra == 3 && rb == 5000 &&
                                         rc == 10 && !memcmp(got + 5003, big + 7, 10), detail);
    /* A long message into a short buffer: the rest is dropped, MSG_TRUNC. */
    send(sv[0], big, 100, 0);
    send(sv[0], "next", 4, 0);
    struct iovec io = { got, 40 };
    struct msghdr m = { 0 };
    m.msg_iov = &io;
    m.msg_iovlen = 1;
    ssize_t cut = recvmsg(sv[1], &m, 0);
    int truncated = (m.msg_flags & MSG_TRUNC) != 0;
    ssize_t next = recv(sv[1], got + 100, 100, 0);
    snprintf(detail, sizeof(detail), "(%zd, MSG_TRUNC %d, then %zd)", cut, truncated, next);
    result("seqpacket: a long message cut, MSG_TRUNC", cut == 40 && truncated && next == 4 &&
                                                       !memcmp(got + 100, "next", 4), detail);
    /* 64 KiB at most; one more byte is EMSGSIZE. */
    ssize_t whole = send(sv[0], big, 65536, 0);
    ssize_t taken = recv(sv[1], got, sizeof(got), 0);
    errno = 0;
    ssize_t over = send(sv[0], big, 65537, 0);
    int e = errno;
    snprintf(detail, sizeof(detail), "(65536: %zd, received %zd; 65537: %zd %s)", whole, taken, over,
             strerror(e));
    result("seqpacket: 64 KiB the most, then EMSGSIZE", whole == 65536 && taken == 65536 &&
                                                        !memcmp(got, big, 65536) && over < 0 && e == EMSGSIZE, detail);
    int type = 0;
    socklen_t tl = sizeof(type);
    result("seqpacket: SO_TYPE", getsockopt(sv[0], SOL_SOCKET, SO_TYPE, &type, &tl) == 0 &&
                                 type == SOCK_SEQPACKET, "");
    /* write and read take whole messages too (beyond their usual pieces). */
    ssize_t w = write(sv[0], big, 20000);
    ssize_t r = read(sv[1], got, sizeof(got));
    snprintf(detail, sizeof(detail), "(wrote %zd, read %zd)", w, r);
    result("seqpacket: write and read whole messages", w == 20000 && r == 20000 && !memcmp(got, big, 20000),
           detail);
    /* O_NONBLOCK: a message that does not fit is EAGAIN, never half sent. */
    fcntl(sv[0], F_SETFL, O_NONBLOCK);
    int sent = 0;
    while (send(sv[0], big, 30000, 0) == 30000 && sent < 100)
        sent++;
    e = errno;
    int whole_ones = 1, n = 0;
    fcntl(sv[1], F_SETFL, O_NONBLOCK);
    while ((r = recv(sv[1], got, sizeof(got), 0)) > 0) {
        whole_ones &= r == 30000;
        n++;
    }
    snprintf(detail, sizeof(detail), "(%d sent, then %s; %d received)", sent, strerror(e), n);
    result("seqpacket: O_NONBLOCK, never half a message", sent >= 1 && e == EAGAIN && n == sent && whole_ones,
           detail);
    fcntl(sv[1], F_SETFL, 0);
    /* The peer's close: its messages first, then the end. */
    fcntl(sv[0], F_SETFL, 0);
    send(sv[0], "last", 4, 0);
    close(sv[0]);
    ssize_t last = recv(sv[1], got, sizeof(got), 0), end = recv(sv[1], got, sizeof(got), 0);
    result("seqpacket: the peer's close after its messages", last == 4 && end == 0, "");
    close(sv[1]);
    /* A named listener: a stream client is refused, a packet one talks,
     * a descriptor goes with its message. */
    struct sockaddr_un addr = { 0 };
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, "/ipctest-seq");
    int l = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    int bound = bind(l, (struct sockaddr *)&addr, sizeof(addr)) == 0 && listen(l, 4) == 0;
    int stream = socket(AF_UNIX, SOCK_STREAM, 0);
    errno = 0;
    int refused = connect(stream, (struct sockaddr *)&addr, sizeof(addr)) < 0 && errno == EPROTOTYPE;
    close(stream);
    int client = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    int joined = connect(client, (struct sockaddr *)&addr, sizeof(addr)) == 0;
    int server = accept(l, 0, 0);
    int fd = memfd_create("with", 0);
    write(fd, "attached", 8);
    char room[CMSG_SPACE(sizeof(int))];
    memset(room, 0, sizeof(room));
    struct iovec out = { "hello", 5 };
    struct msghdr sm = { 0 };
    sm.msg_iov = &out;
    sm.msg_iovlen = 1;
    sm.msg_control = room;
    sm.msg_controllen = sizeof(room);
    struct cmsghdr *h = CMSG_FIRSTHDR(&sm);
    h->cmsg_level = SOL_SOCKET;
    h->cmsg_type = SCM_RIGHTS;
    h->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(h), &fd, sizeof(fd));
    ssize_t s1 = sendmsg(client, &sm, 0);
    send(client, "second", 6, 0);
    char in[16] = { 0 }, back[16] = { 0 }, room2[CMSG_SPACE(sizeof(int))];
    struct iovec iin = { in, sizeof(in) };
    struct msghdr rm = { 0 };
    rm.msg_iov = &iin;
    rm.msg_iovlen = 1;
    rm.msg_control = room2;
    rm.msg_controllen = sizeof(room2);
    ssize_t r1 = recvmsg(server, &rm, 0);
    struct cmsghdr *g = CMSG_FIRSTHDR(&rm);
    int passed = -1;
    if (g && g->cmsg_type == SCM_RIGHTS)
        memcpy(&passed, CMSG_DATA(g), sizeof(passed));
    ssize_t r2 = recv(server, in + 8, 8, 0);
    if (passed >= 0)
        pread(passed, back, 8, 0);
    snprintf(detail, sizeof(detail), "(bound %d, EPROTOTYPE %d, connected %d, %zd/%zd/%zd)", bound, refused,
             joined, s1, r1, r2);
    result("seqpacket: a named listener, a descriptor with its message",
           bound && refused && joined && server >= 0 && s1 == 5 && r1 == 5 && !memcmp(in, "hello", 5) &&
               r2 == 6 && !memcmp(back, "attached", 8), detail);
    if (passed >= 0)
        close(passed);
    close(fd);
    close(server);
    close(client);
    close(l);
}

/* In a child: touch the page, cut the file (cut_fd >= 0), touch it
 * again; how it ended. */
static int touch_in_child(volatile unsigned char *p, int cut_fd, off_t cut_to)
{
    int pid = fork();
    if (pid == 0) {
        (void)*p;                                   /* (mapped in this process now) */
        if (cut_fd >= 0)
            ftruncate(cut_fd, cut_to);
        (void)*p;
        _exit(0);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFSIGNALED(status) ? WTERMSIG(status) : -WEXITSTATUS(status);
}

static void ends(void)
{
    int fd = memfd_create("ends", 0);
    ftruncate(fd, PAGE);
    unsigned char *p = mmap(0, 2 * PAGE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    int past = p == MAP_FAILED ? -100 : touch_in_child(p + PAGE, -1, 0);
    char detail[64];
    snprintf(detail, sizeof(detail), "(signal %d)", past);
    result("past the end: SIGBUS", past == SIGBUS, detail);

    ftruncate(fd, 2 * PAGE);
    p[PAGE + 7] = 7;
    int before = touch_in_child(p + PAGE, -1, 0);
    int cut = touch_in_child(p + PAGE, fd, PAGE);   /* (the child cuts it, then touches) */
    struct stat st;
    fstat(fd, &st);
    snprintf(detail, sizeof(detail), "(before %d, after %d, size %ld)", before, cut, (long)st.st_size);
    result("cut down under a mapping: SIGBUS", before == 0 && cut == SIGBUS && st.st_size == PAGE, detail);

    memset(p, 0x5A, PAGE);
    ftruncate(fd, 100);
    ftruncate(fd, 2 * PAGE);
    int zeroed = all(p + 100, PAGE - 100, 0) && all(p + PAGE, PAGE, 0) && all(p, 100, 0x5A);
    result("a cut inside a page: the rest reads 0", zeroed, "");
    munmap(p, 2 * PAGE);
    close(fd);
}

static void seals(void)
{
    char detail[96];
    int plain = memfd_create("plain", 0);
    int g = fcntl(plain, F_GET_SEALS);
    errno = 0;
    int add = fcntl(plain, F_ADD_SEALS, F_SEAL_WRITE);
    snprintf(detail, sizeof(detail), "(seals %d, adding: %d %s)", g, add, strerror(errno));
    result("without MFD_ALLOW_SEALING: sealed", g == F_SEAL_SEAL && add == -1 && errno == EPERM, detail);
    close(plain);

    int fd = memfd_create("sealed", MFD_ALLOW_SEALING);
    ftruncate(fd, 2 * PAGE);
    int ok = fcntl(fd, F_GET_SEALS) == 0 && fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK) == 0;
    errno = 0;
    ok = ok && ftruncate(fd, PAGE) == -1 && errno == EPERM && ftruncate(fd, 3 * PAGE) == 0;
    result("F_SEAL_SHRINK", ok, "");
    ok = fcntl(fd, F_ADD_SEALS, F_SEAL_GROW) == 0;
    errno = 0;
    ok = ok && pwrite(fd, "x", 1, 3 * PAGE) == -1 && errno == EPERM && pwrite(fd, "x", 1, 0) == 1;
    errno = 0;
    ok = ok && ftruncate(fd, 4 * PAGE) == -1 && errno == EPERM;
    result("F_SEAL_GROW", ok, "");

    unsigned char *w = mmap(0, PAGE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    errno = 0;
    int busy = fcntl(fd, F_ADD_SEALS, F_SEAL_WRITE);
    int busy_errno = errno;
    munmap(w, PAGE);
    int sealed = fcntl(fd, F_ADD_SEALS, F_SEAL_WRITE);
    errno = 0;
    ssize_t written = pwrite(fd, "y", 1, 0);
    int write_errno = errno;
    errno = 0;
    void *writable = mmap(0, PAGE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    int map_errno = errno;
    unsigned char *ro = mmap(0, PAGE, PROT_READ, MAP_SHARED, fd, 0);
    errno = 0;
    int prot = ro == MAP_FAILED ? 0 : mprotect(ro, PAGE, PROT_READ | PROT_WRITE);
    int prot_errno = errno;
    snprintf(detail, sizeof(detail), "(busy %d/%d, write %ld/%d, map %d, mprotect %d/%d)", busy,
             busy_errno, (long)written, write_errno, writable == MAP_FAILED ? map_errno : 0, prot, prot_errno);
    result("F_SEAL_WRITE", busy == -1 && busy_errno == EBUSY && sealed == 0 && written == -1 &&
                           write_errno == EPERM && writable == MAP_FAILED && map_errno == EPERM &&
                           ro != MAP_FAILED && ro[0] == 'x' && prot == -1 && prot_errno == EPERM, detail);
    if (ro != MAP_FAILED)
        munmap(ro, PAGE);
    /* MAP_FIXED over a mapping: refused before anything is taken out. */
    unsigned char *old = mmap(0, PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    old[0] = 0x33;
    errno = 0;
    void *fixed = mmap(old, PAGE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0);
    int fixed_errno = errno;
    int kept = touch_in_child(old, -1, 0);
    snprintf(detail, sizeof(detail), "(%s, the old page in a child: %d)",
             fixed == MAP_FAILED ? strerror(fixed_errno) : "mapped", kept);
    result("F_SEAL_WRITE: MAP_FIXED refused, the old mapping kept",
           fixed == MAP_FAILED && fixed_errno == EPERM && kept == 0 && old[0] == 0x33, detail);
    munmap(old, PAGE);
    ok = fcntl(fd, F_ADD_SEALS, F_SEAL_SEAL) == 0 &&
         fcntl(fd, F_GET_SEALS) == (F_SEAL_SEAL | F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE);
    errno = 0;
    ok = ok && fcntl(fd, F_ADD_SEALS, F_SEAL_FUTURE_WRITE) == -1 && errno == EPERM;
    result("F_SEAL_SEAL", ok, "");
    close(fd);
}

static void names(void)
{
    char detail[96];
    shm_unlink("/ipctest");
    int fd = shm_open("/ipctest", O_RDWR | O_CREAT | O_EXCL, 0640);
    int cloexec = fd >= 0 && (fcntl(fd, F_GETFD) & FD_CLOEXEC);
    ftruncate(fd, PAGE);
    unsigned char *p = mmap(0, PAGE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p != MAP_FAILED)
        memcpy(p, "by name", 7);
    errno = 0;
    int again = shm_open("ipctest", O_RDWR | O_CREAT | O_EXCL, 0600);
    int again_errno = errno;
    int other = shm_open("ipctest", O_RDONLY, 0);
    char buf[8] = { 0 };
    struct stat st, path_st;
    int ok = fd >= 0 && cloexec && p != MAP_FAILED && again == -1 && again_errno == EEXIST && other >= 0 &&
             read(other, buf, 7) == 7 && !memcmp(buf, "by name", 7) && fstat(other, &st) == 0 &&
             st.st_size == PAGE && (st.st_mode & 0777) == 0640 && stat("/dev/shm/ipctest", &path_st) == 0 &&
             path_st.st_size == PAGE && S_ISREG(path_st.st_mode);
    errno = 0;
    int ro_write = (int)write(other, "x", 1);
    int ro_errno = errno;
    snprintf(detail, sizeof(detail), "(fd %d, again %d/%d, read-only write %d/%d)", fd, again, again_errno,
             ro_write, ro_errno);
    result("shm_open by name", ok && ro_write == -1 && ro_errno == EBADF, detail);
    int gone = shm_unlink("/ipctest");
    errno = 0;
    int after = shm_open("/ipctest", O_RDWR, 0);
    int after_errno = errno;
    ok = gone == 0 && after == -1 && after_errno == ENOENT && !memcmp(p, "by name", 7) &&
         pread(fd, buf, 7, 0) == 7 && !memcmp(buf, "by name", 7);
    errno = 0;
    int twice = shm_unlink("/ipctest");
    ok = ok && twice == -1 && errno == ENOENT;
    errno = 0;
    ok = ok && shm_open("/a/b", O_RDWR | O_CREAT, 0600) == -1 && errno == EINVAL;
    result("shm_unlink: the name gone, the memory kept", ok, "");
    munmap(p, PAGE);
    close(fd);
    close(other);
}

static void advice(const char *scratch)
{
    unsigned char *a = mmap(0, 4 * PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    memset(a, 0x55, 4 * PAGE);
    int ok = madvise(a + PAGE, 2 * PAGE, MADV_DONTNEED) == 0 && all(a, PAGE, 0x55) &&
             all(a + PAGE, 2 * PAGE, 0) && all(a + 3 * PAGE, PAGE, 0x55);
    a[PAGE] = 1;
    ok = ok && a[PAGE] == 1;
    result("madvise: anonymous memory zeroed", ok, "");
    munmap(a, 4 * PAGE);

    unsigned char *s = mmap(0, PAGE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    memset(s, 0x66, PAGE);
    result("madvise: shared memory kept", madvise(s, PAGE, MADV_DONTNEED) == 0 && all(s, PAGE, 0x66), "");
    munmap(s, PAGE);

    char path[256];
    snprintf(path, sizeof(path), "%sadvice", scratch);
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    unsigned char page[PAGE];
    memset(page, 0x77, PAGE);
    write(fd, page, PAGE);
    unsigned char *f = mmap(0, PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    ok = f != MAP_FAILED;
    if (ok) {
        memset(f, 0x11, 64);
        ok = f[0] == 0x11 && madvise(f, PAGE, MADV_FREE) == 0 && all(f, PAGE, 0x77);
        munmap(f, PAGE);
    }
    errno = 0;
    ok = ok && madvise((char *)page + 1, PAGE, MADV_DONTNEED) == -1 && errno == EINVAL;
    result("madvise: a private file mapping the file's again", ok, "");
    close(fd);
    unlink(path);
}

static double seconds_since(const struct timespec *t)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)(now.tv_sec - t->tv_sec) + (double)(now.tv_nsec - t->tv_nsec) / 1e9;
}

static struct timespec in_seconds(int n)
{
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    t.tv_sec += n;
    return t;
}

static void semaphores(void)
{
    char detail[96];
    sem_unlink("/ipctest");
    sem_t *s = sem_open("/ipctest", O_CREAT | O_EXCL, 0600, 0);
    errno = 0;
    sem_t *again = sem_open("/ipctest", O_CREAT | O_EXCL, 0600, 0);
    int again_errno = errno;
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int pid = fork();
    if (pid == 0) {
        sem_t *c = sem_open("/ipctest", 0);
        usleep(200000);
        _exit(c != SEM_FAILED && sem_post(c) == 0 ? 0 : 1);
    }
    struct timespec until = in_seconds(5);
    int waited = s == SEM_FAILED ? -1 : sem_timedwait(s, &until);
    double took = seconds_since(&t0);
    int status = 0;
    waitpid(pid, &status, 0);
    snprintf(detail, sizeof(detail), "(again %s, waited %d after %.2f s, the child %d)",
             again == SEM_FAILED ? strerror(again_errno) : "opened", waited, took,
             WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    result("sem_open across fork", s != SEM_FAILED && again == SEM_FAILED && again_errno == EEXIST &&
                                   waited == 0 && took >= 0.15 && took < 4 && WIFEXITED(status) &&
                                   !WEXITSTATUS(status),
           detail);
    int value = -1;
    sem_getvalue(s, &value);
    result("sem_close, sem_unlink", value == 0 && sem_close(s) == 0 && sem_unlink("/ipctest") == 0 &&
                                    sem_open("/ipctest", 0) == SEM_FAILED && errno == ENOENT, "");

    sem_t *u = mmap(0, PAGE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    int init = sem_init(u, 1, 0);
    clock_gettime(CLOCK_MONOTONIC, &t0);
    pid = fork();
    if (pid == 0) {
        usleep(200000);
        _exit(sem_post(u) == 0 ? 0 : 1);
    }
    until = in_seconds(5);
    waited = sem_timedwait(u, &until);
    int wait_errno = errno;
    took = seconds_since(&t0);
    waitpid(pid, &status, 0);
    snprintf(detail, sizeof(detail), "(init %d, waited %d/%d after %.2f s)", init, waited,
             waited ? wait_errno : 0, took);
    result("an unnamed semaphore in shared memory", init == 0 && waited == 0 && took >= 0.15 && took < 4,
           detail);
    munmap(u, PAGE);
}

/* A thread waits on the word through one mapping, the main thread wakes
 * it through another mapping of the same memory file. */
static unsigned *wait_word;

static void *waiter(void *arg)
{
    (void)arg;
    int64_t r = syscall6(SYS_FUTEX, (int64_t)wait_word, FUTEX_WAIT, 0, 3000000000LL, 0, 0);
    return (void *)(intptr_t)r;
}

static void futex_through_two(void)
{
    int fd = memfd_create("futex", 0);
    ftruncate(fd, PAGE);
    unsigned *a = mmap(0, PAGE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    unsigned *b = mmap(0, PAGE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    wait_word = a + 10;
    pthread_t t;
    pthread_create(&t, 0, waiter, 0);
    usleep(200000);
    __atomic_store_n(b + 10, 1, __ATOMIC_SEQ_CST);
    int64_t woken = syscall6(SYS_FUTEX, (int64_t)(b + 10), FUTEX_WAKE, 1, 0, 0, 0);
    void *r;
    pthread_join(t, &r);
    char detail[64];
    snprintf(detail, sizeof(detail), "(woke %ld, the wait %ld)", (long)woken, (long)(intptr_t)r);
    result("a futex woken through another mapping", woken == 1 && (intptr_t)r == 0, detail);
    munmap(a, PAGE);
    munmap(b, PAGE);
    close(fd);
}

/* --- (43b-4) eventfd and timerfd --- */

static int event_fd;

static void *late_writer(void *arg)
{
    (void)arg;
    usleep(200000);
    eventfd_write(event_fd, 4);
    return 0;
}

static void events(void)
{
    char detail[128];
    int fd = eventfd(3, EFD_CLOEXEC | EFD_NONBLOCK);
    eventfd_t v = 0, v2 = 0;
    int ok = fd >= 0 && (fcntl(fd, F_GETFD) & FD_CLOEXEC) && eventfd_read(fd, &v) == 0 && v == 3;
    ok = ok && eventfd_write(fd, 5) == 0 && eventfd_write(fd, 2) == 0 && eventfd_read(fd, &v2) == 0 && v2 == 7;
    errno = 0;
    int empty = eventfd_read(fd, &v);
    int empty_errno = errno;
    errno = 0;
    char small[4];
    ssize_t short_read = read(fd, small, sizeof(small));
    int short_errno = errno;
    uint64_t all_ones = ~0ULL;
    errno = 0;
    ssize_t bad = write(fd, &all_ones, 8);
    int bad_errno = errno;
    snprintf(detail, sizeof(detail), "(%lu then %lu; empty %d/%d, 4 bytes %ld/%d, 2^64-1 %ld/%d)",
             (unsigned long)v, (unsigned long)v2, empty, empty_errno, (long)short_read, short_errno,
             (long)bad, bad_errno);
    result("eventfd counts", ok && empty == -1 && empty_errno == EAGAIN && short_read == -1 &&
                             short_errno == EINVAL && bad == -1 && bad_errno == EINVAL, detail);

    struct pollfd pf = { fd, POLLIN | POLLOUT, 0 };
    int n0 = poll(&pf, 1, 0);
    int r0 = pf.revents;
    eventfd_write(fd, 0xFFFFFFFFFFFFFFFEULL);
    pf.revents = 0;
    int n1 = poll(&pf, 1, 0);
    int r1 = pf.revents;
    errno = 0;
    int over = eventfd_write(fd, 1);
    int over_errno = errno;
    eventfd_read(fd, &v);
    snprintf(detail, sizeof(detail), "(empty %d/%x, full %d/%x, one more %d/%d)", n0, r0, n1, r1, over,
             over_errno);
    result("eventfd poll and its limit", n0 == 1 && r0 == POLLOUT && n1 == 1 && r1 == POLLIN && over == -1 &&
                                         over_errno == EAGAIN && v == 0xFFFFFFFFFFFFFFFEULL, detail);
    close(fd);

    int s = eventfd(2, EFD_SEMAPHORE | EFD_NONBLOCK);
    eventfd_t a = 0, b = 0;
    int got = eventfd_read(s, &a) == 0 && eventfd_read(s, &b) == 0;
    errno = 0;
    result("EFD_SEMAPHORE", got && a == 1 && b == 1 && eventfd_read(s, &a) == -1 && errno == EAGAIN, "");
    close(s);

    event_fd = eventfd(0, 0);
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    pthread_t th;
    pthread_create(&th, 0, late_writer, 0);
    int r = eventfd_read(event_fd, &v);
    double took = seconds_since(&t0);
    pthread_join(th, 0);
    snprintf(detail, sizeof(detail), "(%lu after %.2f s)", (unsigned long)v, took);
    result("eventfd read waits for a write", r == 0 && v == 4 && took >= 0.15 && took < 3, detail);

    int pid = fork();
    if (pid == 0) {
        usleep(200000);
        _exit(eventfd_write(event_fd, 9) == 0 ? 0 : 1);
    }
    clock_gettime(CLOCK_MONOTONIC, &t0);
    struct pollfd wait_in = { event_fd, POLLIN, 0 };
    int n = poll(&wait_in, 1, 3000);
    took = seconds_since(&t0);
    eventfd_read(event_fd, &v);
    int status = 0;
    waitpid(pid, &status, 0);
    snprintf(detail, sizeof(detail), "(poll %d %x after %.2f s, %lu)", n, wait_in.revents, took, (unsigned long)v);
    result("eventfd: a forked child's write ends a poll", n == 1 && (wait_in.revents & POLLIN) && v == 9 &&
                                                         took >= 0.15 && took < 2.5, detail);
    close(event_fd);
}

static struct itimerspec in_ms(long first, long every)
{
    struct itimerspec s;
    s.it_value.tv_sec = first / 1000;
    s.it_value.tv_nsec = first % 1000 * 1000000;
    s.it_interval.tv_sec = every / 1000;
    s.it_interval.tv_nsec = every % 1000 * 1000000;
    return s;
}

static void timers(void)
{
    char detail[128];
    int fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    struct itimerspec now;
    int disarmed = fd >= 0 && timerfd_gettime(fd, &now) == 0 && !now.it_value.tv_sec && !now.it_value.tv_nsec;
    struct itimerspec once = in_ms(100, 0);
    timerfd_settime(fd, 0, &once, 0);
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    struct pollfd pf = { fd, POLLIN, 0 };
    int n = poll(&pf, 1, 2000);
    double took = seconds_since(&t0);
    uint64_t count = 0;
    int got = read(fd, &count, 8) == 8;
    errno = 0;
    int again = (int)read(fd, &count, 8);
    int again_errno = errno;
    snprintf(detail, sizeof(detail), "(poll %d after %.2f s, read %d, again %d/%d)", n, took, got, again,
             again_errno);
    result("timerfd once: poll wakes at its time", disarmed && n == 1 && took >= 0.08 && took < 1 && got &&
                                                 again == -1 && again_errno == EAGAIN, detail);

    struct itimerspec every = in_ms(50, 50), left;
    timerfd_settime(fd, 0, &every, 0);
    usleep(320000);
    count = 0;
    read(fd, &count, 8);
    timerfd_gettime(fd, &left);
    snprintf(detail, sizeof(detail), "(%lu expirations in 0.32 s, left %ld ns, interval %ld ns)",
             (unsigned long)count, left.it_value.tv_nsec, left.it_interval.tv_nsec);
    result("timerfd every 50 ms", count >= 4 && count <= 7 && left.it_value.tv_sec == 0 &&
                                  left.it_value.tv_nsec <= 50000000 && left.it_interval.tv_nsec == 50000000,
           detail);

    struct itimerspec off = in_ms(0, 0), before;
    timerfd_settime(fd, 0, &off, &before);
    pf.revents = 0;
    n = poll(&pf, 1, 300);
    result("timerfd disarmed", n == 0 && before.it_interval.tv_nsec == 50000000, "");
    close(fd);

    int blocking = timerfd_create(CLOCK_REALTIME, 0);
    struct timespec at;
    clock_gettime(CLOCK_REALTIME, &at);
    at.tv_nsec += 150000000;
    if (at.tv_nsec >= 1000000000)
        at.tv_sec++, at.tv_nsec -= 1000000000;
    struct itimerspec abs = { { 0, 0 }, at };
    timerfd_settime(blocking, TFD_TIMER_ABSTIME, &abs, 0);
    clock_gettime(CLOCK_MONOTONIC, &t0);
    count = 0;
    ssize_t r = read(blocking, &count, 8);
    took = seconds_since(&t0);
    snprintf(detail, sizeof(detail), "(%ld: %lu after %.2f s)", (long)r, (unsigned long)count, took);
    result("timerfd at a time: read waits for it", r == 8 && count == 1 && took >= 0.1 && took < 1.5, detail);
    close(blocking);
    errno = 0;
    result("timerfd_create: an unknown clock", timerfd_create(99, 0) == -1 && errno == EINVAL, "");
}

static uint64_t free_pages(void)
{
    struct sys_info info;
    return ritteros_sysinfo(&info) == 0 ? info.free_pages : 0;
}

/* (45e) The other program: the memory file and the eventfd from the
 * socket fd, the semaphore posted (where it maps the memory), 7 written,
 * and that address sent back. */
static int peer(int fd)
{
    char c, room[CMSG_SPACE(2 * sizeof(int))];
    struct iovec io = { &c, 1 };
    struct msghdr m = { 0 };
    m.msg_iov = &io;
    m.msg_iovlen = 1;
    m.msg_control = room;
    m.msg_controllen = sizeof(room);
    if (recvmsg(fd, &m, 0) != 1)
        return 2;
    struct cmsghdr *h = CMSG_FIRSTHDR(&m);
    int fds[2];
    if (!h || h->cmsg_type != SCM_RIGHTS || h->cmsg_len != CMSG_LEN(2 * sizeof(int)))
        return 3;
    memcpy(fds, CMSG_DATA(h), sizeof(fds));
    sem_t *sem = mmap(0, PAGE, PROT_READ | PROT_WRITE, MAP_SHARED, fds[0], 0);
    if (sem == MAP_FAILED)
        return 4;
    usleep(100000);                             /* (the other waits in the kernel meanwhile) */
    if (sem_post(sem) != 0 || eventfd_write(fds[1], 7) != 0)
        return 5;
    char where[32];
    int n = snprintf(where, sizeof(where), "%p", (void *)sem);
    write(fd, where, (size_t)n);
    return 0;
}

static void across(const char *self)
{
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) < 0) {
        result("a semaphore and an eventfd across programs", 0, strerror(errno));
        return;
    }
    int mem = memfd_create("shared semaphore", 0);
    ftruncate(mem, PAGE);
    sem_t *sem = mmap(0, PAGE, PROT_READ | PROT_WRITE, MAP_SHARED, mem, 0);
    int ok = sem != MAP_FAILED && sem_init(sem, 1, 0) == 0;
    int efd = eventfd(0, EFD_SEMAPHORE);
    char number[16];
    snprintf(number, sizeof(number), "%d", sv[1]);
    char *argv[] = { (char *)self, "peer", number, 0 };
    pid_t pid = -1;
    ok = ok && efd >= 0 && posix_spawn(&pid, self, 0, 0, argv, environ) == 0;
    close(sv[1]);
    char c = 'x', room[CMSG_SPACE(2 * sizeof(int))];
    memset(room, 0, sizeof(room));
    struct iovec io = { &c, 1 };
    struct msghdr m = { 0 };
    m.msg_iov = &io;
    m.msg_iovlen = 1;
    m.msg_control = room;
    m.msg_controllen = sizeof(room);
    struct cmsghdr *h = CMSG_FIRSTHDR(&m);
    h->cmsg_level = SOL_SOCKET;
    h->cmsg_type = SCM_RIGHTS;
    h->cmsg_len = CMSG_LEN(2 * sizeof(int));
    int pass[2] = { mem, efd };
    memcpy(CMSG_DATA(h), pass, sizeof(pass));
    ok = ok && sendmsg(sv[0], &m, 0) == 1;
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += 5;
    struct timespec w0, w1;
    clock_gettime(CLOCK_MONOTONIC, &w0);
    int value_before = -1;
    sem_getvalue(sem, &value_before);
    int posted = ok && sem_timedwait(sem, &until) == 0;
    clock_gettime(CLOCK_MONOTONIC, &w1);
    long waited = (long)((w1.tv_sec - w0.tv_sec) * 1000 + (w1.tv_nsec - w0.tv_nsec) / 1000000);
    eventfd_t value = 0;
    int counted = ok && read(efd, &value, sizeof(value)) == (ssize_t)sizeof(value);
    char where[32] = "";
    ssize_t n = ok ? read(sv[0], where, sizeof(where) - 1) : 0;
    if (n > 0)
        where[n] = 0;
    int status = 0;
    if (pid > 0)
        waitpid(pid, &status, 0);
    char mine[32], detail[160];
    snprintf(mine, sizeof(mine), "%p", (void *)sem);
    snprintf(detail, sizeof(detail), "(posted %d, eventfd %d, the other ended %d, mapped elsewhere %d; "
             "the count before the wait %d, waited %ld ms)", posted, counted && value == 1,
             WIFEXITED(status) ? WEXITSTATUS(status) : -1, strcmp(mine, where) != 0, value_before, waited);
    /* (EFD_SEMAPHORE: a read takes 1 of the 7; woken by the post, not by
     * the wait's end: a timed wait that ends finds the post too) */
    result("a semaphore and an eventfd across programs", posted && waited < 2000 && counted && value == 1 &&
                                                         WIFEXITED(status) &&
                                                         WEXITSTATUS(status) == 0 && where[0] &&
                                                         strcmp(mine, where) != 0, detail);
    if (sem != MAP_FAILED)
        munmap(sem, PAGE);
    close(mem);
    close(efd);
    close(sv[0]);
}

static void given_back(void)
{
    uint64_t before = free_pages();
    int fd = memfd_create("big", 0);
    static unsigned char block[64 * 1024];
    memset(block, 0x42, sizeof(block));
    for (int i = 0; i < 64; i++)
        write(fd, block, sizeof(block));            /* 4 MiB: 1,024 pages */
    unsigned char *p = mmap(0, 1024 * PAGE, PROT_READ, MAP_SHARED, fd, 0);
    int seen = p != MAP_FAILED && p[1023 * PAGE] == 0x42;
    uint64_t during = free_pages();
    munmap(p, 1024 * PAGE);
    close(fd);
    uint64_t after = free_pages();
    char detail[96];
    snprintf(detail, sizeof(detail), "(free pages %lu, with it %lu, after %lu)", (unsigned long)before,
             (unsigned long)during, (unsigned long)after);
    result("pages given back", seen && during + 1000 <= before && after + 64 >= before, detail);
}

/* (43b-6) For the shell on a machine with cards: every getifaddrs entry. */
static int list_ifaddrs(void)
{
    struct ifaddrs *list, *a;
    if (getifaddrs(&list) < 0)
        return 1;
    for (a = list; a; a = a->ifa_next) {
        char addr[64] = "-", mask[64] = "-", bcast[64] = "";
        int family = a->ifa_addr ? a->ifa_addr->sa_family : 0;
        if (family == AF_INET) {
            inet_ntop(AF_INET, &((struct sockaddr_in *)a->ifa_addr)->sin_addr, addr, sizeof(addr));
            inet_ntop(AF_INET, &((struct sockaddr_in *)a->ifa_netmask)->sin_addr, mask, sizeof(mask));
            if (a->ifa_broadaddr) {
                strcpy(bcast, " broadcast ");
                inet_ntop(AF_INET, &((struct sockaddr_in *)a->ifa_broadaddr)->sin_addr, bcast + 11,
                          sizeof(bcast) - 11);
            }
        } else if (family == AF_INET6) {
            struct sockaddr_in6 *s = (struct sockaddr_in6 *)a->ifa_addr;
            inet_ntop(AF_INET6, &s->sin6_addr, addr, sizeof(addr));
            inet_ntop(AF_INET6, &((struct sockaddr_in6 *)a->ifa_netmask)->sin6_addr, mask, sizeof(mask));
            if (s->sin6_scope_id)
                snprintf(bcast, sizeof(bcast), " scope %u", s->sin6_scope_id);
        }
        printf("ipctest: ifaddrs %s %s %s %s%s%s%s index %u\n", a->ifa_name,
               family == AF_INET ? "inet" : family == AF_INET6 ? "inet6" : "none", addr, mask, bcast,
               a->ifa_flags & IFF_LOOPBACK ? " loopback" : "", a->ifa_flags & IFF_RUNNING ? " running" : "",
               if_nametoindex(a->ifa_name));
    }
    freeifaddrs(list);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "ifaddrs"))
        return list_ifaddrs();
    if (argc == 3 && !strcmp(argv[1], "peer"))
        return peer(atoi(argv[2]));             /* (45e) */
    if (argc == 3 && !strcmp(argv[1], "keep")) {
        /* For the shell: a memory file left in /dev/shm under a name. */
        int fd = shm_open(argv[2], O_RDWR | O_CREAT, 0644);
        if (fd < 0 || write(fd, "kept in memory\n", 15) != 15)
            return 1;
        return 0;
    }
    if (argc != 2) {
        fprintf(stderr, "usage: ipctest SCRATCH | ipctest keep NAME\n");
        return 2;
    }
    basics();
    passing();
    packets();
    ends();
    seals();
    names();
    advice(argv[1]);
    semaphores();
    futex_through_two();
    events();
    timers();
    across(argv[0]);
    given_back();
    printf(failures ? "ipctest: %d FAILED\n" : "ipctest: all ok\n", failures);
    return failures != 0;
}
