#include "acpi_events.h"
#include "blank.h"
#include "keyboard.h"
#include "kprintf.h"
#include "process.h"
#include "string.h"
#include "sync.h"
#include "terminal.h"
#include "timer.h"
#include "winput.h"

/* A byte the layout produced goes to the window server while it owns the
 * screen (SYS_DISPLAY), otherwise to the terminal. */
static void deliver(uint8_t byte)
{
    if (winput_captured())
        winput_key(byte);
    else
        terminal_receive(byte);
}

#define MAX_KEYBOARDS       8     /* names kbd0-kbd7 */

/* (65a-4) A keyboard's state (held keys, modifiers, the key repeating):
 * its driver changes it and the timer tick repeats keys, which on several
 * cores may run at once (a USB event taken by a waiting thread). Until
 * 65a-4 both ran with interrupts off on the one core. */
static struct spinlock keyboards_lock = SPINLOCK_INIT("keyboards");

#define KEY_A               0x04
#define KEY_Z               0x1D
#define KEY_1               0x1E
#define KEY_0               0x27
#define KEY_CAPS_LOCK       0x39
#define KEY_DELETE          0x4C
#define KEY_POWER           0x66
#define KEY_FIRST_MODIFIER  0xE0
#define KEY_LAST_MODIFIER   0xE7

/* Held keys repeat after 500 ms, about 33 times a second. */
#define REPEAT_DELAY        (TIMER_HZ / 2)
#define REPEAT_EVERY        3

struct keyboard {
    char name[8];                   /* "kbd0" */
    int number;
    bool present;                   /* plugged in and set up */
    const char *events_name;
    uint8_t modifiers;
    bool caps_lock;
    uint8_t down[32];               /* one bit per usage ID */
    uint8_t repeat_key;
    uint64_t repeat_at;             /* tick of the next repetition */
    char dead;                      /* a dead key waiting for its letter, or 0 */
    volatile uint64_t events, presses;
    uint8_t woke;                   /* (37e) the key that woke the screen: its release dropped */
};

static struct keyboard keyboards[MAX_KEYBOARDS];     /* indexed by number */
static int keyboard_count;          /* numbers in use so far */
static bool ticking;

/* US layout: usage IDs 0x2D-0x38 (punctuation), without and with Shift. */
static const char punctuation[] = "-=[]\\#;'`,./";
static const char punctuation_shift[] = "_+{}|~:\"~<>?";
static const char digits_shift[] = "!@#$%^&*()";

static void send(const char *s)
{
    while (*s)
        deliver((uint8_t)*s++);
}

/* A character (Latin-1 or a few beyond) as UTF-8. */
static void send_char(uint32_t c)
{
    char out[4] = { 0 };
    if (c < 0x80) {
        out[0] = (char)c;
    } else if (c < 0x800) {
        out[0] = (char)(0xC0 | c >> 6);
        out[1] = (char)(0x80 | (c & 0x3F));
    } else {
        out[0] = (char)(0xE0 | c >> 12);
        out[1] = (char)(0x80 | (c >> 6 & 0x3F));
        out[2] = (char)(0x80 | (c & 0x3F));
    }
    send(out);
}

/*
 * US International: the accent keys are dead keys. ' ` " ^ ~ wait for the
 * next key: a letter that takes the accent becomes one character (' e ->
 * é, " u -> ü, ~ n -> ñ, ' c -> ç); a space gives the mark itself; any
 * other key gives the mark and then that key. AltGr (right Alt) with some
 * keys gives more (AltGr n -> ñ, AltGr ? -> ¿, AltGr 5 -> €), as on
 * Windows' "US International".
 */
static const struct {
    char dead, letter;
    uint16_t result;
} composed[] = {
    { '\'', 'a', 0xE1 }, { '\'', 'e', 0xE9 }, { '\'', 'i', 0xED }, { '\'', 'o', 0xF3 },
    { '\'', 'u', 0xFA }, { '\'', 'y', 0xFD }, { '\'', 'c', 0xE7 }, { '\'', 'A', 0xC1 },
    { '\'', 'E', 0xC9 }, { '\'', 'I', 0xCD }, { '\'', 'O', 0xD3 }, { '\'', 'U', 0xDA },
    { '\'', 'Y', 0xDD }, { '\'', 'C', 0xC7 },
    { '`', 'a', 0xE0 }, { '`', 'e', 0xE8 }, { '`', 'i', 0xEC }, { '`', 'o', 0xF2 },
    { '`', 'u', 0xF9 }, { '`', 'A', 0xC0 }, { '`', 'E', 0xC8 }, { '`', 'I', 0xCC },
    { '`', 'O', 0xD2 }, { '`', 'U', 0xD9 },
    { '"', 'a', 0xE4 }, { '"', 'e', 0xEB }, { '"', 'i', 0xEF }, { '"', 'o', 0xF6 },
    { '"', 'u', 0xFC }, { '"', 'y', 0xFF }, { '"', 'A', 0xC4 }, { '"', 'E', 0xCB },
    { '"', 'I', 0xCF }, { '"', 'O', 0xD6 }, { '"', 'U', 0xDC },
    { '^', 'a', 0xE2 }, { '^', 'e', 0xEA }, { '^', 'i', 0xEE }, { '^', 'o', 0xF4 },
    { '^', 'u', 0xFB }, { '^', 'A', 0xC2 }, { '^', 'E', 0xCA }, { '^', 'I', 0xCE },
    { '^', 'O', 0xD4 }, { '^', 'U', 0xDB },
    { '~', 'a', 0xE3 }, { '~', 'o', 0xF5 }, { '~', 'n', 0xF1 }, { '~', 'A', 0xC3 },
    { '~', 'O', 0xD5 }, { '~', 'N', 0xD1 },
};

/* AltGr: the key's character (as without AltGr) to what it gives, without
 * and with Shift. */
static const struct {
    char key;
    uint16_t plain, shifted;
} altgr_keys[] = {
    { 'q', 0xE4, 0xC4 }, { 'w', 0xE5, 0xC5 }, { 'e', 0xE9, 0xC9 }, { 'y', 0xFC, 0xDC },
    { 'u', 0xFA, 0xDA }, { 'i', 0xED, 0xCD }, { 'o', 0xF3, 0xD3 }, { 'p', 0xF6, 0xD6 },
    { 'a', 0xE1, 0xC1 }, { 's', 0xDF, 0xA7 }, { 'l', 0xF8, 0xD8 }, { 'z', 0xE6, 0xC6 },
    { 'c', 0xA9, 0xA2 }, { 'n', 0xF1, 0xD1 }, { ',', 0xE7, 0xC7 }, { '1', 0xA1, 0xB9 },
    { '2', 0xB2, 0 }, { '3', 0xB3, 0 }, { '4', 0xA4, 0xA3 }, { '5', 0x20AC, 0 },
    { '/', 0xBF, 0 }, { ';', 0xB6, 0xB0 }, { '-', 0xA5, 0 }, { '=', 0xD7, 0xF7 },
};

static bool is_dead(char c)
{
    return c == '\'' || c == '`' || c == '"' || c == '^' || c == '~';
}

/* A printable character from the layout: through the dead keys. */
static void typed(struct keyboard *k, char c, bool altgr, bool shift)
{
    if (altgr) {
        char base = c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
        if (shift)                      /* the unshifted key: AltGr Shift 1 is ¹, not ! */
            for (size_t i = 0; i < 10; i++)
                if (c == "!@#$%^&*()"[i])
                    base = "1234567890"[i];
        if (shift && c == '?')
            base = '/';
        for (size_t i = 0; i < sizeof(altgr_keys) / sizeof(altgr_keys[0]); i++) {
            if (altgr_keys[i].key == base) {
                uint16_t r = shift && altgr_keys[i].shifted ? altgr_keys[i].shifted
                                                            : altgr_keys[i].plain;
                k->dead = 0;
                send_char(r);
                return;
            }
        }
    }
    if (k->dead) {
        char dead = k->dead;
        k->dead = 0;
        for (size_t i = 0; i < sizeof(composed) / sizeof(composed[0]); i++) {
            if (composed[i].dead == dead && composed[i].letter == c) {
                send_char(composed[i].result);
                return;
            }
        }
        deliver((uint8_t)dead);
        if (c == ' ')
            return;                     /* the mark alone */
    } else if (is_dead(c)) {
        k->dead = c;
        return;
    }
    deliver((uint8_t)c);
}

/* What a key does, with the modifiers held. Returns false for keys that
 * produce nothing (and so do not repeat). Interrupt context. */
static bool press(struct keyboard *k, uint8_t key)
{
    bool shift = k->modifiers & KEYBOARD_SHIFT, ctrl = k->modifiers & KEYBOARD_CTRL;
    bool altgr = k->modifiers & KEYBOARD_ALTGR;
    char c = 0;
    /* Ctrl-Alt-Delete while a program owns the screen: leave it and bring the
     * text console back (the escape works even if the window server hangs). */
    if (key == KEY_DELETE && winput_captured() &&
        (k->modifiers & KEYBOARD_CTRL) && (k->modifiers & KEYBOARD_ALT)) {
        display_escape();
        return true;
    }
    if (key >= KEY_A && key <= KEY_Z) {
        c = (char)('a' + key - KEY_A);
        if (ctrl)
            c &= 0x1F;                              /* Ctrl-C = 3 */
        else if (shift != k->caps_lock)
            c = (char)(c - 'a' + 'A');
    } else if (key >= KEY_1 && key <= KEY_0) {
        c = shift ? digits_shift[key - KEY_1] : "1234567890"[key - KEY_1];
    } else if (key >= 0x2D && key <= 0x38) {
        c = (shift ? punctuation_shift : punctuation)[key - 0x2D];
    } else if (key >= 0x59 && key <= 0x62) {        /* keypad digits */
        c = "1234567890"[key - 0x59];
    } else {
        switch (key) {
        case 0x28: case 0x58: c = '\r'; break;      /* Enter, keypad Enter */
        case 0x29: c = 0x1B; break;                 /* Escape */
        case 0x2A: c = 0x7F; break;                 /* Backspace: DEL, as terminals send */
        case 0x2B: c = '\t'; break;
        case 0x2C: c = ' '; break;
        case 0x54: c = '/'; break;                  /* keypad */
        case 0x55: c = '*'; break;
        case 0x56: c = '-'; break;
        case 0x57: c = '+'; break;
        case 0x63: c = '.'; break;
        case 0x64: c = shift ? '|' : '\\'; break;   /* the extra key of ISO keyboards */
        case KEY_CAPS_LOCK:
            k->caps_lock = !k->caps_lock;
            return false;
        /* Cursor keys as the escape sequences a VT100 terminal sends. */
        case 0x4F: send("\033[C"); return true;     /* right */
        case 0x50: send("\033[D"); return true;     /* left */
        case 0x51: send("\033[B"); return true;     /* down */
        case 0x52: send("\033[A"); return true;     /* up */
        case 0x4A: send("\033[H"); return true;     /* Home */
        case 0x4D: send("\033[F"); return true;     /* End */
        case 0x49: send("\033[2~"); return true;    /* Insert */
        case 0x4C: send("\033[3~"); return true;    /* Delete */
        case 0x4B: send("\033[5~"); return true;    /* Page Up */
        case 0x4E: send("\033[6~"); return true;    /* Page Down */
        default: return false;
        }
    }
    if (c >= 0x20 && c < 0x7F && !ctrl) {
        typed(k, c, altgr, shift);
        return true;
    }
    /* Enter, Backspace, Tab, Escape, Ctrl-x: a waiting dead key is shown
     * first (Backspace and Escape drop it). */
    if (k->dead && c != 0x7F && c != 0x1B)
        deliver((uint8_t)k->dead);
    k->dead = 0;
    deliver((uint8_t)c);
    return true;
}

/* Repeat held keys. Interrupt context (the timer tick). */
static void tick(void *context)
{
    (void)context;
    uint64_t state = spin_lock(&keyboards_lock);
    for (int i = 0; i < keyboard_count; i++) {
        struct keyboard *k = &keyboards[i];
        if (k->present && k->repeat_key && timer_ticks() >= k->repeat_at) {
            press(k, k->repeat_key);
            k->repeat_at += REPEAT_EVERY;
        }
    }
    spin_unlock(&keyboards_lock, state);
}

struct keyboard *keyboard_new(const char *events_name, const struct devname_id *id)
{
    bool again;
    int number = devname_attach("kbd", id, &again);
    if (number < 0 || number >= MAX_KEYBOARDS) {
        if (number >= 0)
            devname_detach("kbd", number);
        return 0;
    }
    if (!ticking)
        ticking = timer_add_tick_handler(tick, 0);
    struct keyboard *k = &keyboards[number];
    memset(k, 0, sizeof(*k));               /* not present while it is reset */
    ksnprintf(k->name, sizeof(k->name), "kbd%d", number);
    k->number = number;
    k->events_name = events_name;
    k->present = true;
    if (number >= keyboard_count)
        keyboard_count = number + 1;
    return k;
}

void keyboard_ready(struct keyboard *k)
{
    terminal_add_input(k->name);
}

void keyboard_drop(struct keyboard *k)
{
    k->present = false;
    devname_detach("kbd", k->number);
}

/* Unplugged: no more keys, nothing held down, no repeat. The keyboard's
 * interrupt endpoint is gone already, so no key event races with this. */
void keyboard_remove(struct keyboard *k)
{
    uint64_t state = spin_lock(&keyboards_lock);
    k->present = false;
    k->repeat_key = 0;
    k->modifiers = 0;
    memset(k->down, 0, sizeof(k->down));
    spin_unlock(&keyboards_lock, state);
    terminal_remove_input(k->name);
    devname_detach("kbd", k->number);
}

const char *keyboard_name(const struct keyboard *k)
{
    return k->name;
}

static void key_locked(struct keyboard *k, uint8_t usage, bool down)
{
    /* (37e) A press while the screen is off only wakes it: dropped, and
     * its release too. */
    if (down && blank_input()) {
        k->woke = usage;
        return;
    }
    if (!down && usage == k->woke && usage) {
        k->woke = 0;
        return;
    }
    if (usage >= KEY_FIRST_MODIFIER && usage <= KEY_LAST_MODIFIER) {
        uint8_t bit = (uint8_t)(1u << (usage - KEY_FIRST_MODIFIER));
        bool was = k->modifiers & bit;
        k->modifiers = down ? k->modifiers | bit : k->modifiers & ~bit;
        if (was != down && winput_captured())
            winput_keycode(usage, down);            /* (a modifier alone too: 15p) */
        return;
    }
    uint8_t mask = (uint8_t)(1u << (usage % 8));
    if (!down) {
        if ((k->down[usage / 8] & mask) && winput_captured())
            winput_keycode(usage, false);           /* the release (15p) */
        k->down[usage / 8] &= ~mask;
        if (k->repeat_key == usage)
            k->repeat_key = 0;
        return;
    }
    if (k->down[usage / 8] & mask)
        return;                                     /* the keyboard's own repeat */
    k->down[usage / 8] |= mask;
    if (winput_captured())
        winput_keycode(usage, true);                /* the press, once (15p) */
    k->presses++;
    if (press(k, usage)) {
        k->repeat_key = usage;
        k->repeat_at = timer_ticks() + REPEAT_DELAY;
    }
}

void keyboard_key(struct keyboard *k, uint8_t usage, bool down)
{
    uint64_t state = spin_lock(&keyboards_lock);
    if (usage == KEY_POWER) {
        /* (37g-7) A press of the power button, as Linux's KEY_POWER: never
         * a key for programs, nor a wake-up of the screen; counted once
         * (a PS/2 keyboard repeats it while held). */
        uint8_t mask = (uint8_t)(1u << (usage % 8));
        bool pressed = down && !(k->down[usage / 8] & mask);
        k->down[usage / 8] = down ? k->down[usage / 8] | mask : k->down[usage / 8] & ~mask;
        spin_unlock(&keyboards_lock, state);
        if (pressed)
            acpi_events_button();
        return;
    }
    key_locked(k, usage, down);
    spin_unlock(&keyboards_lock, state);
}

uint8_t keyboard_modifiers_held(void)
{
    uint8_t held = 0;
    for (int i = 0; i < MAX_KEYBOARDS; i++)
        if (keyboards[i].present)
            held |= keyboards[i].modifiers;
    return held;
}

void keyboard_set_modifiers(struct keyboard *k, uint8_t modifiers)
{
    if (modifiers & ~k->modifiers)
        blank_activity();                           /* (37e) a modifier alone wakes, kept */
    uint64_t state = spin_lock(&keyboards_lock);
    uint8_t changed = (uint8_t)(k->modifiers ^ modifiers);
    k->modifiers = modifiers;
    for (int b = 0; b < 8 && changed && winput_captured(); b++)
        if (changed & (1u << b))                    /* bit b is usage 0xE0 + b (15p) */
            winput_keycode((uint8_t)(KEY_FIRST_MODIFIER + b), modifiers & (1u << b));
    spin_unlock(&keyboards_lock, state);
}

void keyboard_count_event(struct keyboard *k)
{
    k->events++;
}

void keyboard_report(void)
{
    for (int i = 0; i < keyboard_count; i++) {
        struct keyboard *k = &keyboards[i];
        if (!k->present)
            continue;
        char label[16];
        ksnprintf(label, sizeof(label), "%s:", k->name);
        kprintf("  %-15s%lu %s, %lu key presses\n", label, k->events, k->events_name,
                k->presses);
    }
}
