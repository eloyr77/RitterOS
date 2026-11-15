/*
 * syslog.h (43a-3): messages for the system's logs. syslog adds a line
 * to the log named by openlog's ident (the program's name when none), as
 * /system/logs/IDENT.log once the elf logd writes it; only root may add to
 * the logs, so other programs' lines are dropped, unless openlog asked for
 * LOG_PERROR (also on the standard error) or LOG_CONS (there when the log
 * refuses). %m in the format is errno's words. setlogmask picks the
 * priorities kept (LOG_UPTO(LOG_DEBUG): all, the start).
 */
#ifndef RITTEROS_SYSLOG_H
#define RITTEROS_SYSLOG_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdarg.h>

#define LOG_EMERG       0
#define LOG_ALERT       1
#define LOG_CRIT        2
#define LOG_ERR         3
#define LOG_WARNING     4
#define LOG_NOTICE      5
#define LOG_INFO        6
#define LOG_DEBUG       7
#define LOG_PRIMASK     0x07
#define LOG_PRI(p)      ((p) & LOG_PRIMASK)
#define LOG_MAKEPRI(f, p) ((f) | (p))
#define LOG_KERN        (0 << 3)
#define LOG_USER        (1 << 3)
#define LOG_MAIL        (2 << 3)
#define LOG_DAEMON      (3 << 3)
#define LOG_AUTH        (4 << 3)
#define LOG_SYSLOG      (5 << 3)
#define LOG_LPR         (6 << 3)
#define LOG_NEWS        (7 << 3)
#define LOG_UUCP        (8 << 3)
#define LOG_CRON        (9 << 3)
#define LOG_AUTHPRIV    (10 << 3)
#define LOG_FTP         (11 << 3)
#define LOG_LOCAL0      (16 << 3)
#define LOG_LOCAL1      (17 << 3)
#define LOG_LOCAL2      (18 << 3)
#define LOG_LOCAL3      (19 << 3)
#define LOG_LOCAL4      (20 << 3)
#define LOG_LOCAL5      (21 << 3)
#define LOG_LOCAL6      (22 << 3)
#define LOG_LOCAL7      (23 << 3)
#define LOG_FACMASK     0x03f8
#define LOG_MASK(p)     (1 << (p))
#define LOG_UPTO(p)     ((1 << ((p) + 1)) - 1)
#define LOG_PID         0x01
#define LOG_CONS        0x02
#define LOG_ODELAY      0x04
#define LOG_NDELAY      0x08
#define LOG_NOWAIT      0x10
#define LOG_PERROR      0x20

void openlog(const char *ident, int options, int facility);
void syslog(int priority, const char *format, ...) __attribute__((format(printf, 2, 3)));
void vsyslog(int priority, const char *format, va_list ap);
void closelog(void);
int setlogmask(int mask);

#ifdef __cplusplus
}
#endif

#endif
