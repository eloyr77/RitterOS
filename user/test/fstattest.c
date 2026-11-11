/*
 * Test: open's mode and fstat (52b, SYS_FSTAT). Each check prints
 * "fstattest: NAME ok" or "fstattest: FAIL ...":
 *  - a file: fstat of a descriptor matches stat of its path, and sees a
 *    size written through it before it is closed.
 *  - the mode asked for: O_CREAT's mode becomes the file's (less 022, as
 *    Unix's usual umask), creat too; an existing file keeps its own.
 *  - a pipe: both ends are S_ISFIFO.
 *  - the terminal: standard input at the console is S_ISCHR.
 *  - /dev/null (52c): stat and fstat say S_ISCHR, mode 0666; /dev/tty is
 *    a terminal.
 *  - exclusive (52j): O_CREAT | O_EXCL makes a new file and refuses a name
 *    taken (EEXIST; /dev/null too), fopen's "wx" the same, mkstemp makes
 *    two different files of mode 0600.
 * Usage: fstattest DIR (a folder it may write files in).
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failed;

static void result(const char *name, int ok, const char *detail)
{
    if (ok)
        printf("fstattest: %s ok\n", name);
    else {
        printf("fstattest: FAIL %s: %s (errno %d)\n", name, detail, errno);
        failed = 1;
    }
}

static unsigned mode_of(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 ? st.st_mode & 07777 : 07777;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        printf("usage: fstattest DIR\n");
        return 2;
    }
    char path[200];
    struct stat a, b;

    snprintf(path, sizeof(path), "%s/fstat-file.txt", argv[1]);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    int ok = fd >= 0 && write(fd, "hello", 5) == 5 && fstat(fd, &a) == 0;
    close(fd);
    ok &= stat(path, &b) == 0 && S_ISREG(a.st_mode) && a.st_size == 5 && a.st_ino == b.st_ino &&
          a.st_dev == b.st_dev && (a.st_mode & 0777) == 0644;
    result("a file", ok, "fstat and stat disagree, or the size is not 5");

    static const struct { const char *name; unsigned mode, want; } modes[] = {
        { "m600", 0600, 0600 }, { "m666", 0666, 0644 }, { "m777", 0777, 0755 }, { "m0", 0, 0 },
    };
    ok = 1;
    for (int i = 0; i < 4; i++) {
        snprintf(path, sizeof(path), "%s/fstat-%s", argv[1], modes[i].name);
        fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, modes[i].mode);
        close(fd);
        if (fd < 0 || mode_of(path) != modes[i].want) {
            printf("fstattest: %s is %o, not %o\n", path, mode_of(path), modes[i].want);
            ok = 0;
        }
    }
    snprintf(path, sizeof(path), "%s/fstat-creat", argv[1]);
    fd = creat(path, 0640);
    close(fd);
    ok &= fd >= 0 && mode_of(path) == 0640;
    fd = open(path, O_WRONLY | O_CREAT, 0600);     /* (it exists: its mode stays) */
    close(fd);
    ok &= mode_of(path) == 0640;
    result("the mode asked for", ok, "a created file's mode");

    int p[2];
    ok = pipe(p) == 0 && fstat(p[0], &a) == 0 && fstat(p[1], &b) == 0 &&
         S_ISFIFO(a.st_mode) && S_ISFIFO(b.st_mode);
    close(p[0]);
    close(p[1]);
    result("a pipe", ok, "a pipe's ends are not S_ISFIFO");

    ok = fstat(0, &a) == 0 && S_ISCHR(a.st_mode) && fstat(9, &b) == -1 && errno == EBADF;
    result("the terminal", ok, "standard input is not S_ISCHR, or fd 9 was not EBADF");

    fd = open("/dev/null", O_RDWR);
    ok = fd >= 0 && fstat(fd, &a) == 0 && S_ISCHR(a.st_mode) && stat("/dev/null", &b) == 0 &&
         S_ISCHR(b.st_mode) && (b.st_mode & 0777) == 0666 && write(fd, "x", 1) == 1 &&
         read(fd, path, 1) == 0;
    close(fd);
    fd = open("/dev/tty", O_WRONLY);
    ok &= fd >= 0 && isatty(fd);
    close(fd);
    result("/dev/null", ok, "/dev/null is not a character device, or /dev/tty not a terminal");

    snprintf(path, sizeof(path), "%s/fstat-excl", argv[1]);
    unlink(path);
    fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
    ok = fd >= 0;
    close(fd);
    errno = 0;
    ok &= open(path, O_WRONLY | O_CREAT | O_EXCL, 0644) == -1 && errno == EEXIST;
    errno = 0;
    ok &= open("/dev/null", O_WRONLY | O_CREAT | O_EXCL, 0644) == -1 && errno == EEXIST;
    errno = 0;
    ok &= fopen(path, "wx") == 0 && errno == EEXIST;
    snprintf(path, sizeof(path), "%s/fstat-excl2", argv[1]);
    unlink(path);
    FILE *x = fopen(path, "wx");
    ok &= x != 0;
    if (x)
        fclose(x);
    char t1[200], t2[200];
    snprintf(t1, sizeof(t1), "%s/tmpXXXXXX", argv[1]);
    snprintf(t2, sizeof(t2), "%s/tmpXXXXXX", argv[1]);
    int f1 = mkstemp(t1), f2 = mkstemp(t2);
    ok &= f1 >= 0 && f2 >= 0 && strcmp(t1, t2) != 0 && mode_of(t1) == 0600 && mode_of(t2) == 0600;
    close(f1);
    close(f2);
    unlink(t1);
    unlink(t2);
    result("exclusive", ok, "O_EXCL, fopen's wx or mkstemp");

    printf(failed ? "fstattest: FAIL (see above)\n" : "fstattest: all ok\n");
    return failed;
}
