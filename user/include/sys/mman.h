/*
 * Memory mappings (step 22a): mmap and munmap as on Unix, a subset.
 * Anonymous memory (MAP_ANONYMOUS: zeroed, the program's own) and files: a
 * read-only mapping of a RitterFS file shares one copy of its pages with
 * every program that maps the same version of the file (a file rewritten
 * later is a new version; old mappings keep the old pages), a writable
 * MAP_PRIVATE mapping is the program's own copy -- since 22e made a page at
 * a time, at the page's first write; until then it shares the file's pages.
 * Since 22j a MAP_SHARED mapping of a RitterFS file open for writing may be
 * writable: every program mapping that file shares the same pages and sees
 * the others' writes at once; msync (and munmap, and the program's end)
 * writes them to the file. Other file systems: ENOTSUP; a descriptor open
 * for reading only: EACCES. read() sees the writes once they are written
 * back. Anonymous memory is made when first touched, so a large mapping
 * costs nothing until used; a file's pages are read when it is mapped. A
 * mapping may reach past its file's end; touching a page there ends the
 * program with SIGBUS. Never writable and executable at once (ENOTSUP).
 */
#ifndef RITTEROS_SYS_MMAN_H
#define RITTEROS_SYS_MMAN_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <sys/types.h>
#include "../../../common/syscalls.h"

#define PROT_NONE       0               /* a reservation: nothing mapped, every access faults */
#define PROT_READ       MMAP_READ
#define PROT_WRITE      MMAP_WRITE      /* (reading too) */
#define PROT_EXEC       MMAP_EXEC

#define MAP_SHARED      MMAP_SHARED
#define MAP_PRIVATE     MMAP_PRIVATE
#define MAP_FIXED       MMAP_FIXED      /* exactly at addr (a page boundary in the mmap window),
                                         * replacing the mappings there */
#define MAP_ANONYMOUS   MMAP_ANONYMOUS  /* zeroed memory: fd and offset are not used */
#define MAP_ANON        MAP_ANONYMOUS
#define MAP_FILE        0               /* (71a) a file's mapping: the default; Linux's, the
                                         * BSDs' and the Mac's are 0 too (WebKit uses it) */
#define MAP_NORESERVE   0x4000          /* (45c) taken, nothing more: no memory is reserved
                                         * ahead anyway (a page costs memory from its first
                                         * touch), so large PROT_NONE reservations are free */

#define MAP_FAILED      ((void *)-1)

#define MS_ASYNC        MSYNC_ASYNC     /* msync flags (as on Unix; all write back at once) */
#define MS_INVALIDATE   MSYNC_INVALIDATE
#define MS_SYNC         MSYNC_SYNC

/* Map length bytes (rounded up to whole 4096-byte pages) where the system
 * chooses, or exactly at addr with MAP_FIXED; a file from offset (a
 * multiple of 4096). The address, or MAP_FAILED with errno set. */
void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset);

/* Remove the mappings in [addr, addr + length), or parts of them (addr on a
 * page boundary); parts of the range with nothing mapped are fine. 0, or -1
 * with errno set. */
int munmap(void *addr, size_t length);

/* Give the pages in [addr, addr + length) (addr on a page boundary, all of
 * it mapped: else ENOMEM) the access prot; they keep their contents, also
 * through PROT_NONE. 0, or -1 with errno set: ENOTSUP for writable and
 * executable at once, for writing a shared file mapping, and for giving
 * access to a file mapping made PROT_NONE (step 22f). */
int mprotect(void *addr, size_t length, int prot);

/* Write what was written through the writable shared mappings in
 * [addr, addr + length) (addr on a page boundary) to their files (22j). 0, or
 * -1 with errno set (EIO if a file could not be written). */
int msync(void *addr, size_t length, int flags);

/* (43b-3) Memory files: shared memory with a size, read and written as a
 * file (read, write, lseek, ftruncate, fstat) and mapped MAP_SHARED by
 * every program it is given to (sendmsg's SCM_RIGHTS, fork); MAP_PRIVATE
 * only to read (ENOTSUP for writing). A page past its size is SIGBUS.
 * memfd_create makes one without a name (MFD_CLOEXEC; MFD_ALLOW_SEALING:
 * else it comes with F_SEAL_SEAL, as Linux's); shm_open opens one by name
 * in /dev/shm (O_RDONLY or O_RDWR, O_CREAT, O_EXCL, O_TRUNC; the name is
 * "/NAME" or "NAME", no other slash), shm_unlink removes the name (its
 * owner or root): it lives while descriptors or mappings hold it. */
#define MFD_CLOEXEC         0x0001U
#define MFD_ALLOW_SEALING   0x0002U
int memfd_create(const char *name, unsigned int flags);
int shm_open(const char *name, int flags, mode_t mode);
int shm_unlink(const char *name);

/* (43b-3) madvise: MADV_DONTNEED and MADV_FREE give the pages back
 * (anonymous memory reads 0 at the next touch, a private file mapping the
 * file again; shared memory keeps its content), the others change nothing.
 * posix_madvise returns the error instead (POSIX_MADV_DONTNEED changes
 * nothing, as glibc's). */
#define MADV_NORMAL         0
#define MADV_RANDOM         1
#define MADV_SEQUENTIAL     2
#define MADV_WILLNEED       MADVISE_WILLNEED
#define MADV_DONTNEED       MADVISE_DONTNEED
#define MADV_FREE           MADVISE_FREE
#define POSIX_MADV_NORMAL       0
#define POSIX_MADV_RANDOM       1
#define POSIX_MADV_SEQUENTIAL   2
#define POSIX_MADV_WILLNEED     3
#define POSIX_MADV_DONTNEED     4
int madvise(void *addr, size_t length, int advice);
int posix_madvise(void *addr, size_t length, int advice);

/* (46b) Memory locking: not there. RitterOS cannot keep pages out of swap
 * (which is encrypted, with a key made at each `swap on`), so these fail
 * with ENOSYS rather than promise it; OpenSSL's secure heap goes on
 * without it. */
#define MCL_CURRENT     1
#define MCL_FUTURE      2
int mlock(const void *addr, size_t length);
int munlock(const void *addr, size_t length);
int mlockall(int flags);
int munlockall(void);

#ifdef __cplusplus
}
#endif

#endif
