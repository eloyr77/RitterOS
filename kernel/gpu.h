/*
 * (66a, roadmap item 33) The graphics layer, the same for every graphics
 * chip, as net.c is for network cards: the graphics devices (gpu0, gpu1...
 * in the order found), their outputs numbered across all of them (vga0,
 * hdmi0, dp0...), each connected monitor's EDID, watched for monitors
 * plugged in, swapped or unplugged, and the `displays` report (which also
 * lists graphics devices that have no driver). What a chip does its own
 * way is an operation of its driver's struct gpu; nothing here reaches
 * into a driver.
 */
#ifndef RITTEROS_GPU_H
#define RITTEROS_GPU_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "edid.h"
#include "framebuffer.h"
#include "pci.h"
#include "syscalls.h"

#define GPU_OUTPUTS 8

struct gpu_output {
    const char *kind;                   /* "vga", "hdmi", "dp": its name is kind + a number */
    const char *where;                  /* for the report: "the VGA port", "port B" */
    char name[8];                       /* (the layer's) vga0, hdmi1 ... */
    bool connected;                     /* (the layer's, under its lock) */
    uint8_t ident[16];
    uint8_t edid[256];
    uint32_t edid_size;
    struct edid_info info;
    bool dimmed;                        /* (37e-2c, the layer's) its brightness lowered, */
    uint32_t undimmed;                  /* from this level (of 0xffff) */
};

struct gpu {
    char name[8];                       /* (the layer's) gpu0 ... */
    const char *model;
    struct pci_device *pci;
    int output_count;
    struct gpu_output outputs[GPU_OUTPUTS];
    void *priv;                         /* the driver's own */
    /* Read len bytes (at most 256) of the EDID on output i from `offset`:
     * false if no monitor answers. Called from the layer's thread only. */
    bool (*read_edid)(struct gpu *g, int i, uint32_t offset, uint8_t *buf, uint32_t len);
    /* Is output i on (does it show the picture)? 0 when the driver cannot tell. */
    bool (*showing)(struct gpu *g, int i);
    /* The driver's own lines at the end of its part of `displays` (how its
     * reads went: counters for what only real hardware shows), or 0. */
    void (*report)(struct gpu *g);
    /* (66b-2) Show timing t on output i (the one showing the picture): the
     * chain off, the mode programmed, on again; the new framebuffer in *fb.
     * False with the reason in why when it refuses (nothing touched). Called
     * by the layer, one change at a time. 0 when the driver cannot. */
    bool (*set_mode)(struct gpu *g, int i, const struct edid_detailed *t, struct framebuffer *fb,
                     char *why, size_t why_size);
    /* (66c-2) Wait, at most timeout_ms, for the next vertical blank of the
     * pipe output i shows: the screen's frame number in *frame. False on a
     * time-out or when the driver has no interrupt for it. 0 when it cannot. */
    bool (*wait_vblank)(struct gpu *g, int i, uint64_t *frame, uint32_t timeout_ms);
    /* (66d-1) Show screen buffer `buffer` (0: the one the console draws in;
     * the driver makes the others the screen's size, black) on output i
     * from its next vertical blank; true once it shows (its frame in
     * *frame). 0 when the driver cannot. */
    bool (*flip)(struct gpu *g, int i, int buffer, uint64_t *frame);
    /* (66d-2) The screen buffers of output i made ready (the second
     * cleared): their physical place (*phys; buffer k at k x *slot), how
     * many, and the plane's stride in bytes. 0 when the driver cannot. */
    bool (*buffers)(struct gpu *g, int i, uint64_t *phys, uint64_t *slot, int *count,
                    uint32_t *stride);
    /* (37e-2b) Switch output i's backlight off, or on as it was (37e-2d: an
     * external monitor's signal): false when it has none. 0 when the driver
     * cannot. No lock, no sleep (a crash calls it too). */
    bool (*backlight)(struct gpu *g, int i, bool on);
    /* (37e-2c) Set output i's brightness (of 0xffff; raised to the panel's
     * lowest), and read it: false when it has none. 0 when the driver cannot. */
    bool (*brightness)(struct gpu *g, int i, uint32_t level);
    bool (*brightness_now)(struct gpu *g, int i, uint32_t *level);
    /* (37g-5c-2) Its lines of `displays sleep` (what it kept over the last
     * sleep), or 0. */
    void (*report_sleep)(struct gpu *g);
};

/* A driver found its chip: name it and its outputs, look at them once
 * (logged), and watch them from now on (a thread named after it). */
void gpu_register(struct gpu *g);

/* The `displays` report: the firmware's screen, every graphics device on
 * PCI (with its driver's outputs and monitors, or "no driver"). */
void gpu_print(void);

/* (37g-5c-2) `displays sleep`: each driver's part (what it kept over the
 * last sleep), or that none keeps anything. */
void gpu_print_sleep(void);

/* (66b-2) DISPLAY_SET_MODE: show `output`'s monitor at width x height
 * (refresh_mhz, or 0: the monitor's usual rate for that size), a mode the
 * monitor lists; the console follows. 0 or -ERR_*; said gets what was set
 * or why not (and the kernel's log the same). */
int gpu_set_mode(const char *output, uint32_t width, uint32_t height, uint32_t refresh_mhz,
                 char *said, size_t said_size);

/* (66b-6) Set the modes /system/config/displays asks for ("OUTPUT =
 * WxH[@HZ]" lines, # comments), once the system's file system is mounted;
 * each logged (`gpu0: hdmi0 now ... (/system/config/displays)`), or why
 * not. Without the file nothing changes. */
void gpu_load_settings(void);

/* (66c-2) Wait (at most timeout_ms) for the next vertical blank of the
 * screen: the output showing the picture, whose driver can tell. 1 with the
 * frame number in *frame, 0 on a time-out, -ERR_NODEV when no driver can. */
int gpu_wait_vblank(uint64_t *frame, uint32_t timeout_ms);

/* (66d-1) DISPLAY_FLIP: show screen buffer `buffer` on the screen. 1 once
 * it shows, 0 if it did not in time, -ERR_NODEV where no driver flips,
 * -ERR_INVAL for a buffer that does not exist. */
int gpu_flip(int buffer, uint64_t *frame);

/* (66d-1) The screen's owner let go: buffer 0 (the console's) shown again
 * if another was. */
void gpu_screen_back(void);

/* (37e-2b) Every output's backlight off or on (kernel/blank.c, with the
 * screen; 37e-2d: an external monitor's signal, so it goes to standby):
 * what was switched into said ("the backlight off: edp0", "the signal
 * off: hdmi0"; "" if nothing). */
void gpu_backlight(bool on, char *said, size_t size);
/* At a crash: every backlight on, taking no lock. */
void gpu_backlight_emergency(void);

/* (37g-5d) A sleep begins (true) or the drivers are back (false): in
 * between, the layer calls no driver. */
void gpu_sleep(bool going);

/* (37e-2c) Every panel dimmed to a third of its brightness (its level kept),
 * or back to the level kept: what changed into said ("edp0 0x9c00 to
 * 0x3400", "" if nothing). */
void gpu_dim(bool dim, char *said, size_t size);

/* (66d-2) DISPLAY_BUFFERS: the screen buffers' physical place, the slot
 * between them, and *d's count, size and stride (its addresses are the
 * caller's to fill). 0 or -ERR_NODEV. */
int gpu_buffers(struct sys_display_buffers *d, uint64_t *phys, uint64_t *slot);

#endif
