/*
 * CRC-32 checksums.
 *   crc32_ieee: IEEE 802.3 polynomial, as used by GPT, zlib and Ethernet
 *           (46a: not `crc32`, zlib's name for its own, with other
 *           arguments: programs get this one as ritteros_crc32).
 *   crc32c: Castagnoli polynomial, as used by RitterFS, btrfs, ext4 and iSCSI.
 */
#ifndef RITTEROS_CRC32_H
#define RITTEROS_CRC32_H

#include <stddef.h>
#include <stdint.h>

uint32_t crc32_ieee(const void *data, size_t size);
uint32_t crc32c(const void *data, size_t size);

#endif
