/*
 * Test: tlsclient [-c CAFILE] [-s NAME] [-k] [-n] [-t] HOST PORT PATH asks a
 * TLS server for PATH (HTTP/1.0, as openssl s_server -WWW answers) through
 * libtls (step 23d) and prints the protocol, the cipher suite, the answer's
 * first line and the size and SHA-256 of its body; or libtls's error. -s
 * names the server (default HOST), -k/-n/-t switch off the chain, name and
 * date checks, -c trusts CAFILE instead of the system's roots.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ritteros.h"
#include "tls.h"

static char answer[1 << 20];

int main(int argc, char **argv)
{
    struct tls_config *config = tls_config_new();
    const char *name = 0, *ca = 0;
    int i = 1;
    for (; i < argc && argv[i][0] == '-'; i++) {
        if (!strcmp(argv[i], "-k"))
            tls_config_insecure_noverifycert(config);
        else if (!strcmp(argv[i], "-n"))
            tls_config_insecure_noverifyname(config);
        else if (!strcmp(argv[i], "-t"))
            tls_config_insecure_noverifytime(config);
        else if (!strcmp(argv[i], "-c") && i + 1 < argc)
            ca = argv[++i];
        else if (!strcmp(argv[i], "-s") && i + 1 < argc)
            name = argv[++i];
        else
            return 2;
    }
    if (argc - i != 3)
        return 2;
    if (ca && tls_config_set_ca_file(config, ca) < 0) {
        printf("tlsclient: %s\n", tls_config_error(config));
        return 1;
    }
    struct tls *t = tls_client();
    tls_configure(t, config);
    if (tls_connect_servername(t, argv[i], argv[i + 1], name ? name : argv[i]) < 0) {
        printf("tlsclient: %s\n", tls_error(t));
        return 1;
    }
    char request[300];
    int n = snprintf(request, sizeof(request), "GET %s HTTP/1.0\r\n\r\n", argv[i + 2]);
    if (tls_write(t, request, (size_t)n) != n) {
        printf("tlsclient: %s\n", tls_error(t));
        return 1;
    }
    size_t have = 0;
    ssize_t k;
    while (have < sizeof(answer) && (k = tls_read(t, answer + have, sizeof(answer) - have)) > 0)
        have += (size_t)k;
    if (k < 0) {
        printf("tlsclient: %s\n", tls_error(t));
        return 1;
    }
    printf("tlsclient: %s %s, server %s\n", tls_conn_version(t), tls_conn_cipher(t),
           tls_conn_servername(t));
    char *end = 0;
    for (size_t j = 0; !end && j + 4 <= have; j++)
        if (!memcmp(answer + j, "\r\n\r\n", 4))
            end = answer + j;
    char *eol = memchr(answer, '\r', have);
    printf("tlsclient: %.*s\n", eol ? (int)(eol - answer) : 0, answer);
    if (end) {
        end += 4;
        size_t body = have - (size_t)(end - answer);
        struct sha256 s;
        uint8_t hash[SHA256_SIZE];
        sha256_init(&s);
        sha256_update(&s, end, body);
        sha256_final(&s, hash);
        printf("tlsclient: body %lu bytes, sha256 ", (unsigned long)body);
        for (int b = 0; b < SHA256_SIZE; b++)
            printf("%02x", hash[b]);
        printf("\n");
    }
    if (tls_close(t) < 0)
        printf("tlsclient: close: %s\n", tls_error(t));
    tls_free(t);
    tls_config_free(config);
    return 0;
}
