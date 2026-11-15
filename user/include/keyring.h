/*
 * The keyring (70b, item 48; libkeyring.so.1): secrets a program keeps for
 * its user (passwords, tokens), each under a service and an account,
 * encrypted in the user's ~/.config/keyring.db. The elf keyringd holds it
 * open from the user's login (opened with the login password) until the
 * last login ends; these calls ask it, so a program never sees the
 * keyring's password. A service and an account are 1 to KEYRING_NAME_MAX
 * bytes without control characters; a secret is any KEYRING_SECRET_MAX
 * bytes at most. (70c) A program may use the entries it stored; another
 * entry only once the user allowed it (keyringd asks in a window, or
 * `keyring allow`): the call waits for that answer. On failure -1 and
 * errno: ENOENT no such entry, EACCES the keyring is locked (`keyring
 * unlock` opens it), EPERM not allowed to this program, ECONNREFUSED
 * keyringd does not run, EADDRINUSE another user holds keyringd's name,
 * EINVAL a name or size refused, and the reason a change could not be saved
 * (ENOSPC...).
 */
#ifndef RITTEROS_KEYRING_H
#define RITTEROS_KEYRING_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <sys/types.h>

#define KEYRING_NAME_MAX     255
#define KEYRING_SECRET_MAX   4096

/* The secret of (service, account) into secret (at most size bytes, not
 * NUL-terminated): its whole length, which may be more than size. */
ssize_t keyring_get(const char *service, const char *account, void *secret, size_t size);
/* Keeps secret (length bytes) under (service, account), a new entry or
 * the entry's new secret: 0. */
int keyring_set(const char *service, const char *account, const void *secret, size_t length);
/* Takes (service, account) out: 0. */
int keyring_remove(const char *service, const char *account);
/* Calls each(service, account, arg) for every entry (never a secret):
 * the number of entries. */
int keyring_list(void (*each)(const char *service, const char *account, void *arg), void *arg);
/* (70d) From now on this program's calls use the system keyring (root's
 * programs', /system/config/keyring.db, kept by the elf `keyringd -s`)
 * instead of the user's own. Only programs running as root get answers
 * there (EACCES otherwise); every root program may use every entry. */
void keyring_system(void);

#ifdef __cplusplus
}
#endif

#endif
