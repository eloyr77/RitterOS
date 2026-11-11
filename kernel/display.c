#include "display.h"
#include "edid.h"
#include "kprintf.h"
#include "string.h"

static uint8_t edid[BOOT_EDID_MAX];
static uint32_t edid_size;
static struct edid_info parsed;
static struct boot_mode modes[BOOT_MODES_MAX];
static uint32_t mode_count;

void display_init(const struct boot_info *info)
{
    edid_size = info->edid_size < BOOT_EDID_MAX ? info->edid_size : BOOT_EDID_MAX;
    memcpy(edid, info->edid, edid_size);
    mode_count = info->mode_count < BOOT_MODES_MAX ? info->mode_count : BOOT_MODES_MAX;
    memcpy(modes, info->modes, mode_count * sizeof(modes[0]));
    const struct framebuffer *fb = &info->framebuffer;
    if (!edid_size) {
        kprintf("  display:       no EDID from the firmware (%s); %u modes it can set\n",
                info->version < 6 ? "a bootloader from before 65e-34" : "none on the screen's handle",
                mode_count);
        return;
    }
    if (!edid_parse(edid, edid_size, &parsed)) {
        kprintf("  display:       an EDID of %u bytes that is not valid (header or checksum); "
                "%u modes\n", edid_size, mode_count);
        return;
    }
    uint32_t now = edid_refresh(&parsed, fb->width, fb->height);
    kprintf("  display:       EDID '%s', %u block(s); native %ux%u at %u.%03u Hz; the screen "
            "%ux%u at %s%u.%03u Hz; %d modes listed, %u the firmware can set\n",
            parsed.name[0] ? parsed.name : "(no name)", parsed.blocks, parsed.native.width,
            parsed.native.height, parsed.native.refresh_mhz / 1000, parsed.native.refresh_mhz % 1000,
            fb->width, fb->height, now ? "" : "(not listed) ", now / 1000, now % 1000,
            parsed.count, mode_count);
}

void display_modes(struct sys_display_modes *out, uint32_t width, uint32_t height)
{
    memset(out, 0, sizeof(*out));
    out->now = (struct sys_display_mode){ width, height, edid_refresh(&parsed, width, height) };
    if (parsed.valid) {
        out->native = (struct sys_display_mode){ parsed.native.width, parsed.native.height,
                                                 parsed.native.refresh_mhz };
        memcpy(out->name, parsed.name, sizeof(out->name));
    }
    out->edid_size = (uint16_t)edid_size;
    memcpy(out->edid, edid, edid_size);
    out->mode_count = mode_count < DISPLAY_MODES_MAX ? mode_count : DISPLAY_MODES_MAX;
    for (uint32_t i = 0; i < out->mode_count; i++)
        out->modes[i] = (struct sys_display_mode){
            modes[i].width, modes[i].height, edid_refresh(&parsed, modes[i].width, modes[i].height) };
}
