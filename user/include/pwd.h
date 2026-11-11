/* Users: /system/config/users holds one line per user, name:uid:gid:home
 * (readable by everyone; passwords are elsewhere, see ritteros.h). */
#ifndef RITTEROS_PWD_H
#define RITTEROS_PWD_H

#include <sys/types.h>

#define USERS_FILE "/system/config/users"

struct passwd {
    char *pw_name;
    uid_t pw_uid;
    gid_t pw_gid;
    char *pw_dir;               /* the home directory */
};

/* The user with that name or ID, or 0 (errno set only on a read error).
 * The result lives until the next call. */
struct passwd *getpwnam(const char *name);
struct passwd *getpwuid(uid_t uid);

#endif
