/*
 * The PS/2 keyboard and mouse, through the PC's 8042 keyboard controller
 * (i8042): data at I/O port 0x60, status and commands at 0x64. Laptops
 * still wire their built-in keyboard (port 1) and trackpad (port 2) this
 * way (or have the embedded controller imitate it). The keyboard speaks
 * scan code set 2; the controller translates that into the older set 1:
 * one byte per key press, the same plus 0x80 for the release, 0xE0 before
 * the newer keys (arrows, right Ctrl, ...). Set 1 codes become HID usage
 * IDs for keyboard.c. The mouse sends 3-byte packets (buttons, then signed
 * X and Y movement); those become mouse.c events.
 *
 * Set up after USB: firmware with "USB legacy support" imitates a PS/2
 * controller for USB keyboards until the USB controllers are taken over.
 * A PC may have no controller at all (its ports read 0xFF), or nothing on
 * a port: every wait here is bounded.
 */
#include "acpi.h"
#include "apic.h"
#include "arch.h"
#include "io.h"
#include "keyboard.h"
#include "kprintf.h"
#include "mouse.h"
#include "sleep.h"

#define DATA            0x60
#define STATUS          0x64    /* read */
#define COMMAND         0x64    /* write */
#define STATUS_OUTPUT   0x01    /* a byte waits at DATA */
#define STATUS_INPUT    0x02    /* the controller has not taken the last byte yet */
#define STATUS_AUX      0x20    /* the waiting byte is from port 2 (a mouse) */

#define CMD_READ_CONFIG     0x20
#define CMD_WRITE_CONFIG    0x60
#define CMD_DISABLE_PORT2   0xA7
#define CMD_ENABLE_PORT2    0xA8
#define CMD_TEST_PORT2      0xA9    /* answers 0x00 */
#define CMD_SELF_TEST       0xAA    /* answers 0x55 */
#define CMD_TEST_PORT1      0xAB    /* answers 0x00 */
#define CMD_DISABLE_PORT1   0xAD
#define CMD_ENABLE_PORT1    0xAE
#define CMD_WRITE_PORT2     0xD4    /* the next byte to DATA goes to port 2 */
#define CONFIG_IRQ1         0x01    /* interrupt for port 1 */
#define CONFIG_IRQ12        0x02    /* interrupt for port 2 */
#define CONFIG_NO_CLOCK1    0x10    /* port 1 switched off */
#define CONFIG_NO_CLOCK2    0x20    /* port 2 switched off */
#define CONFIG_TRANSLATE    0x40    /* set 2 -> set 1 */

#define KBD_RESET       0xFF        /* answers 0xFA (ack), then 0xAA (passed) */
#define KBD_ACK         0xFA
#define KBD_PASSED      0xAA
#define MOUSE_ENABLE    0xF4        /* enable data reporting (answers 0xFA) */
#define KEYBOARD_IRQ    1
#define MOUSE_IRQ       12

/* Set 1 scan codes 0x00-0x58 as HID usage IDs (0: none). */
static const uint8_t usage_of[0x59] = {
    0,    0x29, 0x1E, 0x1F, 0x20, 0x21, 0x22, 0x23,   /* 00: -, Esc, 1-6 */
    0x24, 0x25, 0x26, 0x27, 0x2D, 0x2E, 0x2A, 0x2B,   /* 08: 7-0, - =, Backspace, Tab */
    0x14, 0x1A, 0x08, 0x15, 0x17, 0x1C, 0x18, 0x0C,   /* 10: Q W E R T Y U I */
    0x12, 0x13, 0x2F, 0x30, 0x28, 0xE0, 0x04, 0x16,   /* 18: O P [ ] Enter, left Ctrl, A S */
    0x07, 0x09, 0x0A, 0x0B, 0x0D, 0x0E, 0x0F, 0x33,   /* 20: D F G H J K L ; */
    0x34, 0x35, 0xE1, 0x31, 0x1D, 0x1B, 0x06, 0x19,   /* 28: ' ` left Shift \ Z X C V */
    0x05, 0x11, 0x10, 0x36, 0x37, 0x38, 0xE5, 0x55,   /* 30: B N M , . / right Shift, keypad * */
    0xE2, 0x2C, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E,   /* 38: left Alt, Space, Caps Lock, F1-F5 */
    0x3F, 0x40, 0x41, 0x42, 0x43, 0x53, 0x47, 0x5F,   /* 40: F6-F10, Num Lock, Scroll Lock, kp 7 */
    0x60, 0x61, 0x56, 0x5C, 0x5D, 0x5E, 0x57, 0x59,   /* 48: kp 8 9 - 4 5 6 + 1 */
    0x5A, 0x5B, 0x62, 0x63, 0,    0x66, 0x64, 0x44,   /* 50: kp 2 3 0 ., -, Power (*), ISO \, F11 */
    0x45,                                             /* 58: F12 */
};
/* (*) (37g-7) 0x55 is no key on a standard keyboard; the Acer's embedded
 * controller sends its power button as 0x55 while the machine is awake
 * (the button's ACPI status comes only in a sleep). */

/* After 0xE0. */
static uint8_t usage_of_extended(uint8_t code)
{
    switch (code) {
    case 0x1C: return 0x58;     /* keypad Enter */
    case 0x1D: return 0xE4;     /* right Ctrl */
    case 0x35: return 0x54;     /* keypad / */
    case 0x38: return 0xE6;     /* right Alt */
    case 0x47: return 0x4A;     /* Home */
    case 0x48: return 0x52;     /* up */
    case 0x49: return 0x4B;     /* Page Up */
    case 0x4B: return 0x50;     /* left */
    case 0x4D: return 0x4F;     /* right */
    case 0x4F: return 0x4D;     /* End */
    case 0x50: return 0x51;     /* down */
    case 0x51: return 0x4E;     /* Page Down */
    case 0x52: return 0x49;     /* Insert */
    case 0x53: return 0x4C;     /* Delete */
    case 0x5B: return 0xE3;     /* left GUI (Windows key) */
    case 0x5C: return 0xE7;     /* right GUI */
    case 0x5D: return 0x65;     /* Menu */
    case 0x5E: return 0x66;     /* Power (37g-7) */
    default: return 0;          /* includes the fake Shifts around Print Screen */
    }
}

static struct keyboard *keyboard;
static bool extended;           /* the last byte was 0xE0 */
static int skip;                /* bytes left of a Pause sequence (E1 1D 45 E1 9D C5) */

static struct mouse *mouse;     /* 0 if there is none on port 2 */
static uint8_t packet[4];       /* a mouse packet being collected */
static int packet_index, packet_size = 3;   /* 4 with a wheel (IntelliMouse) */

static bool wait_input_empty(void)
{
    for (int ms = 0; ms < 100; ms++) {
        if (!(inb(STATUS) & STATUS_INPUT))
            return true;
        arch_delay_ms(1);
    }
    return false;
}

static bool command(uint8_t c)
{
    if (!wait_input_empty())
        return false;
    outb(COMMAND, c);
    return true;
}

static bool write_data(uint8_t b)
{
    if (!wait_input_empty())
        return false;
    outb(DATA, b);
    return true;
}

/* Send a byte to the mouse on port 2 (prefixed with 0xD4). */
static bool write_mouse(uint8_t b)
{
    return command(CMD_WRITE_PORT2) && write_data(b);
}

static int read_data(int ms);

/* A byte to the mouse, acknowledged. */
static bool mouse_cmd(uint8_t b)
{
    return write_mouse(b) && read_data(200) == KBD_ACK;
}

/* The next byte from the controller or keyboard, or -1 after ms. */
static int read_data(int ms)
{
    for (int i = 0; i <= ms; i++) {
        if (inb(STATUS) & STATUS_OUTPUT)
            return inb(DATA);
        arch_delay_ms(1);
    }
    return -1;
}

static void drain(void)
{
    for (int i = 0; i < 32 && (inb(STATUS) & STATUS_OUTPUT); i++)
        (void)inb(DATA);
}

static void scan_code(uint8_t b)
{
    if (skip > 0) {
        skip--;
        return;
    }
    if (b == 0xE0) {
        extended = true;
        return;
    }
    if (b == 0xE1) {
        skip = 5;
        return;
    }
    /* Errors and answers, not keys. Not 0xAA: as a passed self-test it only
     * comes right after the reset, but it is also the release of left
     * Shift (0x2A + 0x80). */
    if (b == 0x00 || b == 0xFF || b == KBD_ACK || b == 0xEE || b == 0xFE)
        return;
    uint8_t code = b & 0x7F;
    uint8_t usage = extended ? usage_of_extended(code)
                             : code < sizeof(usage_of) ? usage_of[code] : 0;
    extended = false;
    if (usage)
        keyboard_key(keyboard, usage, !(b & 0x80));
}

/* A byte from the mouse on port 2. A packet is three bytes: flags (bit 3
 * always 1, so it resynchronises; bits 0-2 the buttons, 4-5 the sign of X
 * and Y), then the signed X and Y movement; an IntelliMouse adds a fourth,
 * the signed wheel steps (positive towards the user). PS/2 Y grows upward,
 * the screen downward. Interrupt context. */
static void mouse_byte(uint8_t b)
{
    if (packet_index == 0 && !(b & 0x08))
        return;                         /* out of sync: wait for a first byte */
    packet[packet_index++] = b;
    if (packet_index < packet_size)
        return;
    packet_index = 0;
    uint8_t flags = packet[0];
    int dx = (flags & 0x10) ? (int)packet[1] - 256 : packet[1];
    int dy = (flags & 0x20) ? (int)packet[2] - 256 : packet[2];
    int wheel = packet_size == 4 ? -(int)(int8_t)packet[3] : 0;   /* (positive up, as USB) */
    mouse_event(mouse, dx, -dy, wheel, (uint8_t)(flags & 0x07));
}

/* Interrupt context: no console, no heap. */
static void ps2_interrupt(void *context)
{
    (void)context;
    for (int i = 0; i < 16; i++) {
        uint8_t status = inb(STATUS);
        if (!(status & STATUS_OUTPUT))
            break;
        uint8_t b = inb(DATA);
        if (status & STATUS_AUX) {
            if (mouse) {
                mouse_count_event(mouse);
                mouse_byte(b);
            }
            continue;
        }
        keyboard_count_event(keyboard);
        scan_code(b);
    }
}

static void failed(const char *why)
{
    kprintf("  PS/2:          %s\n", why);
}

/* Set up a mouse on port 2: enable the port, reset the mouse (it answers
 * 0xFA, then 0xAA for the self-test and a device id), enable data reporting,
 * and route IRQ 12. The caller then turns on port 2's clock and interrupt in
 * the configuration byte. Returns false (and leaves no mouse) when there is
 * no second port or no mouse answers. */
static bool ps2_mouse_init(void)
{
    if (!command(CMD_ENABLE_PORT2))
        return false;
    int result = command(CMD_TEST_PORT2) ? read_data(100) : -1;
    if (result != 0x00)
        return false;                       /* no usable second port */
    drain();
    if (!write_mouse(KBD_RESET) || read_data(200) != KBD_ACK)
        return false;                       /* no mouse on port 2 */
    if (read_data(1000) != KBD_PASSED) {
        failed("mouse self-test failed");
        return false;
    }
    read_data(100);                         /* device id (0x00 for a plain mouse) */
    /* Is it a touchpad that can say more than a mouse (15r)? Synaptics's
     * "identify" (four E8 00, then E9: the middle byte 0x47) and ELAN's knock
     * (three E6, then E9: 0x3c 0x03 ...); a plain mouse answers both with
     * its status bytes. Recorded for the line below; then back to defaults. */
    int syn[3], elan[3];
    bool asked = true;
    for (int i = 0; i < 4 && asked; i++)
        asked = mouse_cmd(0xE8) && mouse_cmd(0x00);
    asked = asked && mouse_cmd(0xE9);
    for (int i = 0; i < 3; i++)
        syn[i] = asked ? read_data(200) : -1;
    asked = mouse_cmd(0xE6) && mouse_cmd(0xE6) && mouse_cmd(0xE6) && mouse_cmd(0xE9);
    for (int i = 0; i < 3; i++)
        elan[i] = asked ? read_data(200) : -1;
    mouse_cmd(0xF6);                        /* set defaults */
    /* The wheel (15o): sample rates 200, 100, 80 in a row ask an IntelliMouse
     * to report it; it then says it is one (id 3) and sends four-byte
     * packets. A plain mouse ignores the knock and stays at three. */
    static const uint8_t knock[] = { 200, 100, 80 };
    bool ok = true;
    for (int i = 0; i < 3 && ok; i++)
        ok = write_mouse(0xF3) && read_data(200) == KBD_ACK &&
             write_mouse(knock[i]) && read_data(200) == KBD_ACK;
    if (ok && write_mouse(0xF2) && read_data(200) == KBD_ACK && read_data(200) == 3)
        packet_size = 4;
    if (!write_mouse(MOUSE_ENABLE) || read_data(200) != KBD_ACK) {
        failed("the mouse does not take 'enable reporting'");
        return false;
    }
    static const struct devname_id aux = { 0, 0, "", "i8042-aux" };
    mouse = mouse_new("packets", &aux);
    if (!mouse)
        return false;
    uint32_t gsi;
    unsigned vector;
    if (!apic_route_isa_irq(MOUSE_IRQ, ps2_interrupt, 0, &gsi, &vector)) {
        mouse_drop(mouse);
        mouse = 0;
        return false;
    }
    char label[16];
    ksnprintf(label, sizeof(label), "%s:", mouse_name(mouse));
    kprintf("  %-15sPS/2 mouse on the i8042 (port 2)%s, IRQ 12 -> GSI %u (IO APIC), "
            "vector 0x%02x\n", label, packet_size == 4 ? " with a wheel" : "", gsi, vector);
    const char *kind = syn[1] == 0x47 ? "a Synaptics touchpad"
                     : elan[0] == 0x3c && elan[1] == 0x03 ? "an ELAN touchpad" : "no touchpad answer";
    kprintf("  %-15sidentify: Synaptics %02x %02x %02x, ELAN %02x %02x %02x: %s\n", "",
            syn[0] & 0xFF, syn[1] & 0xFF, syn[2] & 0xFF, elan[0] & 0xFF, elan[1] & 0xFF,
            elan[2] & 0xFF, kind);
    acpi_report_input_devices();
    mouse_ready(mouse);
    return true;
}

/* (37g-2) Sleep: in S3 the controller and the keyboard (and mouse) lose
 * their state. Before, interrupts off in the configuration; after, as at
 * the boot: the controller's self-test and configuration, the keyboard's
 * reset, the mouse's commands again (the same mouse), then the last
 * configuration byte (interrupts on). */
static uint8_t final_config;

static bool ps2_suspend(void *ctx)
{
    (void)ctx;
    if (command(CMD_WRITE_CONFIG))
        write_data((uint8_t)(final_config & ~(CONFIG_IRQ1 | CONFIG_IRQ12)));
    return true;
}

static bool mouse_restart(void)
{
    if (!command(CMD_ENABLE_PORT2) || !write_mouse(KBD_RESET) || read_data(200) != KBD_ACK ||
        read_data(1000) != KBD_PASSED)
        return false;
    read_data(100);                         /* device id */
    mouse_cmd(0xF6);                        /* set defaults */
    static const uint8_t knock[] = { 200, 100, 80 };
    bool ok = packet_size == 4;
    for (int i = 0; i < 3 && ok; i++)
        ok = write_mouse(0xF3) && read_data(200) == KBD_ACK &&
             write_mouse(knock[i]) && read_data(200) == KBD_ACK;
    if (ok && write_mouse(0xF2) && read_data(200) == KBD_ACK)
        read_data(200);                     /* (3: the wheel again) */
    packet_index = 0;
    return write_mouse(MOUSE_ENABLE) && read_data(200) == KBD_ACK;
}

static bool ps2_resume(void *ctx)
{
    (void)ctx;
    command(CMD_DISABLE_PORT1);
    command(CMD_DISABLE_PORT2);
    drain();
    uint8_t quiet = (uint8_t)(final_config & ~(CONFIG_IRQ1 | CONFIG_IRQ12));
    bool ok = command(CMD_SELF_TEST) && read_data(1000) == 0x55 &&
              command(CMD_WRITE_CONFIG) && write_data(quiet) && command(CMD_ENABLE_PORT1);
    drain();
    ok = ok && write_data(KBD_RESET) && read_data(200) == KBD_ACK && read_data(1000) == KBD_PASSED;
    skip = 0;
    if (ok && mouse && !mouse_restart())
        kprintf("  PS/2:          the mouse did not come back after the sleep\n");
    drain();
    command(CMD_WRITE_CONFIG);
    write_data(final_config);
    if (!ok)
        kprintf("  PS/2:          the keyboard did not come back after the sleep\n");
    return ok;
}

void arch_input_init(void)
{
    if (inb(STATUS) == 0xFF) {
        failed("no keyboard controller (i8042)");
        return;
    }
    /* Both ports off while the controller is set up; then its configuration
     * without interrupts, with translation to set 1. */
    if (!command(CMD_DISABLE_PORT1) || !command(CMD_DISABLE_PORT2)) {
        failed("the keyboard controller does not take commands");
        return;
    }
    drain();
    int config = command(CMD_READ_CONFIG) ? read_data(100) : -1;
    if (config < 0) {
        failed("the keyboard controller does not answer");
        return;
    }
    config = (config & ~(CONFIG_IRQ1 | CONFIG_IRQ12)) | CONFIG_TRANSLATE;
    int result = command(CMD_SELF_TEST) ? read_data(1000) : -1;
    if (result != 0x55) {
        kprintf("  PS/2:          keyboard controller self-test failed (answer 0x%x)\n", result);
        return;
    }
    /* Some controllers reset their configuration in the self-test. */
    if (!command(CMD_WRITE_CONFIG) || !write_data((uint8_t)(config & ~CONFIG_NO_CLOCK1))) {
        failed("the keyboard controller does not take its configuration");
        return;
    }
    result = command(CMD_TEST_PORT1) ? read_data(100) : -1;
    if (result != 0x00) {
        kprintf("  PS/2:          port 1 test failed (answer 0x%x)\n", result);
        return;
    }
    command(CMD_ENABLE_PORT1);

    /* Reset the keyboard: it acknowledges, tests itself (up to ~500 ms) and
     * reports the result. Without a keyboard, nothing comes. */
    drain();
    if (!write_data(KBD_RESET) || read_data(200) != KBD_ACK) {
        failed("controller found, no keyboard on port 1");
        return;
    }
    result = read_data(1000);
    if (result != KBD_PASSED) {
        kprintf("  PS/2:          keyboard self-test failed (answer 0x%x)\n", result);
        return;
    }

    static const struct devname_id built_in = { 0, 0, "", "i8042" };
    keyboard = keyboard_new("scan codes", &built_in);
    if (!keyboard)
        return;
    uint32_t gsi;
    unsigned vector;
    if (!apic_route_isa_irq(KEYBOARD_IRQ, ps2_interrupt, 0, &gsi, &vector)) {
        keyboard_drop(keyboard);
        failed("IRQ 1 cannot be routed");
        return;
    }
    char label[16];
    ksnprintf(label, sizeof(label), "%s:", keyboard_name(keyboard));
    kprintf("  %-15sPS/2 keyboard on the i8042 (I/O 0x60/0x64), IRQ 1 -> GSI %u (IO APIC), "
            "vector 0x%02x, scan code set 1, US layout\n", label, gsi, vector);

    /* A mouse on port 2, if the controller has one and a mouse answers. Its
     * interrupt (IRQ 12) and port 2's clock go into the configuration byte,
     * written last so no interrupt fires until the handlers are in place. */
    uint8_t cfg = (uint8_t)((config & ~CONFIG_NO_CLOCK1) | CONFIG_IRQ1);
    if (ps2_mouse_init())
        cfg = (uint8_t)((cfg & ~CONFIG_NO_CLOCK2) | CONFIG_IRQ12);
    drain();
    command(CMD_WRITE_CONFIG);
    write_data(cfg);
    final_config = cfg;
    sleep_register("i8042", ps2_suspend, ps2_resume, 0);  /* (37g-2) */
    keyboard_ready(keyboard);
}
