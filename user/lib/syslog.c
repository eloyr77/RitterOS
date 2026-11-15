/*
 * (43a-3) syslog: a line in the log of openlog's ident (LOG_NAME_SIZE
 * bytes of it, the program's name without one) through SYS_LOG, which
 * only root may write; "%m" is errno's words. LOG_PERROR copies the line
 * to the standard error, LOG_CONS writes it there when the log refuses.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>
#include "ritteros.h"

static char ident[LOG_NAME_SIZE];
static int options;
static int mask = LOG_UPTO(LOG_DEBUG);

void openlog(const char *name, int opts, int facility)
{
    (void)facility;
    snprintf(ident, sizeof(ident), "%s", name ? name : "");
    options = opts;
}

void closelog(void)
{
    ident[0] = 0;
    options = 0;
}

int setlogmask(int new_mask)
{
    int old = mask;
    if (new_mask)
        mask = new_mask;
    return old;
}

void vsyslog(int priority, const char *format, va_list ap)
{
    if (!(mask & LOG_MASK(LOG_PRI(priority))))
        return;
    int saved = errno;
    char expanded[1024], line[1100];
    size_t n = 0;
    for (const char *f = format; *f && n + 1 < sizeof(expanded); f++) {
        if (f[0] == '%' && f[1] == 'm') {       /* (%m: errno's words, %%m stays) */
            n += (size_t)snprintf(expanded + n, sizeof(expanded) - n, "%s", strerror(saved));
            f++;
        } else if (f[0] == '%' && f[1] == '%') {
            expanded[n++] = *f++;
            if (n + 1 < sizeof(expanded))
                expanded[n++] = *f;
        } else {
            expanded[n++] = *f;
        }
        if (n >= sizeof(expanded))
            n = sizeof(expanded) - 1;
    }
    expanded[n] = 0;
    errno = saved;
    vsnprintf(line, sizeof(line), expanded, ap);
    const char *name = ident[0] ? ident : getprogname();
    char log_name[LOG_NAME_SIZE];
    snprintf(log_name, sizeof(log_name), "%s", name && *name ? name : "syslog");
    int r = ritteros_log_write(log_name, line);
    if ((options & LOG_PERROR) || (r < 0 && (options & LOG_CONS))) {
        if (options & LOG_PID)
            dprintf(STDERR_FILENO, "%s[%d]: %s\n", log_name, (int)getpid(), line);
        else
            dprintf(STDERR_FILENO, "%s: %s\n", log_name, line);
    }
    errno = saved;
}

void syslog(int priority, const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    vsyslog(priority, format, ap);
    va_end(ap);
}
