/*
 * Reading test for RitterFS, run at every boot: list the directory tree and
 * verify every file named in /test/manifest.txt (size and CRC-32C, written by
 * scripts/mkdisk.py) by reading it through the kernel's file system code.
 */
#ifndef RITTEROS_FSTEST_H
#define RITTEROS_FSTEST_H

#include "ritterfs.h"

void fs_test(struct ritterfs *fs);

/* "fswrite": three commits creating, changing and removing files under /work,
 * each followed by a full check; finally everything is read back. */
void fs_write_test(struct ritterfs *fs);

/* "fstorture": up to 5000 commits of random creates, writes and removes under
 * /t, logging each commit; scripts/test.sh kills the VM at random moments. */
void fs_torture(struct ritterfs *fs, uint64_t seed);

/* Snapshot test ("fssnap"): a snapshot keeps a file's old content while the
 * live one changes; deleting one sweeps its blocks; one snapshot stays for
 * the host to check. */
void fs_snapshot_test(struct ritterfs *fs);

/* Instant copy test ("fscopy"): a directory copied without its data, the
 * copy changed apart from the original, the original removed and swept;
 * /copy/dst and a copy of it, /copy/again, stay for the host to check. */
void fs_copy_test(struct ritterfs *fs);

/* Attribute test ("fsattr"): set, read back, replace, remove, refuse bad
 * keys and values, a table larger than a block, attributes dropped with
 * their file and kept by copies; /attr stays for the host to check. */
void fs_attr_test(struct ritterfs *fs);

/* Transaction test ("fstxn"): uncommitted changes of every kind thrown
 * away with ritterfs_abort, then a commit as usual. */
void fs_abort_test(struct ritterfs *fs);

#endif
