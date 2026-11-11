/*
 * Virtual network cards (38a): tap0, tap1, ... A tap is a card of the
 * stack like any other (addresses, routes, the firewall, net's report),
 * whose wire is a program: `net tap add [user NAME]` makes one, and a
 * program that opens /dev/tapN reads every frame the stack sends through
 * the card (one whole Ethernet frame per read, without its checksum) and
 * writes the frames the card receives (one per write, 14 to 1514 bytes).
 * Linux's TAP device and BSD's /dev/tapN work the same way.
 *
 * Only root and the card's user may open it, one program at a time; the
 * link is up while it is open, down when it closes (what was not read is
 * dropped). A frame written for another card's MAC address is dropped, as
 * a card's own filter would (broadcast and multicast frames go up).
 */
#ifndef RITTEROS_TAP_H
#define RITTEROS_TAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TAP_MAX         64              /* tap0..tap63 */

struct tap;

/* A new tap for user uid, tapN with N `want` (38c: the settings file's
 * number) or the lowest free one (want -1): its number, or -ERR_NOSPC when
 * every number is taken (or the card list is full), -ERR_EXIST (tapN is
 * there), -ERR_NOMEM. Root's call. */
int tap_add(uint32_t uid, int want);

/* Remove tapN: 0, -ERR_NOENT, or -ERR_BUSY while a program has it open. */
int tap_remove(int number);

/* Open /dev/tapN for a process of user uid: 0 with *out set, -ERR_NOENT
 * (no such tap), -ERR_ACCES (another user's), -ERR_BUSY (open already),
 * -ERR_NOMEM. */
int tap_open(int number, uint32_t uid, struct tap **out);

/* The program let go of it (its last descriptor closed). */
void tap_close(struct tap *t);

/* The next frame the stack sent into buf (a kernel buffer of size bytes; a
 * longer frame is cut to size): waits for one unless the calling thread
 * does not wait (-ERR_AGAIN); -ERR_INTR on a signal. */
int64_t tap_read(struct tap *t, uint8_t *buf, size_t size);

/* One frame from the program, received by the card: its length, or
 * -ERR_INVAL for less than a header or more than ETH_FRAME_MAX bytes. */
int64_t tap_write(struct tap *t, const uint8_t *frame, size_t length);

/* POLLIN when a frame waits; POLLOUT always (a write never waits). */
int tap_poll(struct tap *t);

/* Whether tapN is there now (for /dev). */
bool tap_present(int number);

#endif
