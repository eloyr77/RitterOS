/*
 * The firmware's events (37c-2). See acpi_events.h.
 *
 * x86's ACPI registers (ACPI 6.5 4.8): each PM1 event block is a status
 * register (bits set by the hardware, cleared by writing 1) and an enable
 * register of the same size behind it; a GPE block likewise, a byte of
 * status per 8 events, then the enable bytes. The SCI is raised while any
 * status bit has its enable bit set, so the handler clears what it takes
 * (a fixed event's status) or switches it off (a GPE's enable, until its
 * method has run) and the level-triggered line goes quiet.
 *
 * (37c-2b) A GPE runs as ACPICA dispatches it: off in the handler, an
 * edge's status cleared there; on the thread its \_GPE._Lxx or _Exx, a
 * level's status cleared after it, then on again. Only GPEs with a method
 * are on, and none a present device's _PRW names (those are for waking).
 * ARM's (hardware-reduced) events come through the GED (ACPI0013): its
 * interrupts, each one's _EVT(GSI) run on the thread (a level line masked
 * until then). A power button device's Notify(0x80) is a press as the
 * fixed button's is.
 *
 * (37c-2c) The embedded controller's GPE has no method: it says the
 * controller has events (its queries, each one's _Qxx run: ec.c), and is
 * handled as Linux's driver does, edge-triggered. The lid's and the AC
 * adapter's notifications are logged with their new state; the lid does
 * what /system/config/power's `lid` says.
 *
 * (37g-7) `power_button = sleep`, `lid = sleep` (a close) and `sleep_after`
 * minutes without input (blank.c asks: acpi_events_idle) start
 * `/system/<arch>/bin/power sleep`, as the button's `off` starts poweroff:
 * one way to sleep. From the sleep until its end a press of the power
 * button is only the wake's reason (the SCI notes it, nothing acts on it;
 * presses counted before are dropped then): a laptop woken by its button
 * must not switch off, or sleep again, at once (ACPICA's
 * AcpiHwLegacyWake clears the button's status for the same reason).
 */
#include "acpi_events.h"
#include "acpi.h"
#include "aml.h"
#include "blank.h"
#include "ec.h"
#include "arch.h"
#include "arch_api.h"
#include "kprintf.h"
#include "mounts.h"
#include "process.h"
#include "sched.h"
#include "string.h"
#include "sync.h"
#include "timer.h"
#include "vfs.h"

#define PM1_SCI_EN          (1u << 0)       /* control: ACPI mode is on */
#define PM1_PWRBTN          (1u << 8)       /* status and enable: the power button */
#define PM1_RTC             (1u << 10)      /* (37g-1) status and enable: the RTC's alarm */
/* every fixed event's status: timer, bus master, global lock, power and
 * sleep buttons, RTC alarm, wake */
#define PM1_ALL_STATUS      0x8731u
#define SCI_STORM           10000           /* SCIs in a row with nothing to handle */
#define GPES                256
#define GPE_STORM           100             /* one GPE's runs within a second: switched off */
#define GPE_FAILS_LOGGED    8
#define GED_LINES           8

#define POWER_FILE          "/system/config/power"
#define POWER_FILE_MAX      4096

enum button { BUTTON_OFF, BUTTON_SLEEP, BUTTON_NOTHING };
static const char *const button_names[] = { "off", "sleep", "nothing" };
enum lid { LID_NOTHING, LID_SLEEP };
static const char *const lid_names[] = { "nothing", "sleep" };

static struct {
    struct spinlock lock;               /* what the handlers and the thread share */
    struct wait_queue queue;            /* the thread waits here */
    bool started, sci, fixed_button, storm;
    uint16_t pm1[2];                    /* PM1a, PM1b event blocks (0: none) */
    uint8_t pm1_half;                   /* the status register's size, in bytes */
    uint16_t gpe[2];                    /* GPE0, GPE1 blocks */
    uint8_t gpe_half[2];
    unsigned gpe_base[2], gpe_total;    /* block 1's first GPE; how many GPEs */
    uint8_t enabled[2][128];            /* each block's enable bytes, as written */
    uint8_t gpe_pending[GPES / 8];      /* off, waiting for their method */
    uint8_t kind[GPES];                 /* 'L', 'E' or 0 (acpi_aml_gpes) */
    bool wake[GPES], gpe_off[GPES];     /* for waking only; switched off (a storm) */
    uint32_t gpe_runs[GPES];
    uint64_t gpe_window[GPES];          /* the second a GPE's storm count is in */
    uint32_t gpe_in_window[GPES];
    uint32_t gpe_failed, gpe_storms;
    struct acpi_ged_line ged[GED_LINES];
    int ged_count;
    uint32_t ged_pending, ged_interrupts, ged_runs, ged_failed;
    uint32_t gsi;
    bool level, active_low;
    unsigned vector;
    uint32_t interrupts, empty, empty_in_a_row;
    uint32_t presses, presses_taken;    /* the power button, and how many the thread took */
    bool asleep, woke_by_button;        /* (37g-7) from the sleep until its end */
    uint32_t presses_dropped;           /* (37g-7) before a sleep or while waking */
    bool idle_sleep;                    /* (37g-7) sleep_after's minutes passed: asked */
    unsigned sleep_after;               /* (37g-7) minutes, 0 never */
    char button_devices[96];            /* PNP0C0C devices, if the firmware has them */
    enum button button;
    const char *button_from;
    int ec_gpe;                         /* (37c-2c) the embedded controller's, -1 none */
    uint32_t notified;                  /* 1 << ACPI_EVENT_*: waiting for the thread */
    uint32_t lid_changes, ac_changes, battery_notes;
    enum lid lid;
    const char *lid_from;
} ev = { .lock = SPINLOCK_INIT("acpi events"), .button_from = "the default: not read yet",
         .ec_gpe = -1, .lid_from = "the default: not read yet" };

static uint32_t port_in(uint16_t port, int width)
{
    uint32_t v = 0;
    arch_port_access(port, width, &v, false);
    return v;
}

static void port_out(uint16_t port, int width, uint32_t v)
{
    arch_port_access(port, width, &v, true);
}

/* A PM1 register (status at 0, enable at the block's half): a and b
 * together, as ACPI wants them read and written. */
static uint32_t pm1_read(int at)
{
    uint32_t v = 0;
    for (int k = 0; k < 2; k++)
        if (ev.pm1[k])
            v |= port_in((uint16_t)(ev.pm1[k] + at), 16);
    return v;
}

static void pm1_write(int at, uint32_t v)
{
    for (int k = 0; k < 2; k++)
        if (ev.pm1[k])
            port_out((uint16_t)(ev.pm1[k] + at), 16, v);
}

/* Where GPE g is: its block, byte and bit. */
static bool gpe_place(unsigned g, int *block, unsigned *byte, uint8_t *bit)
{
    for (int b = 0; b < 2; b++)
        if (ev.gpe_half[b] && g >= ev.gpe_base[b] && g < ev.gpe_base[b] + 8u * ev.gpe_half[b]) {
            *block = b;
            *byte = (g - ev.gpe_base[b]) / 8;
            *bit = (uint8_t)(1u << (g - ev.gpe_base[b]) % 8);
            return true;
        }
    return false;
}

/* (under ev.lock) */
static void gpe_enable(unsigned g, bool on)
{
    int b;
    unsigned i;
    uint8_t bit;
    if (!gpe_place(g, &b, &i, &bit))
        return;
    ev.enabled[b][i] = on ? ev.enabled[b][i] | bit : ev.enabled[b][i] & ~bit;
    port_out((uint16_t)(ev.gpe[b] + ev.gpe_half[b] + i), 8, ev.enabled[b][i]);
}

static void gpe_clear(unsigned g)
{
    int b;
    unsigned i;
    uint8_t bit;
    if (gpe_place(g, &b, &i, &bit))
        port_out((uint16_t)(ev.gpe[b] + i), 8, bit);
}

/* The SCI: core 0, interrupts masked. Only notes what came; the thread
 * acts on it. */
static void sci(void *context)
{
    (void)context;
    uint64_t state = spin_lock(&ev.lock);
    ev.interrupts++;
    uint32_t fixed = pm1_read(0) & pm1_read(ev.pm1_half);
    bool any = false;
    if (fixed & PM1_PWRBTN) {
        pm1_write(0, PM1_PWRBTN);               /* (writing 1 clears it) */
        if (ev.asleep)
            ev.woke_by_button = true;           /* (37g-7) the wake's reason, not a press */
        else
            ev.presses++;
        any = true;
    }
    for (int b = 0; b < 2; b++) {
        for (unsigned i = 0; i < ev.gpe_half[b]; i++) {
            uint8_t came = (uint8_t)port_in((uint16_t)(ev.gpe[b] + i), 8) & ev.enabled[b][i];
            if (!came)
                continue;
            ev.enabled[b][i] &= (uint8_t)~came;    /* off until its method has run */
            port_out((uint16_t)(ev.gpe[b] + ev.gpe_half[b] + i), 8, ev.enabled[b][i]);
            for (unsigned k = 0; k < 8; k++) {
                if (!(came & 1u << k))
                    continue;
                unsigned g = ev.gpe_base[b] + 8 * i + k;
                if (ev.kind[g] == 'E' || ev.kind[g] == 'C')    /* (edge-triggered) */
                    port_out((uint16_t)(ev.gpe[b] + i), 8, 1u << k);
                ev.gpe_pending[g / 8] |= (uint8_t)(1u << g % 8);
            }
            any = true;
        }
    }
    if (any) {
        ev.empty_in_a_row = 0;
    } else {
        ev.empty++;
        if (++ev.empty_in_a_row == SCI_STORM) {
            arch_gsi_mask(ev.gsi);              /* (something we do not handle holds it up) */
            ev.storm = true;
            any = true;
        }
    }
    spin_unlock(&ev.lock, state);
    if (any)
        wait_queue_wake_all(&ev.queue);
}

/* A GED line (context: its index): a level one is masked until its _EVT
 * has run. */
static void ged_interrupt(void *context)
{
    int i = (int)(uintptr_t)context;
    uint64_t state = spin_lock(&ev.lock);
    ev.ged_interrupts++;
    if (ev.ged[i].level)
        arch_gsi_mask(ev.ged[i].gsi);
    ev.ged_pending |= 1u << i;
    spin_unlock(&ev.lock, state);
    wait_queue_wake_all(&ev.queue);
}

void acpi_events_notify(int what)
{
    uint64_t state = spin_lock(&ev.lock);
    ev.notified |= 1u << what;
    if (what == ACPI_EVENT_BATTERY)
        ev.battery_notes++;
    spin_unlock(&ev.lock, state);
    wait_queue_wake_all(&ev.queue);
}

void acpi_events_button(void)
{
    uint64_t state = spin_lock(&ev.lock);
    if (!ev.asleep)                             /* (37g-7) */
        ev.presses++;
    spin_unlock(&ev.lock, state);
    wait_queue_wake_all(&ev.queue);
}

/* ---- The power button's action ---- */

/* poweroff, or (37g-7) `power sleep`, started for `why` ("the power
 * button: power_button = off") and awaited: its exit code, -1 when it did
 * not start. */
static int start_program(const char *why, const char *name, const char *arg)
{
    char path[64];
    ksnprintf(path, sizeof(path), "/system/%s/bin/%s", ARCH_NAME, name);
    struct vfs *fs = mounts_root();
    if (!fs) {
        kprintf("acpi: %s: no file system to start %s from\n", why, path);
        return -1;
    }
    const char *argv[] = { name, arg, 0 };
    int error, status;
    struct process *p = process_create(fs, path, arg ? 2 : 1, argv, 0, 0, 0, 0, &error);
    if (!p) {
        kprintf("acpi: %s: %s: %s\n", why, path, process_error_text(error));
        return -1;
    }
    int pid = process_id(p);
    /* (before it starts: it may switch off before this thread runs again) */
    kprintf("acpi: %s: %s%s%s, process %d\n", why, path, arg ? " " : "", arg ? arg : "", pid);
    if (!process_start(p)) {
        kprintf("acpi: %s: %s: out of memory\n", why, path);
        return -1;
    }
    process_wait(0, pid, &status, false);
    return status >> 8;
}

/* (37g-7) Asleep through the power program, as `power sleep` typed. */
static void start_sleep(const char *why)
{
    int code = start_program(why, "power", "sleep");
    if (code >= 0)
        kprintf("acpi: power sleep ended (exit code %d)\n", code);
}

static void pressed(void)
{
    if (ev.button == BUTTON_OFF) {
        int code = start_program("the power button: power_button = off", "poweroff", 0);
        if (code >= 0)
            kprintf("acpi: poweroff ended (exit code %d): the machine is still on\n", code);
    } else if (ev.button == BUTTON_SLEEP) {
        start_sleep("the power button: power_button = sleep");
    } else {
        kprintf("acpi: the power button: power_button = nothing\n");
    }
}

/* (37c-2c) The lid's and the AC adapter's new state. */
static void lid_changed(void)
{
    char path[96];
    int open = acpi_aml_lid(path, sizeof(path));
    ev.lid_changes++;
    if (open < 0)
        kprintf("acpi: the lid: its state could not be read\n");
    else if (open || ev.lid == LID_NOTHING)
        kprintf("acpi: the lid %s: %s\n", path, open ? "open" : "closed (lid = nothing)");
    else
        start_sleep("the lid: closed (lid = sleep)");   /* (37g-7) */
}

/* (37g-7) blank.c: sleep_after's minutes without input passed. */
void acpi_events_idle(void)
{
    uint64_t state = spin_lock(&ev.lock);
    ev.idle_sleep = true;
    spin_unlock(&ev.lock, state);
    wait_queue_wake_all(&ev.queue);
}

static void ac_changed(void)
{
    char path[96];
    int on = acpi_aml_ac(path, sizeof(path));
    ev.ac_changes++;
    if (on < 0)
        kprintf("acpi: the AC adapter: its state could not be read\n");
    else
        kprintf("acpi: the AC adapter %s: %s\n", path, on ? "plugged in" : "unplugged");
}

/* GPE g's method, then on again (unless it came too often). */
static void run_gpe(unsigned g)
{
    char kind = (char)ev.kind[g];
    bool ok = kind == 'C' ? acpi_aml_ec_queries() >= 0 : kind && acpi_aml_gpe_run(g, kind);
    uint64_t now = timer_elapsed_ms(0) / 1000, state = spin_lock(&ev.lock);
    ev.gpe_runs[g]++;
    if (!ok)
        ev.gpe_failed++;
    if (ev.gpe_window[g] != now)
        ev.gpe_window[g] = now, ev.gpe_in_window[g] = 0;
    bool storm = ++ev.gpe_in_window[g] > GPE_STORM;
    if (kind == 'L')
        gpe_clear(g);                       /* (its source quiet now) */
    if (storm) {
        ev.gpe_off[g] = true;
        ev.gpe_storms++;
    } else {
        gpe_enable(g, true);
    }
    uint32_t failed = ev.gpe_failed;
    spin_unlock(&ev.lock, state);
    if (!ok && failed <= GPE_FAILS_LOGGED)
        kprintf("acpi: GPE 0x%02X: \\_GPE._%c%02X failed\n", g, kind ? kind : '?', g);
    if (storm)
        kprintf("acpi: GPE 0x%02X came more than %u times in a second: switched off\n", g,
                GPE_STORM);
}

static bool work_waiting(void)
{
    if (ev.presses != ev.presses_taken || ev.storm || ev.ged_pending || ev.notified ||
        ev.idle_sleep)
        return true;
    for (unsigned i = 0; i < GPES / 8; i++)
        if (ev.gpe_pending[i])
            return true;
    return false;
}

static void events_thread(void *arg)
{
    (void)arg;
    for (;;) {
        wait_event(&ev.queue, work_waiting());
        uint8_t gpes[GPES / 8];
        uint64_t state = spin_lock(&ev.lock);
        bool storm = ev.storm;
        uint32_t ged = ev.ged_pending;
        memcpy(gpes, ev.gpe_pending, sizeof(gpes));
        memset(ev.gpe_pending, 0, sizeof(ev.gpe_pending));
        ev.ged_pending = 0;
        ev.storm = false;
        spin_unlock(&ev.lock, state);
        if (storm)
            kprintf("acpi: the SCI came %u times in a row with nothing to handle: "
                    "switched off\n", SCI_STORM);
        for (unsigned g = 0; g < GPES; g++)
            if (gpes[g / 8] & 1u << g % 8)
                run_gpe(g);
        for (int i = 0; i < ev.ged_count; i++) {
            if (!(ged & 1u << i))
                continue;
            bool ok = acpi_aml_ged_run(ev.ged[i].device, ev.ged[i].gsi);
            state = spin_lock(&ev.lock);
            ev.ged_runs++;
            ev.ged_failed += !ok;
            if (ev.ged[i].level)
                arch_gsi_unmask(ev.ged[i].gsi);
            spin_unlock(&ev.lock, state);
            if (!ok)
                kprintf("acpi: the GED's _EVT(%u) failed\n", ev.ged[i].gsi);
        }
        /* (the methods above may have notified: the button, the lid...) */
        state = spin_lock(&ev.lock);
        bool press = ev.presses != ev.presses_taken;
        ev.presses_taken = ev.presses;          /* (presses while one is handled: one) */
        uint32_t notified = ev.notified;
        ev.notified = 0;
        bool idle = ev.idle_sleep;
        ev.idle_sleep = false;
        spin_unlock(&ev.lock, state);
        if (notified & 1u << ACPI_EVENT_LID)
            lid_changed();
        if (notified & 1u << ACPI_EVENT_AC)
            ac_changed();
        if (press)
            pressed();
        if (idle) {
            char why[64];
            ksnprintf(why, sizeof why, "%u minute%s without input (sleep_after)", ev.sleep_after,
                      ev.sleep_after == 1 ? "" : "s");
            start_sleep(why);
        }
    }
}

/* ---- Starting ---- */

static void ports_from(const struct acpi_fadt *f)
{
    ev.pm1[0] = (uint16_t)f->pm1a_event_block;
    ev.pm1[1] = (uint16_t)f->pm1b_event_block;
    ev.pm1_half = f->pm1_event_length / 2;
    ev.gpe[0] = (uint16_t)f->gpe0_block;
    ev.gpe[1] = (uint16_t)f->gpe1_block;
    ev.gpe_half[0] = ev.gpe[0] ? f->gpe0_length / 2 : 0;
    ev.gpe_half[1] = ev.gpe[1] ? f->gpe1_length / 2 : 0;
    ev.gpe_base[1] = f->gpe1_base;
    for (int b = 0; b < 2; b++) {
        if (ev.gpe_base[b] >= GPES)
            ev.gpe_half[b] = 0;
        if (ev.gpe_base[b] + 8u * ev.gpe_half[b] > GPES)      /* (GPE numbers stop at 255) */
            ev.gpe_half[b] = (uint8_t)((GPES - ev.gpe_base[b]) / 8);
        if (ev.gpe_half[b] && ev.gpe_base[b] + 8u * ev.gpe_half[b] > ev.gpe_total)
            ev.gpe_total = ev.gpe_base[b] + 8u * ev.gpe_half[b];
    }
}

/* ACPI mode (SCI_EN): the firmware's SMI handler gives the events to the
 * SCI once the OS asks through the SMI command port (ACPI 6.5 4.8.3.2.1). */
static bool acpi_mode(const struct acpi_fadt *f, bool *switched)
{
    uint16_t control = (uint16_t)f->pm1a_control_block;
    *switched = false;
    if (port_in(control, 16) & PM1_SCI_EN)
        return true;
    if (!f->smi_command || !f->acpi_enable)
        return false;
    port_out((uint16_t)f->smi_command, 8, f->acpi_enable);
    for (int ms = 0; ms < 1000; ms++) {
        if (port_in(control, 16) & PM1_SCI_EN) {
            *switched = true;
            return true;
        }
        timer_sleep_ms(1);
    }
    return false;
}

/* The fixed hardware's part (x86): the SCI, the power button, the GPEs. */
static void start_sci(const struct acpi_fadt *f)
{
    ports_from(f);
    bool switched;
    if (!acpi_mode(f, &switched)) {
        kprintf("  ACPI: events: the firmware did not switch ACPI mode on: no SCI\n");
        return;
    }
    /* Everything off and cleared before the line is open. */
    pm1_write(ev.pm1_half, 0);
    pm1_write(0, PM1_ALL_STATUS);
    for (int b = 0; b < 2; b++)
        for (unsigned i = 0; i < ev.gpe_half[b]; i++) {
            port_out((uint16_t)(ev.gpe[b] + ev.gpe_half[b] + i), 8, 0);
            port_out((uint16_t)(ev.gpe[b] + i), 8, 0xFF);
        }
    acpi_aml_gpes(ev.kind, ev.wake, GPES);
    int ec = ec_gpe();                      /* (37c-2c) on, even if it also wakes */
    if (ec >= 0 && ec < GPES) {
        ev.ec_gpe = ec;
        ev.kind[ec] = 'C';
        ev.wake[ec] = false;
    }
    if (!arch_route_sci(f->sci_interrupt, sci, 0, &ev.gsi, &ev.level, &ev.active_low,
                        &ev.vector)) {
        kprintf("  ACPI: events: the SCI (IRQ %u) cannot be routed: none\n", f->sci_interrupt);
        return;
    }
    ev.sci = true;
    ev.fixed_button = !(f->flags & FADT_PWR_BUTTON);
    if (ev.fixed_button)
        pm1_write(ev.pm1_half, PM1_PWRBTN);
    unsigned methods = 0, wake = 0, on = 0;
    char list[64] = "";
    size_t at = 0;
    uint64_t state = spin_lock(&ev.lock);
    for (unsigned g = 0; g < ev.gpe_total; g++) {
        int b;
        unsigned i;
        uint8_t bit;
        if (!ev.kind[g] || !gpe_place(g, &b, &i, &bit))
            continue;
        methods += ev.kind[g] != 'C';
        if (ev.wake[g]) {
            wake++;
            continue;
        }
        gpe_enable(g, true);
        on++;
        if (at + 6 < sizeof(list))
            at += (size_t)ksnprintf(list + at, sizeof(list) - at, " 0x%02X", g);
    }
    spin_unlock(&ev.lock, state);
    char ec_text[48] = "";
    if (ev.ec_gpe >= 0)
        ksnprintf(ec_text, sizeof(ec_text), " (0x%02X the embedded controller's)",
                  (unsigned)ev.ec_gpe);
    kprintf("  ACPI: events: SCI IRQ %u, GSI %u (%s, active %s), vector 0x%02x%s; PM1 event "
            "0x%X, GPE0 0x%X: %u GPEs, %u with methods (%u for waking only), on:%s%s; the "
            "power button %s\n", f->sci_interrupt, ev.gsi, ev.level ? "level" : "edge",
            ev.active_low ? "low" : "high", ev.vector,
            switched ? "; ACPI mode switched on" : "", (unsigned)ev.pm1[0],
            (unsigned)ev.gpe[0], ev.gpe_total, methods, wake, on ? list : " none", ec_text,
            ev.fixed_button ? "(fixed): on" : "a device");
}

/* (37g-1) Around a sleep (ACPI 6.5 16.1.6, as Linux's
 * acpi_hw_legacy_sleep/_wake): before it every GPE off, every status
 * cleared and only the RTC's alarm (37g-7: with one) and the fixed power
 * button enabled to wake; after it ACPI mode again if the firmware left
 * it, PM1's status (what woke the machine) cleared and every enable as
 * before. (37g-7) Presses are not acted on from here until
 * acpi_events_awake. */
void acpi_events_sleep(bool going, bool alarm, uint32_t *wake_status)
{
    uint64_t state = spin_lock(&ev.lock);
    if (going) {
        ev.asleep = true;
        ev.woke_by_button = false;
    }
    if (!ev.sci) {
        spin_unlock(&ev.lock, state);
        return;
    }
    for (int b = 0; b < 2; b++)
        for (unsigned i = 0; i < ev.gpe_half[b]; i++) {
            port_out((uint16_t)(ev.gpe[b] + ev.gpe_half[b] + i), 8,
                     going ? 0 : ev.enabled[b][i]);
            if (going)
                port_out((uint16_t)(ev.gpe[b] + i), 8, 0xFF);
        }
    if (going) {
        pm1_write(0, PM1_ALL_STATUS);
        pm1_write(ev.pm1_half, (ev.fixed_button ? PM1_PWRBTN : 0) | (alarm ? PM1_RTC : 0));
    } else {
        if (wake_status)        /* (the SCI may have taken the button's status first) */
            *wake_status = pm1_read(0) | (ev.woke_by_button ? PM1_PWRBTN : 0);
        pm1_write(0, PM1_ALL_STATUS);
        pm1_write(ev.pm1_half, ev.fixed_button ? PM1_PWRBTN : 0);
    }
    spin_unlock(&ev.lock, state);
    if (!going) {
        bool switched;
        if (!acpi_mode(acpi_fadt(), &switched))
            kprintf("  ACPI: events: ACPI mode is off after the sleep and does not come back\n");
    }
}

/* (37g-7) The sleep's end (every driver back): presses again; those
 * counted before it, or while waking, are dropped. */
void acpi_events_awake(void)
{
    uint64_t state = spin_lock(&ev.lock);
    ev.presses_dropped += ev.presses - ev.presses_taken;
    ev.presses_taken = ev.presses;
    ev.asleep = false;
    spin_unlock(&ev.lock, state);
}

/* The GED's lines (ARM; any machine whose firmware has one). */
static void start_ged(void)
{
    struct acpi_ged_line found[GED_LINES];
    int n = acpi_aml_ged_lines(found, GED_LINES);
    for (int i = 0; i < n; i++) {
        unsigned vector;
        char path[96];
        aml_path(found[i].device, path, sizeof(path));
        ev.ged[ev.ged_count] = found[i];
        if (!arch_route_gsi(found[i].gsi, found[i].level, found[i].active_low, ged_interrupt,
                            (void *)(uintptr_t)ev.ged_count, &vector)) {
            kprintf("  ACPI: events: GED %s, GSI %u cannot be routed\n", path, found[i].gsi);
            continue;
        }
        ev.ged_count++;
        kprintf("  ACPI: events: GED %s, GSI %u (%s, active %s)\n", path, found[i].gsi,
                found[i].level ? "level" : "edge", found[i].active_low ? "low" : "high");
    }
}

void acpi_events_start(void)
{
    const struct acpi_fadt *f = acpi_fadt();
    if (!f)
        return;
    if (!thread_create("acpi", events_thread, 0)) {
        kprintf("  ACPI: events: no thread: none\n");
        return;
    }
    if (!(f->flags & FADT_HARDWARE_REDUCED) && f->pm1a_event_block && f->pm1a_control_block &&
        f->pm1_event_length >= 4)
        start_sci(f);
    start_ged();
    acpi_aml_buttons(ev.button_devices, sizeof(ev.button_devices));
    ev.started = ev.sci || ev.ged_count;
    if (ev.started && ev.button_devices[0])
        kprintf("  ACPI: events: the power button device %s\n", ev.button_devices);
}

/* ---- The setting ---- */

static bool blank(char c)
{
    return c == ' ' || c == '\t' || c == '\r';
}

/* The value of key in text ("key = value" lines; # starts a comment; the
 * last one counts): its place and length, or false. */
static bool setting(const char *text, size_t n, const char *key, const char **value,
                    size_t *length)
{
    size_t k = strlen(key);
    bool found = false;
    for (size_t at = 0; at < n;) {
        size_t end = at;
        while (end < n && text[end] != '\n')
            end++;
        size_t i = at;
        while (i < end && blank(text[i]))
            i++;
        if (end - i > k && !memcmp(text + i, key, k) && (blank(text[i + k]) || text[i + k] == '=')) {
            i += k;
            while (i < end && blank(text[i]))
                i++;
            if (i < end && text[i] == '=') {
                i++;
                while (i < end && blank(text[i]))
                    i++;
                size_t stop = i;
                while (stop < end && !blank(text[stop]) && text[stop] != '#')
                    stop++;
                *value = text + i, *length = stop - i;
                found = true;
            }
        }
        at = end + 1;
    }
    return found;
}

/* (37e) A number of minutes: the key's value, else the default (a value
 * that is not a number logged). */
static unsigned minutes_setting(const char *text, int64_t got, const char *key, unsigned dflt)
{
    const char *value;
    size_t length;
    if (got < 0 || !setting(text, (size_t)got, key, &value, &length))
        return dflt;
    unsigned n = 0;
    size_t i = 0;
    while (i < length && value[i] >= '0' && value[i] <= '9' && n < 100000)
        n = n * 10 + (unsigned)(value[i++] - '0');
    if (!length || i != length) {
        kprintf("  %-15s%s = %.*s in %s is not a number of minutes: %u\n", "power:", key,
                (int)(length < 32 ? length : 32), value, POWER_FILE, dflt);
        return dflt;
    }
    return n;
}

void acpi_events_settings(void)
{
    static char text[POWER_FILE_MAX];
    struct vfs *fs = mounts_root();
    uint64_t id;
    struct rfs_inode inode;
    int64_t got = fs && vfs_lookup(fs, POWER_FILE, &id, &inode)
                  ? vfs_read(fs, id, &inode, 0, text, sizeof(text)) : -1;
    const char *value;
    size_t length;
    enum button b = BUTTON_OFF;
    const char *from = got < 0 ? "the default: no " POWER_FILE : POWER_FILE;
    if (got >= 0 && !setting(text, (size_t)got, "power_button", &value, &length)) {
        from = "the default: no power_button in " POWER_FILE;
    } else if (got >= 0) {
        int k = 0;
        while (k < 3 && (strlen(button_names[k]) != length ||
                         memcmp(button_names[k], value, length)))
            k++;
        if (k == 3)
            kprintf("  %-15spower_button = %.*s in %s is not a setting: off\n", "power:",
                    (int)(length < 32 ? length : 32), value, POWER_FILE);
        else
            b = (enum button)k;
    }
    ev.button = b;
    ev.button_from = from;
    kprintf("  %-15spower_button = %s (%s)%s\n", "power:", button_names[b], from,
            ev.started ? "" : "; no button events on this machine");
    /* (37c-2c) the lid */
    enum lid l = LID_NOTHING;
    from = got < 0 ? "the default: no " POWER_FILE : POWER_FILE;
    if (got >= 0 && !setting(text, (size_t)got, "lid", &value, &length)) {
        from = "the default: no lid in " POWER_FILE;
    } else if (got >= 0) {
        int k = 0;
        while (k < 2 && (strlen(lid_names[k]) != length || memcmp(lid_names[k], value, length)))
            k++;
        if (k == 2)
            kprintf("  %-15slid = %.*s in %s is not a setting: nothing\n", "power:",
                    (int)(length < 32 ? length : 32), value, POWER_FILE);
        else
            l = (enum lid)k;
    }
    ev.lid = l;
    ev.lid_from = from;
    kprintf("  %-15slid = %s (%s)\n", "power:", lid_names[l], from);
    /* (37e) the screen: off after screen_off minutes without input (0
     * never); (37e-2c) dimmed at screen_dim when that comes first */
    unsigned off = minutes_setting(text, got, "screen_off", 5);
    unsigned dim = minutes_setting(text, got, "screen_dim", 2);
    /* (37g-7) asleep after sleep_after minutes without input (0 never) */
    unsigned after = minutes_setting(text, got, "sleep_after", 0);
    ev.sleep_after = after;
    blank_set(off, dim, after);
    kprintf("  %-15sscreen_off = %u, screen_dim = %u (minutes; %s): %s\n", "power:", off, dim,
            got < 0 ? "the defaults: no " POWER_FILE : POWER_FILE,
            off ? "the screen goes off" : "the screen stays on");
    kprintf("  %-15ssleep_after = %u (minutes; %s): %s\n", "power:", after,
            got < 0 ? "the default: no " POWER_FILE : POWER_FILE,
            after ? "the machine sleeps without input" : "it never sleeps by itself");
}

void acpi_events_print(void)
{
    if (!ev.started) {
        kprintf("events: none (no SCI or GED on this machine)\n");
        return;
    }
    char text[200] = "";
    size_t at = 0;
    uint64_t state = spin_lock(&ev.lock);
    uint32_t presses = ev.presses, dropped = ev.presses_dropped;
    if (ev.sci) {
        uint32_t runs = 0, on = 0;
        for (unsigned g = 0; g < ev.gpe_total; g++) {
            runs += ev.gpe_runs[g];
            on += ev.kind[g] && !ev.wake[g] && !ev.gpe_off[g];
        }
        at += (size_t)ksnprintf(text + at, sizeof(text) - at,
                                "the SCI (GSI %u): %u interrupts, %u with nothing to handle; "
                                "GPEs: %u on, %u runs, %u failed, %u switched off; ", ev.gsi,
                                ev.interrupts, ev.empty, on, runs, ev.gpe_failed,
                                ev.gpe_storms);
    }
    if (ev.ged_count)
        at += (size_t)ksnprintf(text + at, sizeof(text) - at,
                                "the GED (GSI %u%s): %u interrupts, %u runs, %u failed; ",
                                ev.ged[0].gsi, ev.ged_count > 1 ? ", ..." : "",
                                ev.ged_interrupts, ev.ged_runs, ev.ged_failed);
    spin_unlock(&ev.lock, state);
    char kind[160];
    ksnprintf(kind, sizeof(kind), "%s%s%s%s", ev.fixed_button ? "fixed" : "",
              ev.fixed_button && ev.button_devices[0] ? ", " : "", ev.button_devices,
              ev.fixed_button || ev.button_devices[0] ? "; keyboards' Power key"
                                                      : "keyboards' Power key");
    char around[48] = "";
    if (dropped)                                /* (37g-7) */
        ksnprintf(around, sizeof around, " (%u around a sleep, not acted on)", dropped);
    kprintf("events: %sthe power button (%s): %u presses%s, power_button = %s (%s); the lid: %u "
            "changes, lid = %s (%s); the AC adapter: %u changes; batteries: %u notifications\n",
            text, kind, presses, around, button_names[ev.button], ev.button_from,
            ev.lid_changes, lid_names[ev.lid], ev.lid_from, ev.ac_changes, ev.battery_notes);
}
