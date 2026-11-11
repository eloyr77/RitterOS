/* Test: permissions. As root it prepares /perm, then becomes user 1000 and
 * tries what the files' owners and rights allow or refuse. */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include "ritteros.h"
#include <signal.h>

static char bin[128];

static void report(const char *what, long result)
{
    if (result < 0)
        printf("perm: %s: -1, %s\n", what, strerror(errno));
    else
        printf("perm: %s: %ld\n", what, result);
}

static void put(const char *path, const char *text, mode_t mode)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    write(fd, text, strlen(text));
    close(fd);
    chmod(path, mode);
}

static long run(const char *path, char *const argv[])
{
    pid_t pid = spawn(path, argv);
    if (pid < 0)
        return -1;
    int status;
    waitpid(pid, &status, 0);
    return WEXITSTATUS(status);
}

int main(int argc, char **argv)
{
    /* The programs' directory: /system/<arch>/bin, next to our test/. */
    (void)argc;
    snprintf(bin, sizeof(bin), "%s", argv[0]);
    char *slash = strrchr(bin, '/');
    if (slash && slash - bin >= 4)
        strcpy(slash - 4, "bin");
    char echo[160], id[160];
    snprintf(echo, sizeof(echo), "%s/echo", bin);
    snprintf(id, sizeof(id), "%s/id", bin);

    /* As root: the stage. */
    mkdir("/perm", 0755);
    put("/perm/secret", "only root\n", 0600);
    put("/perm/open.txt", "for everyone\n", 0644);
    mkdir("/perm/user", 0755);
    chown("/perm/user", 1000, 1000);
    mkdir("/perm/closed", 0755);
    put("/perm/closed/in.txt", "inside\n", 0644);
    chmod("/perm/closed", 0700);
    ritteros_copy(echo, "/perm/echo-run");
    ritteros_copy(echo, "/perm/echo-norun");
    chmod("/perm/echo-norun", 0644);
    /* A directory the user may read, holding a file only root may read. */
    mkdir("/perm/mixed", 0755);
    put("/perm/mixed/open.txt", "readable\n", 0644);
    put("/perm/mixed/secret", "only root\n", 0600);

    report("setgid 1000", setgid(1000));
    report("setuid 1000", setuid(1000));
    char *id_argv[] = { "id", 0 };
    report("id as the user (exit code)", run(id, id_argv));

    char buf[64];
    int fd = open("/perm/secret", O_RDONLY);
    report("read root's 0600 file", fd);
    fd = open("/perm/open.txt", O_RDONLY);
    long n = fd < 0 ? -1 : read(fd, buf, sizeof(buf));
    close(fd);
    report("read a 0644 file (bytes)", n);
    report("open it for writing", open("/perm/open.txt", O_WRONLY));
    report("create in root's directory", open("/perm/new.txt", O_WRONLY | O_CREAT, 0644));
    report("mkdir in root's directory", mkdir("/perm/x", 0755));
    report("remove root's file", unlink("/perm/open.txt"));
    report("rename root's file", rename("/perm/open.txt", "/perm/user/taken.txt"));
    fd = open("/perm/user/mine.txt", O_WRONLY | O_CREAT, 0644);
    report("create in my directory", fd);
    write(fd, "mine\n", 5);
    close(fd);
    struct stat st;
    stat("/perm/user/mine.txt", &st);
    printf("perm: my new file: owner %u, group %u, mode %o\n", st.st_uid, st.st_gid,
           st.st_mode & 0777);
    report("chmod my file", chmod("/perm/user/mine.txt", 0600));
    report("chmod root's file", chmod("/perm/open.txt", 0777));
    report("chown my file to root", chown("/perm/user/mine.txt", 0, 0));
    report("stat through a 0700 directory", stat("/perm/closed/in.txt", &st));
    report("list a 0700 directory", opendir("/perm/closed") ? 0 : -1);
    report("cd into it", chdir("/perm/closed"));
    char *echo_argv[] = { "echo", "an allowed program ran", 0 };
    report("run a 0644 copy of echo", run("/perm/echo-norun", echo_argv));
    report("run a 0755 copy (exit code)", run("/perm/echo-run", echo_argv));
    report("copy root's secret", ritteros_copy("/perm/secret", "/perm/user/stolen"));
    report("copy a readable file into my directory", ritteros_copy("/perm/open.txt",
                                                                   "/perm/user/copy.txt"));
    stat("/perm/user/copy.txt", &st);
    printf("perm: the copy's owner: %u\n", st.st_uid);
    report("copy a directory holding a root-only file", ritteros_copy("/perm/mixed",
                                                                      "/perm/user/mixed"));
    report("signal the root process 1", kill(1, 15));
    int sock = ritteros_socket(SOCKET_TCP);
    report("bind a port below 1024", ritteros_bind(sock, 80));
    report("bind a high port", ritteros_bind(sock, 50000));
    close(sock);
    report("attribute on root's file", ritteros_attr(ATTR_SET, "/perm/open.txt", "k", "v", 1));
    report("read an attribute of root's 0600 file",
           ritteros_attr(ATTR_GET, "/perm/secret", "k", buf, sizeof(buf)));
    report("take a snapshot", ritteros_snapshot(SNAPSHOT_CREATE, "mine", 0, 0));
    report("back to root", setuid(0));
    report("switch the machine off", ritteros_power(POWER_OFF));
    report("remove my file", unlink("/perm/user/mine.txt"));
    return 0;
}
