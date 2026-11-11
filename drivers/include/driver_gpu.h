/*
 * (67f) The graphics class of the driver interface (driver.h): a graphics
 * chip's driver tells the kernel's graphics layer (one for every chip, as
 * the network stack is for cards) the chip's outputs and what it can do;
 * the layer names the device (gpu0, ...) and the outputs across all
 * devices (vga0, hdmi0, dp0, edp0 ...), watches every output for monitors
 * (their EDID, read through the driver) every 2 s, prints `displays` and
 * runs the system calls (setting a mode, the vertical blank, flipping
 * screen buffers) through the driver's operations. An operation the chip
 * cannot do is left 0.
 *
 * A driver keeps a struct gpu_card (in its own state), fills it in (the
 * outputs from what the chip and its firmware report) and registers it
 * with gpu_register once the chip is understood; the layer then calls its
 * operations with that card. A monitor on an output is the layer's to know
 * (gpu_output_monitor: its EDID, parsed).
 */
#ifndef RITTEROS_DRIVER_GPU_H
#define RITTEROS_DRIVER_GPU_H

#include "driver.h"

#define GPU_OUTPUTS 8
#define EDID_TIMINGS 64
#define EDID_DETAILED 16

struct edid_timing {
    uint32_t width, height;
    uint32_t refresh_mhz;               /* millihertz: 59950 is 59.95 Hz */
};

/* A mode's whole timing, as a driver programs it: pixels from the line's
 * (or frame's) start, where sync begins and ends and the total; which way
 * each sync pulse goes. */
struct edid_detailed {
    uint32_t clock_khz;
    uint32_t hactive, hsync_start, hsync_end, htotal;
    uint32_t vactive, vsync_start, vsync_end, vtotal;
    bool hsync_high, vsync_high;
    uint32_t refresh_mhz;               /* (from the clock and totals) */
};

/* A monitor's EDID, parsed. */
struct edid_info {
    bool valid;                         /* the header and every block's checksum were right */
    uint32_t blocks;                    /* 128-byte blocks read (the base and extensions) */
    char name[14];                      /* the monitor name descriptor, "" if none */
    struct edid_timing native;          /* the first detailed timing (0 if none) */
    struct edid_timing timings[EDID_TIMINGS];
    int count;
    struct edid_detailed detailed[EDID_DETAILED];   /* every detailed timing, whole */
    int detailed_count;
};

/* The screen as set_mode made it: its first pixel (32 bits each, a kernel
 * address), size, the pixels from one row to the next, each 8-bit color's
 * bit position. */
struct gpu_screen {
    volatile uint32_t *base;
    uint32_t width, height, stride;
    uint8_t red_shift, green_shift, blue_shift;
};

struct gpu_card_output {
    const char *kind;                   /* "vga", "hdmi", "dp", "edp": named kind + a number */
    const char *where;                  /* for the report: "the VGA port", "port B", "the panel" */
};

struct gpu_card {
    const char *model;                  /* "Intel Bay Trail HD Graphics" */
    struct pci_device *pci;
    int output_count;
    struct gpu_card_output outputs[GPU_OUTPUTS];
    void *priv;                         /* the driver's own */
    /* Read len bytes (at most 256) of the EDID on output i from offset:
     * false if no monitor answers. From the layer's thread only. */
    bool (*read_edid)(struct gpu_card *card, int i, uint32_t offset, uint8_t *buf, uint32_t len);
    /* Does output i show the picture? */
    bool (*showing)(struct gpu_card *card, int i);
    /* The driver's own lines at the end of its part of `displays` (counters
     * for what only the hardware shows, the state as read back). */
    void (*report)(struct gpu_card *card);
    /* Show timing t on output i (the one showing the picture): the new
     * screen in *screen; false with the reason in why when it refuses
     * (nothing touched). One change at a time. */
    bool (*set_mode)(struct gpu_card *card, int i, const struct edid_detailed *t,
                     struct gpu_screen *screen, char *why, size_t why_size);
    /* Wait, at most timeout_ms, for the next vertical blank of the picture
     * output i shows (its frame number in *frame); false on a time-out, when
     * the thread is asked to stop (thread_stop_requested) or when the driver
     * has no interrupt for it. */
    bool (*wait_vblank)(struct gpu_card *card, int i, uint64_t *frame, uint32_t timeout_ms);
    /* Show screen buffer `buffer` (0: the one the console draws in; the
     * driver makes the others the screen's size, black) on output i from
     * its next vertical blank; true once it shows (its frame in *frame). */
    bool (*flip)(struct gpu_card *card, int i, int buffer, uint64_t *frame);
    /* The screen buffers of output i made ready: their physical place
     * (*phys; buffer k at k x *slot), how many, the stride in bytes. */
    bool (*buffers)(struct gpu_card *card, int i, uint64_t *phys, uint64_t *slot, int *count,
                    uint32_t *stride);
    /* (3.0) Switch the backlight of output i's panel off, or on again as it
     * was (its brightness kept); (37e-2d) on an external monitor's output,
     * its signal (so the monitor goes to standby): false when the output
     * has neither. The layer
     * calls it when the screen goes off and comes back (the console's
     * thread), and with on at a crash, on any core with the others stopped:
     * so it takes no lock and does not sleep. */
    bool (*backlight)(struct gpu_card *card, int i, bool on);
    /* (4.0) Set the brightness of output i's panel to level (of 0xffff),
     * raised to the lowest its firmware allows; false when it has none.
     * The layer's call (the console's thread: it may wait a frame). */
    bool (*brightness)(struct gpu_card *card, int i, uint32_t level);
    /* (4.0) Output i's brightness now (of 0xffff) in *level: false when it
     * has none. */
    bool (*brightness_now)(struct gpu_card *card, int i, uint32_t *level);
    /* (6.0) The driver's lines of `displays sleep`: what it kept over the
     * last sleep (the registers before it and after it), or why nothing. */
    void (*report_sleep)(struct gpu_card *card);
    void *state;                        /* (the layer's) */
};

#ifndef RITTEROS_KERNEL

/* The chip is understood: the layer names it and its outputs, looks at
 * every output once (logged) and watches them from now on. */
DRIVER_WRAPPED void gpu_register(struct gpu_card *card);

/* Output i's name ("hdmi0"; after gpu_register), and the monitor on it:
 * its EDID parsed, or 0 when none answers. */
const char *gpu_output_name(struct gpu_card *card, int i);
const struct edid_info *gpu_output_monitor(struct gpu_card *card, int i);

#endif /* RITTEROS_KERNEL */
#endif
