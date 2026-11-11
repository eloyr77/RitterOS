/* The release the kernel was built as (68i): VERSION's year.month and the
 * commit, from the build's version.h. */
#ifndef RELEASE_H
#define RELEASE_H

#include "syscalls.h"

extern const char release_version[];   /* "2026.11" */
extern const char release_commit[];    /* "513e2a4", "513e2a4+" or "" */

/* The system's names, as SYS_UNAME answers. */
void release_uname(struct sys_utsname *u);

#endif
