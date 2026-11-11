/*
 * id: the user and group this program runs as, with the user's name if
 * /system/config/users knows it (0: root); in a setuid program (50b) also
 * the effective user, when it differs: uid=1000(anna) euid=0(root).
 */
#include <pwd.h>
#include <stdio.h>
#include <unistd.h>
#include "ritteros.h"

PROGRAM_HELP("id", "the user and group you are");

int main(void)
{
    uid_t uid = getuid(), euid = geteuid();
    gid_t gid = getgid();
    char who[48] = "", as[64] = "";
    struct passwd *pw = getpwuid(uid);
    if (pw)
        snprintf(who, sizeof(who), "(%s)", pw->pw_name);
    if (euid != uid) {
        pw = getpwuid(euid);
        snprintf(as, sizeof(as), " euid=%u%s%s%s", euid, pw ? "(" : "", pw ? pw->pw_name : "",
                 pw ? ")" : "");
    }
    printf("uid=%u%s%s gid=%u\n", uid, who, as, gid);
    return 0;
}
