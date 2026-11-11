#include "console.h"

static EFI_SYSTEM_TABLE *sys;
static EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *out;
static UINTN screen_rows = 25;
static UINTN lines_shown;      /* lines printed since clear or last pause, capped at a screenful */
static BOOLEAN pager_on;

/* Switch to the text mode with the most rows, then clear the screen. */
void console_init(EFI_SYSTEM_TABLE *st)
{
    sys = st;
    out = st->ConOut;

    UINTN best_mode = (UINTN)out->Mode->Mode;
    UINTN best_rows = 0;
    for (UINTN mode = 0; mode < (UINTN)out->Mode->MaxMode; mode++) {
        UINTN cols, rows;
        if (EFI_ERROR(out->QueryMode(out, mode, &cols, &rows)))
            continue;
        if (rows > best_rows) {
            best_rows = rows;
            best_mode = mode;
        }
    }
    out->SetMode(out, best_mode);
    if (best_rows > 0)
        screen_rows = best_rows;
    out->ClearScreen(out);
    lines_shown = 0;
}

EFI_INPUT_KEY wait_key(void)
{
    UINTN index;
    EFI_INPUT_KEY key;
    sys->BootServices->WaitForEvent(1, &sys->ConIn->WaitForKey, &index);
    sys->ConIn->ReadKeyStroke(sys->ConIn, &key);
    return key;
}

void set_pager(BOOLEAN on)
{
    pager_on = on;
    sys->ConIn->Reset(sys->ConIn, 0);   /* drop keys pressed earlier */
}

void set_color(UINTN attribute)
{
    out->SetAttribute(out, attribute);
}

void print(const CHAR16 *s)
{
    out->OutputString(out, s);

    for (const CHAR16 *c = s; *c; c++)
        if (*c == u'\n')
            lines_shown++;
    if (!pager_on && lines_shown > screen_rows - 1)
        lines_shown = screen_rows - 1;
    if (pager_on && lines_shown >= screen_rows - 1) {
        UINTN attribute = (UINTN)out->Mode->Attribute;
        out->SetAttribute(out, EFI_BLACK | EFI_BACKGROUND_LIGHTGRAY);
        out->OutputString(out, u"-- more (press any key) --");
        out->SetAttribute(out, attribute);
        wait_key();
        out->OutputString(out, u"\r                          \r");
        lines_shown = 0;
    }
}

void print_ascii(const char *s)
{
    CHAR16 buf[64];
    while (*s) {
        UINTN n = 0;
        while (*s && n < 63)
            buf[n++] = (CHAR16)(unsigned char)*s++;
        buf[n] = 0;
        print(buf);
    }
}

void print_dec(UINT64 value, int width)
{
    CHAR16 buf[21];
    int i = 20;
    buf[i] = 0;
    do {
        buf[--i] = (CHAR16)(u'0' + value % 10);
        value /= 10;
    } while (value != 0);
    while (20 - i < width && i > 0)
        buf[--i] = u' ';
    print(&buf[i]);
}

void print_hex(UINT64 value, int digits)
{
    CHAR16 buf[19];
    if (digits < 1)
        digits = 1;
    if (digits > 16)
        digits = 16;
    buf[0] = u'0';
    buf[1] = u'x';
    for (int i = 0; i < digits; i++) {
        UINT64 nibble = (value >> ((digits - 1 - i) * 4)) & 0xF;
        buf[2 + i] = (CHAR16)(nibble < 10 ? u'0' + nibble : u'A' + nibble - 10);
    }
    buf[2 + digits] = 0;
    print(buf);
}

void print_size(UINT64 bytes, int width)
{
    /* Rounded to the nearest unit. */
    if (bytes < 1024 * 1024) {
        print_dec((bytes + 512) / 1024, width);
        print(u" KiB");
    } else {
        print_dec((bytes + 512 * 1024) / (1024 * 1024), width);
        print(u" MiB");
    }
}
