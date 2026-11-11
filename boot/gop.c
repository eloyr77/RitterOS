#include "gop.h"
#include "console.h"
#include "string.h"

EFI_STATUS gop_locate(EFI_BOOT_SERVICES *bs, EFI_GRAPHICS_OUTPUT_PROTOCOL **gop)
{
    static EFI_GUID guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    return bs->LocateProtocol(&guid, 0, (void **)gop);
}

static const CHAR16 *format_name(EFI_GRAPHICS_PIXEL_FORMAT format)
{
    switch (format) {
    case PixelRedGreenBlueReserved8BitPerColor: return u"RGBx, 8 bits per color";
    case PixelBlueGreenRedReserved8BitPerColor: return u"BGRx, 8 bits per color";
    case PixelBitMask:                          return u"custom bit masks";
    case PixelBltOnly:                          return u"no framebuffer (Blt only)";
    default:                                    return u"unknown";
    }
}

void gop_print_info(EFI_BOOT_SERVICES *bs, EFI_GRAPHICS_OUTPUT_PROTOCOL *gop)
{
    EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *mode = gop->Mode;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info = mode->Info;

    print(u"  Current mode:   ");
    print_dec(mode->Mode, 0);
    print(u" (");
    print_dec(info->HorizontalResolution, 0);
    print(u"x");
    print_dec(info->VerticalResolution, 0);
    print(u")\r\n  Pixel format:   ");
    print(format_name(info->PixelFormat));
    print(u"\r\n  Scanline:       ");
    print_dec(info->PixelsPerScanLine, 0);
    print(u" pixels\r\n  Framebuffer:    ");
    print_hex(mode->FrameBufferBase, 10);
    print(u", ");
    print_size(mode->FrameBufferSize, 0);
    print(u"\r\n  Available modes (");
    print_dec(mode->MaxMode, 0);
    print(u"):\r\n");

    for (UINT32 m = 0; m < mode->MaxMode; m++) {
        UINTN size;
        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *mi;
        if (EFI_ERROR(gop->QueryMode(gop, m, &size, &mi)))
            continue;
        if (m % 6 == 0)
            print(u"   ");
        print(m == mode->Mode ? u" *" : u"  ");
        print_dec(mi->HorizontalResolution, 4);
        print(u"x");
        print_dec(mi->VerticalResolution, 0);
        if (mi->VerticalResolution < 1000)
            print(u" ");
        print(u"  ");
        if (m % 6 == 5 || m + 1 == mode->MaxMode)
            print(u"\r\n");
        bs->FreePool(mi);   /* QueryMode allocates the info for us */
    }
}

EFI_STATUS gop_find_mode(EFI_BOOT_SERVICES *bs, EFI_GRAPHICS_OUTPUT_PROTOCOL *gop,
                         UINT32 width, UINT32 height, UINT32 *mode)
{
    for (UINT32 m = 0; m < gop->Mode->MaxMode; m++) {
        UINTN size;
        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *mi;
        if (EFI_ERROR(gop->QueryMode(gop, m, &size, &mi)))
            continue;
        BOOLEAN match = mi->HorizontalResolution == width &&
                        mi->VerticalResolution == height &&
                        mi->PixelFormat != PixelBltOnly;
        bs->FreePool(mi);
        if (match) {
            *mode = m;
            return EFI_SUCCESS;
        }
    }
    return EFI_NOT_FOUND;
}

static uint8_t mask_shift(UINT32 mask)
{
    return mask ? (uint8_t)__builtin_ctz(mask) : 0;
}

EFI_STATUS gop_get_framebuffer(EFI_GRAPHICS_OUTPUT_PROTOCOL *gop, struct framebuffer *fb)
{
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info = gop->Mode->Info;

    switch (info->PixelFormat) {
    case PixelRedGreenBlueReserved8BitPerColor:
        fb->red_shift = 0;
        fb->green_shift = 8;
        fb->blue_shift = 16;
        break;
    case PixelBlueGreenRedReserved8BitPerColor:
        fb->red_shift = 16;
        fb->green_shift = 8;
        fb->blue_shift = 0;
        break;
    case PixelBitMask:
        fb->red_shift = mask_shift(info->PixelInformation.RedMask);
        fb->green_shift = mask_shift(info->PixelInformation.GreenMask);
        fb->blue_shift = mask_shift(info->PixelInformation.BlueMask);
        break;
    default:
        return EFI_UNSUPPORTED;
    }

    fb->base = (volatile uint32_t *)gop->Mode->FrameBufferBase;
    fb->width = info->HorizontalResolution;
    fb->height = info->VerticalResolution;
    fb->stride = info->PixelsPerScanLine;
    return EFI_SUCCESS;
}

/* (65e-34) The EDID sits on the handle that holds the GOP: the active one
 * (what the output uses), else the discovered one. The GOP LocateProtocol
 * gives may be a console splitter's, with no EDID on its handle: then the
 * first handle with a GOP and an EDID. */
uint32_t gop_edid(EFI_BOOT_SERVICES *bs, EFI_GRAPHICS_OUTPUT_PROTOCOL *gop, uint8_t *out,
                  uint32_t max)
{
    static EFI_GUID gop_guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    static EFI_GUID active = EFI_EDID_ACTIVE_PROTOCOL_GUID;
    static EFI_GUID discovered = EFI_EDID_DISCOVERED_PROTOCOL_GUID;
    UINTN count = 0;
    EFI_HANDLE *handles = 0;
    if (EFI_ERROR(bs->LocateHandleBuffer(ByProtocol, &gop_guid, 0, &count, &handles)))
        return 0;
    uint32_t got = 0;
    for (int pass = 0; pass < 2 && !got; pass++) {
        for (UINTN i = 0; i < count && !got; i++) {
            void *g = 0;
            if (pass == 0 && (EFI_ERROR(bs->HandleProtocol(handles[i], &gop_guid, &g)) || g != gop))
                continue;                       /* (first only the handle of ours) */
            EFI_EDID_PROTOCOL *e = 0;
            if ((EFI_ERROR(bs->HandleProtocol(handles[i], &active, (void **)&e)) || !e ||
                 e->SizeOfEdid < 128 || !e->Edid) &&
                (EFI_ERROR(bs->HandleProtocol(handles[i], &discovered, (void **)&e)) || !e ||
                 e->SizeOfEdid < 128 || !e->Edid))
                continue;
            got = e->SizeOfEdid < max ? e->SizeOfEdid : max;
            memcpy(out, e->Edid, got);
        }
    }
    bs->FreePool(handles);
    return got;
}

uint32_t gop_modes(EFI_BOOT_SERVICES *bs, EFI_GRAPHICS_OUTPUT_PROTOCOL *gop, uint32_t *widths,
                   uint32_t *heights, uint32_t max)
{
    uint32_t n = 0;
    for (UINT32 m = 0; m < gop->Mode->MaxMode && n < max; m++) {
        UINTN size;
        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *mi;
        if (EFI_ERROR(gop->QueryMode(gop, m, &size, &mi)))
            continue;
        if (mi->PixelFormat != PixelBltOnly) {
            widths[n] = mi->HorizontalResolution;
            heights[n] = mi->VerticalResolution;
            n++;
        }
        bs->FreePool(mi);
    }
    return n;
}
