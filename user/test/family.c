/*
 * Test: processes. Children run side by side (their output interleaves), a
 * child that never ends does not stop the others, and wait reports how each
 * child ended: its exit code, or the signal that stopped it.
 */
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static char system_dir[128];            /* "/system/<arch>", from argv[0] */

static pid_t start(const char *where, const char *name, char *a, char *b, char *c)
{
    char path[192];
    snprintf(path, sizeof(path), "%s/%s/%s", system_dir, where, name);
    char *argv[] = { (char *)name, a, b, c, 0 };
    return spawn(path, argv);
}

static void report(pid_t pid, const char *name)
{
    int status;
    pid_t id = waitpid(pid, &status, 0);
    if (id != pid)
        printf("family: waiting for %s failed: %s\n", name, strerror(errno));
    else if (WIFEXITED(status))
        printf("family: %s exited with code %d\n", name, WEXITSTATUS(status));
    else
        printf("family: %s was stopped by signal %d\n", name, WTERMSIG(status));
}

int main(int argc, char **argv)
{
    (void)argc;
    /* argv[0] is /system/<arch>/test/family: keep /system/<arch>. */
    size_t n = strlen(argv[0]);
    for (int slashes = 0; n > 0 && slashes < 2;)
        if (argv[0][--n] == '/')
            slashes++;
    memcpy(system_dir, argv[0], n < sizeof(system_dir) - 1 ? n : sizeof(system_dir) - 1);
    printf("family: I am process %d\n", getpid());

    /* Two children at once: their lines alternate. */
    pid_t a = start("test", "counter", "A", "4", "40");
    pid_t b = start("test", "counter", "B", "4", "40");
    printf("family: started counters %s\n", a > 0 && b > a ? "A and B" : "?");
    pid_t first = wait(0), second = wait(0);
    printf("family: both counters ended (%s)\n",
           (first == a && second == b) || (first == b && second == a) ? "ok" : "?");

    /* A child that computes forever does not stop the others. */
    pid_t loop = start("test", "loop", 0, 0, 0);
    report(start("test", "counter", "C", "2", "10"), "counter C");
    printf("family: kill loop: %d\n", kill(loop, SIGKILL));
    report(loop, "loop");

    /* How children end. */
    report(start("bin", "false", 0, 0, 0), "false");
    report(start("test", "kwrite", 0, 0, 0), "kwrite");
    report(start("test", "privileged", 0, 0, 0), "privileged");

    /* Errors. */
    char *none[] = { "nope", 0 };
    printf("family: spawn /nope: %d, %s\n", spawn("/nope", none), strerror(errno));
    printf("family: spawn /readme.txt: %d, %s\n", spawn("/readme.txt", none), strerror(errno));
    printf("family: wait without children: %d, %s\n", wait(0), strerror(errno));
    printf("family: kill 9999: %d, %s\n", kill(9999, SIGKILL), strerror(errno));
    return 0;
}
