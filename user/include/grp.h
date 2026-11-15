/*
 * grp.h (43a): groups. RitterOS has no groups file: each user's group
 * (its gid in /system/config/users) is a group of that one user, named as
 * the user whose uid it is (gid 0 is "root"); getgrouplist and
 * getgroups give a user's one group. setgroups and initgroups change
 * nothing (0 for root, else EPERM).
 */
#ifndef RITTEROS_GRP_H
#define RITTEROS_GRP_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <sys/types.h>

struct group {
    char *gr_name;
    char *gr_passwd;
    gid_t gr_gid;
    char **gr_mem;
};

struct group *getgrnam(const char *name);
struct group *getgrgid(gid_t gid);
int getgrnam_r(const char *name, struct group *grp, char *buf, size_t size, struct group **result);
int getgrgid_r(gid_t gid, struct group *grp, char *buf, size_t size, struct group **result);
struct group *getgrent(void);
void setgrent(void);
void endgrent(void);
int getgrouplist(const char *user, gid_t group, gid_t *groups, int *count);
int setgroups(size_t count, const gid_t *groups);
int initgroups(const char *user, gid_t group);

#ifdef __cplusplus
}
#endif

#endif
