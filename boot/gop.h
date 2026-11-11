/* UEFI Graphics Output Protocol: find it, inspect modes, set a resolution. */
#ifndef RITTEROS_GOP_H
#define RITTEROS_GOP_H

#include "efi.h"
#include "framebuffer.h"

EFI_STATUS gop_locate(EFI_BOOT_SERVICES *bs, EFI_GRAPHICS_OUTPUT_PROTOCOL **gop);

/* Print the current mode, framebuffer details and all available modes. */
void gop_print_info(EFI_BOOT_SERVICES *bs, EFI_GRAPHICS_OUTPUT_PROTOCOL *gop);

/* Find the mode number for width x height; returns 0 if found. */
EFI_STATUS gop_find_mode(EFI_BOOT_SERVICES *bs, EFI_GRAPHICS_OUTPUT_PROTOCOL *gop,
                         UINT32 width, UINT32 height, UINT32 *mode);

/* (65e-34) The EDID of the screen gop draws on, at most max bytes into out:
 * the bytes copied, 0 if the firmware has none. */
uint32_t gop_edid(EFI_BOOT_SERVICES *bs, EFI_GRAPHICS_OUTPUT_PROTOCOL *gop, uint8_t *out,
                  uint32_t max);

/* (65e-34) The modes gop can set (width, height), at most max: how many. */
uint32_t gop_modes(EFI_BOOT_SERVICES *bs, EFI_GRAPHICS_OUTPUT_PROTOCOL *gop, uint32_t *widths,
                   uint32_t *heights, uint32_t max);

/* Describe the current mode as a struct framebuffer; returns 0 on success. */
EFI_STATUS gop_get_framebuffer(EFI_GRAPHICS_OUTPUT_PROTOCOL *gop, struct framebuffer *fb);

#endif
