/*
 * Disk write test ("disktest" on the command line), using the last 129
 * sectors of a partition: a 64 KiB pattern is written, flushed and read back,
 * and the very last sector holds a boot record (counter, random seed, time)
 * that the next boot reports, proving that writes persist.
 */
#ifndef RITTEROS_DISKTEST_H
#define RITTEROS_DISKTEST_H

#include <stdint.h>
#include "blockdev.h"

void disk_test(struct block_device *part, uint64_t seed);

#endif
