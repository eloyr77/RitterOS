/*
 * Test: getaddrinfo and the other netdb.h calls (53d). Run where the
 * tests' DNS server knows NAME and a web server listens on PORT (the fetch
 * test): names, numbers, services from /system/config/services, the
 * errors, getnameinfo, gethostbyname, getservbyname/-port, and a
 * connection made from getaddrinfo's answer asking the web server for a
 * page. Lines "gaitest: ...".
 * (64d) IPV6NAME has only an IPv6 address; AF_INET6, AI_V4MAPPED,
 * numbers, and the order of both families (IPv6 first on a network with
 * IPv6).
 * Usage: gaitest NAME PORT [IPV6NAME]
 */
#include <arpa/inet.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static void show(const char *what, const char *node, const char *service, int family, int socktype,
                 int flags)
{
    struct addrinfo hints, *list = 0;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = family;
    hints.ai_socktype = socktype;
    hints.ai_flags = flags;
    int r = getaddrinfo(node, service, &hints, &list);
    if (r) {
        printf("gaitest: %s: %s\n", what, gai_strerror(r));
        return;
    }
    for (struct addrinfo *a = list; a; a = a->ai_next) {
        char text[INET6_ADDRSTRLEN + 2];
        unsigned port;
        if (a->ai_family == AF_INET6) {             /* (64d) "[ADDRESS]:PORT" */
            struct sockaddr_in6 *in = (struct sockaddr_in6 *)a->ai_addr;
            text[0] = '[';
            inet_ntop(AF_INET6, &in->sin6_addr, text + 1, sizeof(text) - 2);
            strcat(text, "]");
            port = ntohs(in->sin6_port);
        } else {
            struct sockaddr_in *in = (struct sockaddr_in *)a->ai_addr;
            inet_ntop(AF_INET, &in->sin_addr, text, sizeof(text));
            port = ntohs(in->sin_port);
        }
        printf("gaitest: %s: %s:%u %s%s%s\n", what, text, port,
               a->ai_socktype == SOCK_STREAM ? "tcp" : "udp", a->ai_canonname ? ", named " : "",
               a->ai_canonname ? a->ai_canonname : "");
    }
    freeaddrinfo(list);
}

int main(int argc, char **argv)
{
    if (argc != 3 && argc != 4) {
        printf("usage: gaitest NAME PORT [IPV6NAME]\n");
        return 2;
    }
    const char *name = argv[1], *name6 = argc == 4 ? argv[3] : "nowhere6.test";
    show("a name and a service", name, "http", AF_INET, SOCK_STREAM, AI_CANONNAME);
    show("for bind", 0, "8080", AF_UNSPEC, SOCK_STREAM, AI_PASSIVE);
    show("no node, both types", 0, "domain", 0, 0, 0);
    show("numbers only", "127.0.0.1", "22", AF_INET, SOCK_DGRAM, AI_NUMERICHOST | AI_NUMERICSERV);
    show("a name with AI_NUMERICHOST", name, 0, 0, SOCK_STREAM, AI_NUMERICHOST);
    show("an unknown service", name, "nosuchservice", 0, SOCK_STREAM, 0);
    show("a service name with AI_NUMERICSERV", name, "http", 0, SOCK_STREAM, AI_NUMERICSERV);
    show("IPv6", name, "http", AF_INET6, SOCK_STREAM, 0);
    show("IPv6, mapped", name, "http", AF_INET6, SOCK_STREAM, AI_V4MAPPED);
    show("an IPv6 name", name6, "http", 0, SOCK_STREAM, 0);
    show("an IPv6 number", "::1", "22", AF_UNSPEC, SOCK_DGRAM, AI_NUMERICHOST);
    show("an IPv6 number for IPv4", "::1", "22", AF_INET, SOCK_DGRAM, 0);
    show("a name nobody knows", "nowhere.test", "http", 0, SOCK_STREAM, 0);
    show("neither", 0, 0, 0, 0, 0);

    struct sockaddr_in in;
    memset(&in, 0, sizeof(in));
    in.sin_family = AF_INET;
    in.sin_port = htons(443);
    inet_pton(AF_INET, "10.0.2.2", &in.sin_addr);
    char host[NI_MAXHOST], serv[NI_MAXSERV];
    int r = getnameinfo((struct sockaddr *)&in, sizeof(in), host, sizeof(host), serv, sizeof(serv), 0);
    printf("gaitest: getnameinfo: %d %s %s\n", r, host, serv);
    r = getnameinfo((struct sockaddr *)&in, sizeof(in), host, sizeof(host), serv, sizeof(serv),
                    NI_NUMERICSERV);
    printf("gaitest: getnameinfo NI_NUMERICSERV: %d %s %s\n", r, host, serv);
    r = getnameinfo((struct sockaddr *)&in, sizeof(in), host, sizeof(host), 0, 0, NI_NAMEREQD);
    printf("gaitest: getnameinfo NI_NAMEREQD: %s\n", gai_strerror(r));

    struct hostent *h = gethostbyname(name);
    char text[INET_ADDRSTRLEN];
    printf("gaitest: gethostbyname: %s %s\n", h ? h->h_name : "-",
           h ? inet_ntop(AF_INET, h->h_addr, text, sizeof(text)) : "-");
    h = gethostbyname("nowhere.test");
    printf("gaitest: gethostbyname of nobody's name: %s, h_errno %s\n", h ? "found" : "none",
           h_errno == HOST_NOT_FOUND ? "HOST_NOT_FOUND" : "another");
    struct servent *s = getservbyname("ssh", "tcp");
    printf("gaitest: getservbyname ssh: %d %s\n", s ? ntohs((unsigned short)s->s_port) : -1, s ? s->s_proto : "-");
    s = getservbyport(htons(53), "udp");
    printf("gaitest: getservbyport 53: %s\n", s ? s->s_name : "-");

    struct addrinfo hints, *list;
    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    if ((r = getaddrinfo(name, argv[2], &hints, &list))) {
        printf("gaitest: the page: %s\n", gai_strerror(r));
        return 1;
    }
    int fd = socket(list->ai_family, list->ai_socktype, list->ai_protocol);
    r = connect(fd, list->ai_addr, list->ai_addrlen);
    freeaddrinfo(list);
    char request[128];
    snprintf(request, sizeof(request), "GET /hello.txt HTTP/1.0\r\nHost: %s\r\n\r\n", name);
    send(fd, request, strlen(request), 0);
    char reply[512];
    size_t got = 0;
    ssize_t n;
    while (got < sizeof(reply) - 1 && (n = recv(fd, reply + got, sizeof(reply) - 1 - got, 0)) > 0)
        got += (size_t)n;
    reply[got] = 0;
    close(fd);
    char *eol = strchr(reply, '\r');
    if (eol)
        *eol = 0;
    printf("gaitest: connect %d, the page's first line: %s\n", r, reply);
    printf("gaitest: done\n");
    return 0;
}
