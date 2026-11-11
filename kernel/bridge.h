/*
 * Bridges (38b): br0, br1, ... A bridge joins cards (its ports: wired
 * cards and taps) into one Ethernet network, as a switch does: a frame
 * that comes in on one port goes out the port where its destination was
 * last heard from, or out every other port while that is not known yet
 * (and always for broadcast and multicast frames). The bridge is a card
 * of the stack itself: the machine's own address, DHCP and routes go on
 * brN, whose MAC address is its first port's; the ports carry no traffic
 * of their own (the stack sees no link on them) and take every frame
 * (promiscuous mode). Learned addresses are forgotten after 300 seconds
 * unheard, as 802.1D's default. There is no spanning tree (Linux's
 * default too): two bridges joined twice pass frames round for ever.
 *
 * Frames passed between ports are not the firewall's (they are layer 2);
 * what reaches brN itself is checked as any card's.
 */
#ifndef RITTEROS_BRIDGE_H
#define RITTEROS_BRIDGE_H

#include <stddef.h>
#include <stdint.h>

#define BRIDGE_MAX      8               /* br0..br7 */
#define BRIDGE_PORTS    16              /* ports of one bridge */

struct bridge;
struct netdev;

/* Card d joins bridge number (or a new bridge: number -1 the lowest free,
 * (38c) or new_number brN itself, else -1): the bridge's number, or
 * -ERR_NOENT (no such bridge), -ERR_EXIST (brN is there already), -ERR_BUSY
 * (d is a port already),
 * -ERR_INVAL (d is a bridge or the loopback), -ERR_NOTSUP (d cannot take
 * other machines' frames: a Wi-Fi card, a driver without set_promiscuous),
 * -ERR_NOSPC (every bridge or port taken), -ERR_NOMEM. */
int bridge_add(int number, int new_number, struct netdev *d);

/* Card d leaves bridge number (d 0: the whole bridge goes, every port
 * leaving): 0, -ERR_NOENT (no such bridge, or d not its port). */
int bridge_remove(int number, struct netdev *d);

/* A frame card `in` (a port of b) received: passed on, and given to the
 * stack as brN's when it is for the machine. From net_receive. */
void bridge_input(struct bridge *b, struct netdev *in, const uint8_t *frame, size_t length);

/* The bridge of a port, its name ("br0"), for net's report. */
const char *bridge_name(const struct bridge *b);

#endif
