/* The release the kernel was built as (68i): the build writes VERSION's
 * year.month and the commit into build/version.h, read only here. */
#include "release.h"
#include "arch.h"
#include "string.h"
#include "../build/version.h"

const char release_version[] = RITTEROS_VERSION;
const char release_commit[] = RITTEROS_COMMIT;

static void put(char *field, const char *text)
{
    size_t n = strlen(text);
    if (n >= SYS_UTSNAME_LENGTH)
        n = SYS_UTSNAME_LENGTH - 1;
    memcpy(field, text, n);
    field[n] = 0;
}

void release_uname(struct sys_utsname *u)
{
    memset(u, 0, sizeof(*u));
    put(u->sysname, "RitterOS");
    put(u->nodename, "ritteros");
    put(u->release, release_version);
    put(u->version, release_commit[0] ? release_commit : "unknown");  /* (one word for scripts) */
    put(u->machine, ARCH_NAME);
}
