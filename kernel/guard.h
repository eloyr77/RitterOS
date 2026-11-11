/*
 * The firewall ("guard", roadmap item 12): the kernel's half. Every packet
 * that arrives for the machine on a card, or leaves it on one, is checked
 * against the loaded table of rules (see SYS_GUARD in common/syscalls.h);
 * lo is never checked, forwarded packets not yet. The program guardd
 * loads the table; until then what comes in is dropped.
 */
#ifndef RITTEROS_GUARD_H
#define RITTEROS_GUARD_H

#include <stdbool.h>
#include <stddef.h>
#include "net.h"
#include "syscalls.h"

/* May this IPv4 packet (header and all) pass: arriving on card d, or
 * leaving on it? Card threads and senders call them; no waiting. */
bool guard_check_in(struct netdev *d, const uint8_t *packet, size_t length);
bool guard_check_out(struct netdev *d, const uint8_t *packet, size_t length);
/* A packet passing between two cards (forwarding); in and out are the cards
 * it arrived on and leaves on. */
bool guard_check_forward(struct netdev *in, struct netdev *out, const uint8_t *packet,
                         size_t length);

/* Load a table (its rules and defaults; counters from 0): 0, or ERR_INVAL. */
int guard_load(const struct sys_guard *g);

/* No checks at all from now on. */
/* (64e) IPv6's own table (GUARD_SET6, GUARD_GET6). */
int guard_set6(const struct sys_guard6 *g);
void guard_get6(struct sys_guard6 *g);

void guard_off(void);

/* The table as loaded, with its counters and the last drops. */
void guard_status(struct sys_guard *g);

#endif
