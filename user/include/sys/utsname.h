/* The system's names (68i, POSIX): uname fills a struct utsname, whose
 * layout is the kernel's struct sys_utsname. */
#ifndef RITTEROS_SYS_UTSNAME_H
#define RITTEROS_SYS_UTSNAME_H

#define _UTSNAME_LENGTH 65

struct utsname {
    char sysname[_UTSNAME_LENGTH];      /* "RitterOS" */
    char nodename[_UTSNAME_LENGTH];     /* "ritteros" (no host names yet) */
    char release[_UTSNAME_LENGTH];      /* the release, year.month: "2026.11" */
    char version[_UTSNAME_LENGTH];      /* the commit it was built from ("513e2a4", + when
                                           changed), or "unknown" */
    char machine[_UTSNAME_LENGTH];      /* "x86_64" or "aarch64" */
};

/* Fills *name: 0, or -1 with errno EFAULT. */
int uname(struct utsname *name);

#endif
