/* Users: /system/config/users holds one line per user, name:uid:gid:home
 * (readable by everyone; passwords are elsewhere, see ritteros.h). */
#ifndef RITTEROS_PWD_H
#define RITTEROS_PWD_H

#ifdef __cplusplus
extern "C" {
#endif

#include <sys/types.h>

#define USERS_FILE "/system/config/users"

/* (43a) POSIX's whole struct passwd: pw_passwd is "x" (passwords are
 * elsewhere), pw_gecos the user's name, pw_shell /bin/sh. Programs built
 * before 43a had a shorter one (name, uid, gid, dir): the library keeps
 * their getpwnam and getpwuid, and these declarations name the new ones. */
struct passwd {
    char *pw_name;
    char *pw_passwd;
    uid_t pw_uid;
    gid_t pw_gid;
    char *pw_gecos;
    char *pw_dir;               /* the home directory */
    char *pw_shell;
};

/* The user with that name or ID, or 0 (errno set only on a read error).
 * The result lives until the next call. */
struct passwd *getpwnam(const char *name) __asm__("__ritteros_getpwnam2");
struct passwd *getpwuid(uid_t uid) __asm__("__ritteros_getpwuid2");

/* (43a) The same into the caller's pwd, its strings in buf (size bytes):
 * 0 with *result = pwd, or 0 with *result = 0 when there is none, or an
 * error number (ERANGE: buf too small). getpwent: the users one by one
 * (root first), 0 after the last; setpwent starts over, endpwent ends. */
int getpwnam_r(const char *name, struct passwd *pwd, char *buf, size_t size, struct passwd **result);
int getpwuid_r(uid_t uid, struct passwd *pwd, char *buf, size_t size, struct passwd **result);
struct passwd *getpwent(void);
void setpwent(void);
void endpwent(void);

#ifdef __cplusplus
}
#endif

#endif
