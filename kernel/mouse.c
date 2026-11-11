#include "blank.h"
#include "mouse.h"
#include "console.h"
#include "kprintf.h"
#include "string.h"
#include "winput.h"

#define MAX_MICE    8       /* names mouse0-mouse7 */

struct mouse {
    char name[8];                   /* "mouse0" */
    int number;
    bool present;                   /* plugged in and set up */
    const char *events_name;
    int x, y;                       /* the pointer, clamped to the screen */
    int max_x, max_y;               /* the screen's last pixel (0 if none) */
    uint8_t buttons;                /* MOUSE_* bits held now */
    volatile uint64_t events;       /* driver reports/packets */
    volatile uint64_t moves;        /* reports that moved the pointer */
    volatile uint64_t clicks;       /* button-down transitions */
    volatile int wheel;             /* net wheel steps */
    bool woke;                      /* (37e) a click woke the screen: dropped until released */
};

static struct mouse mice[MAX_MICE];     /* indexed by number */
static int mouse_count;                 /* numbers in use so far */

static int clamp(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

struct mouse *mouse_new(const char *events_name, const struct devname_id *id)
{
    bool again;
    int number = devname_attach("mouse", id, &again);
    if (number < 0 || number >= MAX_MICE) {
        if (number >= 0)
            devname_detach("mouse", number);
        return 0;
    }
    struct mouse *m = &mice[number];
    memset(m, 0, sizeof(*m));           /* not present while it is set up */
    ksnprintf(m->name, sizeof(m->name), "mouse%d", number);
    m->number = number;
    m->events_name = events_name;
    /* Start the pointer in the middle of the screen, if there is one. */
    uint32_t w, h;
    if (console_screen_size(&w, &h) && w && h) {
        m->max_x = (int)w - 1;
        m->max_y = (int)h - 1;
        m->x = m->max_x / 2;
        m->y = m->max_y / 2;
    }
    m->present = true;
    if (number >= mouse_count)
        mouse_count = number + 1;
    return m;
}

void mouse_ready(struct mouse *m)
{
    (void)m;    /* no consumer yet; windowd (15e) will take its events */
}

void mouse_drop(struct mouse *m)
{
    m->present = false;
    devname_detach("mouse", m->number);
}

void mouse_remove(struct mouse *m)
{
    m->present = false;
    m->buttons = 0;
    devname_detach("mouse", m->number);
}

const char *mouse_name(const struct mouse *m)
{
    return m->name;
}

/* The wheel, the click counting, and handing the pointer to the window server
 * (shared by the relative and absolute entry points). */
static void notify(struct mouse *m, int wheel, uint8_t buttons)
{
    if (wheel)
        m->wheel += wheel;
    uint8_t went_down = (uint8_t)(buttons & ~m->buttons);
    for (int b = 0; b < 8; b++)
        if (went_down & (1u << b))
            m->clicks++;
    m->buttons = buttons;
    /* (37e) While the screen is off, input only wakes it: dropped (the
     * pointer still moves), and a click's buttons until they are up. */
    if (blank_input()) {
        m->woke = buttons != 0;
        return;
    }
    if (m->woke) {
        m->woke = buttons != 0;
        return;
    }
    if (winput_captured()) {                    /* the window server wants it */
        winput_pointer(m->x, m->y, buttons);
        if (wheel)                              /* (drivers: positive up, as USB's) */
            winput_wheel(m->x, m->y, -wheel);
    }
}

void mouse_event(struct mouse *m, int dx, int dy, int wheel, uint8_t buttons)
{
    if (dx || dy) {
        /* HID y grows downward, like the screen. */
        m->x = clamp(m->x + dx, 0, m->max_x);
        m->y = clamp(m->y + dy, 0, m->max_y);
        m->moves++;
    }
    notify(m, wheel, buttons);
}

void mouse_absolute(struct mouse *m, int x, int y, int x_range, int y_range, int wheel,
                    uint8_t buttons)
{
    if (x_range > 0 && y_range > 0 && (m->max_x > 0 || m->max_y > 0)) {
        int nx = clamp((int)((int64_t)x * m->max_x / x_range), 0, m->max_x);
        int ny = clamp((int)((int64_t)y * m->max_y / y_range), 0, m->max_y);
        if (nx != m->x || ny != m->y) {
            m->x = nx;
            m->y = ny;
            m->moves++;
        }
    }
    notify(m, wheel, buttons);
}

void mouse_screen(uint32_t width, uint32_t height)
{
    if (!width || !height)
        return;
    for (int i = 0; i < mouse_count; i++) {
        struct mouse *m = &mice[i];
        /* (A move in an interrupt meanwhile clamps to the old or the new
         * size: either is put right by the next one.) */
        m->max_x = (int)width - 1;
        m->max_y = (int)height - 1;
        m->x = clamp(m->x, 0, m->max_x);
        m->y = clamp(m->y, 0, m->max_y);
    }
}

void mouse_count_event(struct mouse *m)
{
    m->events++;
}

static void buttons_text(uint8_t b, char *out, size_t size)
{
    if (!b) {
        ksnprintf(out, size, "none");
        return;
    }
    out[0] = 0;
    const char *sep = "";
    if (b & MOUSE_LEFT)
        ksnprintf(out, size, "left"), sep = " ";
    if (b & MOUSE_MIDDLE) {
        size_t n = strlen(out);
        ksnprintf(out + n, size - n, "%smiddle", sep), sep = " ";
    }
    if (b & MOUSE_RIGHT) {
        size_t n = strlen(out);
        ksnprintf(out + n, size - n, "%sright", sep);
    }
}

void mouse_report(void)
{
    for (int i = 0; i < mouse_count; i++) {
        struct mouse *m = &mice[i];
        if (!m->present)
            continue;
        char label[16], held[24];
        ksnprintf(label, sizeof(label), "%s:", m->name);
        buttons_text(m->buttons, held, sizeof(held));
        kprintf("  %-15s%lu %s, pointer at %d,%d, %lu move(s), %lu click(s), buttons: %s\n",
                label, m->events, m->events_name, m->x, m->y, m->moves, m->clicks, held);
    }
}
