/* Test: the firmware's boot entries through SYS_EFIVAR (48c), as osinstall
 * writes them.
 *   efivar     BootCurrent (the entry the firmware started this time),
 *              BootOrder, and each entry of BootOrder: active or not, its
 *              description and its device path (hard drive and file path
 *              nodes decoded, others as their type and subtype)
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include "ritteros.h"

static struct sys_efivar var;

static int get(const char *name)
{
    static const uint8_t global[16] = { 0x61, 0xDF, 0xE4, 0x8B, 0xCA, 0x93, 0xD2, 0x11,
                                        0xAA, 0x0D, 0x00, 0xE0, 0x98, 0x03, 0x2B, 0x8C };
    memset(&var, 0, sizeof(var));
    snprintf(var.name, sizeof(var.name), "%s", name);
    memcpy(var.guid, global, 16);
    return ritteros_efivar(EFIVAR_GET, &var);
}

static uint64_t le(const uint8_t *p, int bytes)
{
    uint64_t v = 0;
    for (int i = bytes - 1; i >= 0; i--)
        v = v << 8 | p[i];
    return v;
}

/* UTF-16 (ASCII only) at p up to NUL or end: printed; its bytes with the NUL. */
static size_t wide(const uint8_t *p, size_t room)
{
    size_t i = 0;
    for (; i + 1 < room && (p[i] || p[i + 1]); i += 2)
        putchar(p[i + 1] ? '?' : p[i]);
    return i + 2;
}

static void device_path(const uint8_t *p, size_t length)
{
    for (size_t at = 0; at + 4 <= length;) {
        uint8_t type = p[at], sub = p[at + 1];
        size_t n = (size_t)le(p + at + 2, 2);
        if (n < 4 || at + n > length || (type == 0x7F && sub == 0xFF))
            break;
        if (at)
            putchar('/');
        const uint8_t *q = p + at;
        if (type == 4 && sub == 1 && n >= 42) {
            const uint8_t *g = q + 24;
            printf("HD(%lu,%s,%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X,"
                   "0x%lx,0x%lx)", (unsigned long)le(q + 4, 4), q[40] == 2 ? "GPT" : "MBR",
                   g[3], g[2], g[1], g[0], g[5], g[4], g[7], g[6], g[8], g[9], g[10], g[11],
                   g[12], g[13], g[14], g[15], (unsigned long)le(q + 8, 8),
                   (unsigned long)le(q + 16, 8));
        } else if (type == 4 && sub == 4) {
            wide(q + 4, n - 4);
        } else {
            printf("?(%u,%u)", type, sub);
        }
        at += n;
    }
}

int main(void)
{
    if (get("BootCurrent") == 0 && var.size >= 2)
        printf("BootCurrent: %04X\n", (unsigned)le(var.data, 2));
    else
        printf("BootCurrent: %s\n", strerror(errno));
    if (get("BootOrder") < 0) {
        printf("BootOrder: %s\n", strerror(errno));
        return 1;
    }
    static uint16_t order[EFIVAR_DATA_MAX / 2];
    int count = (int)(var.size / 2);
    memcpy(order, var.data, (size_t)count * 2);
    printf("BootOrder:");
    for (int i = 0; i < count; i++)
        printf(" %04X", order[i]);
    printf("\n");
    for (int i = 0; i < count; i++) {
        char name[16];
        snprintf(name, sizeof(name), "Boot%04X", order[i]);
        if (get(name) < 0) {
            printf("%s: %s\n", name, strerror(errno));
            continue;
        }
        size_t path_length = (size_t)le(var.data + 4, 2);
        printf("%s: %s, '", name, le(var.data, 4) & 1 ? "active" : "inactive");
        size_t at = 6 + wide(var.data + 6, var.size - 6);
        printf("', ");
        if (at + path_length <= var.size)
            device_path(var.data + at, path_length);
        printf("\n");
    }
    return 0;
}
