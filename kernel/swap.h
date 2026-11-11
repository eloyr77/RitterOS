/*
 * Swap (roadmap item 35, kernel/swap.c): programs' pages kept in a file
 * while memory is short. The file is /system/swap, a fixed file of the
 * system's RitterFS (RFS_FLAG_FIXED: its blocks stay where they are and are
 * written in place, never through the file system). It is split into slots
 * of a page each; a page is written encrypted (XTS-AES-256 with a key made
 * when swap is turned on, kept only in memory, the slot as the tweak), so
 * what is left in the file after a power-off cannot be read.
 *
 * Off until the user asks: `swap on` (SYS_SWAP) turns it on and writes
 * `on = yes` into /system/config/swap, which later boots read after the
 * root mount (swap_settings, after the boot's check). (35b) The thread
 * pageout writes programs' pages there while free memory is low (see
 * process_pageout and mmap_pageout); a fault on one reads it back.
 */
#ifndef RITTEROS_SWAP_H
#define RITTEROS_SWAP_H

#include <stdbool.h>
#include <stdint.h>

struct sys_swap;

#define SWAP_FILE     "/system/swap"
#define SWAP_SETTINGS "/system/config/swap"
#define SWAP_NONE     (~(uint64_t)0)    /* no slot */

/* After the root mount: swap on if /system/config/swap says `on = yes`.
 * selftest (the boot's option): every time swap is turned on, every slot is
 * written and read back (swap's self-test). */
void swap_settings(bool selftest);

/* Turn swap on with /system/swap (made fixed first if it is not yet) or
 * off (refused while slots are used: -ERR_BUSY): 0 or -ERR_*. */
int64_t swap_on(void);
int64_t swap_off(void);

/* What SYS_SWAP's SWAP_STATUS reports. */
void swap_status(struct sys_swap *out);

/* A free slot (SWAP_NONE: none, or swap is off), and giving one back.
 * (35c) A slot may be named by several page-table entries (a page forked
 * processes share, a fork of an entry): each holds it (swap_slot_hold:
 * false if it cannot, at 65535), and it is free once the last gives it. */
uint64_t swap_slot_take(void);
void swap_slot_give(uint64_t slot);
bool swap_slot_hold(uint64_t slot);
unsigned swap_slot_refs(uint64_t slot);         /* (how many name it now) */

/* A page (4 KiB of kernel memory) written to a slot, or read from one. */
bool swap_write(uint64_t slot, const void *page);
bool swap_read(uint64_t slot, void *page);

/* (35b) For page faults: whether free memory is down to the reserve while
 * swap is on (a program's fault then waits instead of taking a page);
 * whether swap is on; a look after taking one, which wakes pageout below its
 * low mark; and the wait (up to a second) for pageout to free pages: 1 when
 * memory is above the reserve again, 0 when the second ran out, -1 when
 * pageout found nothing to write (or swap is off): waiting longer is no use. */
bool swap_short(void);
bool swap_on_now(void);
void swap_note(void);
int swap_wait_memory(void);

/* REPORT_MEMORY's line about swap (mem). */
void swap_print(void);

#endif
