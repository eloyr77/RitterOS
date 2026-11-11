/*
 * Random bytes (kernel/random.c): a ChaCha20 generator seeded from the
 * firmware's random seed and the CPU's counter.
 */
#ifndef RITTEROS_RANDOM_H
#define RITTEROS_RANDOM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void random_init(const uint8_t *seed, size_t size);
bool random_selftest(void);         /* the RFC 8439 test vector */
void random_bytes(void *buf, size_t n);

#endif
