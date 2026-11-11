/*
 * Test: local (Unix-domain) stream sockets (step 15b). Two modes, each
 * printing what it saw so the test can compare:
 *   local serve PATH        listen on PATH, accept one connection, read a
 *                           message, print it, send it back with "echo: ",
 *                           then end
 *   local talk PATH MESSAGE connect to PATH (retrying while nobody listens
 *                           yet), send MESSAGE, print the reply
 *   local pair PATH MESSAGE both at once: a server thread and the client in
 *                           one program (what the test runs)
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <thread.h>
#include <unistd.h>
#include "ritteros.h"

static char buffer[1024];

static int fail(const char *what)
{
    printf("local: %s: %s\n", what, strerror(errno));
    return 1;
}

static int serve(const char *path)
{
    int fd = ritteros_local_listen(path, 4);
    if (fd < 0)
        return fail("listen");
    printf("local: listening on %s\n", path);
    int c = ritteros_accept(fd, 0, 0);
    if (c < 0)
        return fail("accept");
    ssize_t n = read(c, buffer, sizeof(buffer) - 1);
    if (n < 0)
        return fail("read");
    buffer[n] = 0;
    printf("server got: %s\n", buffer);
    char reply[1040];
    int len = snprintf(reply, sizeof(reply), "echo: %s", buffer);
    if (write(c, reply, (size_t)len) < 0)
        return fail("write");
    close(c);
    close(fd);
    return 0;
}

static int talk(const char *path, const char *message)
{
    int fd = -1;
    for (int i = 0; i < 40 && fd < 0; i++) {        /* wait up to ~2 s for the listener */
        fd = ritteros_local_connect(path);
        if (fd < 0 && errno == ECONNREFUSED)
            usleep(50000);
        else
            break;
    }
    if (fd < 0)
        return fail("connect");
    if (write(fd, message, strlen(message)) < 0)
        return fail("write");
    ssize_t n = read(fd, buffer, sizeof(buffer) - 1);
    if (n < 0)
        return fail("read");
    buffer[n] = 0;
    printf("client got: %s\n", buffer);
    close(fd);
    return 0;
}

static const char *pair_path;

static int server_thread(void *arg)
{
    (void)arg;
    return serve(pair_path);
}

int main(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[1], "serve") == 0)
        return serve(argv[2]);
    if (argc == 4 && strcmp(argv[1], "talk") == 0)
        return talk(argv[2], argv[3]);
    if (argc == 4 && strcmp(argv[1], "pair") == 0) {
        pair_path = argv[2];
        int t = thread_create(server_thread, 0);
        if (t < 0)
            return fail("thread");
        int client = talk(argv[2], argv[3]);
        int server = 0;
        thread_join(t, &server);
        return client || server;
    }
    printf("usage: local serve PATH | local talk PATH MESSAGE | local pair PATH MESSAGE\n");
    return 1;
}
