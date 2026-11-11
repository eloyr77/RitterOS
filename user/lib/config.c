/*
 * The system's settings (/system/config): one "key = value" per line;
 * spaces around the key and the value do not count, a line starting with
 * "#" is a comment. The first line with the key counts.
 */
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include "ritteros.h"

#define CONFIG_MAX 16384

static char text[CONFIG_MAX + 1];

static bool space(char c)
{
    return c == ' ' || c == '\t' || c == '\r';
}

int config_get(const char *path, const char *key, char *value, size_t size)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    size_t n = 0;
    ssize_t r;
    while (n < CONFIG_MAX && (r = read(fd, text + n, CONFIG_MAX - n)) > 0)
        n += (size_t)r;
    close(fd);
    text[n] = 0;
    size_t key_length = strlen(key);
    for (char *line = text; *line;) {
        char *end = strchr(line, '\n');
        if (!end)
            end = line + strlen(line);
        char *p = line;
        while (p < end && space(*p))
            p++;
        if (p < end && *p != '#' && (size_t)(end - p) > key_length &&
            strncmp(p, key, key_length) == 0) {
            char *q = p + key_length;
            while (q < end && space(*q))
                q++;
            if (q < end && *q == '=') {
                q++;
                while (q < end && space(*q))
                    q++;
                char *last = end;
                while (last > q && space(last[-1]))
                    last--;
                size_t length = (size_t)(last - q);
                if (size == 0 || length >= size) {
                    errno = ENAMETOOLONG;
                    return -1;
                }
                memcpy(value, q, length);
                value[length] = 0;
                return 1;
            }
        }
        line = *end ? end + 1 : end;
    }
    return 0;
}
