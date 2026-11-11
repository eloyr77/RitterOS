#include "cmdline.h"

bool cmdline_get(const char *cmdline, const char *key, char *value, size_t size)
{
    const char *p = cmdline;
    while (*p) {
        while (*p == ' ')
            p++;
        if (!*p)
            break;

        /* Compare the word's key part with the wanted key. */
        const char *k = key;
        const char *w = p;
        while (*k && *w == *k) {
            k++;
            w++;
        }
        bool match = *k == 0 && (*w == 0 || *w == ' ' || *w == '=');

        if (match) {
            size_t n = 0;
            if (*w == '=') {
                w++;
                while (*w && *w != ' ' && n + 1 < size)
                    value[n++] = *w++;
            }
            if (size > 0)
                value[n] = 0;
            return true;
        }
        while (*p && *p != ' ')
            p++;
    }
    return false;
}
