/*
 * Text in user space for the window system (step 15f; fonts from files since
 * step 22c): glyphs drawn into a 32-bit pixel buffer. The default face, 9x15
 * (and its bold, 9x15B), is compiled into the library -- the same
 * public-domain X11 misc-fixed glyphs the console uses -- and always there;
 * the other fonts are the BDF files in /system/fonts, read when a program
 * first asks for one. A font is opaque: programs hold a pointer and ask
 * through these functions, so its layout can change (wider glyphs,
 * proportional faces) without breaking them. Used by libwindow and windowd.
 * (41c-3) Scalable fonts from TrueType and OpenType files: wfont_open.
 */
#ifndef RITTEROS_WFONT_H
#define RITTEROS_WFONT_H

#include <stdint.h>

struct font;

#define WFONT_TRANSPARENT 0xFFFFFFFFu        /* leave the pixels behind the text */
#define WFONT_DIR "/system/fonts"            /* the font files: NAME.bdf */

/* The compiled-in default face, 9x15, and its bold, 9x15B (whose missing
 * glyphs come from the regular one): always there, and the fallback when a
 * font file is missing or cannot be read. Functions, not variables: a shared
 * library exports functions only (22c). */
const struct font *wfont_regular(void);
const struct font *wfont_bold(void);

/* The font NAME from WFONT_DIR/NAME.bdf ("10x20", "9x18B", "helvR12", ...):
 * a bitmap BDF font with glyphs up to 64 pixels wide, fixed-width or
 * proportional, read the first time it is asked for, then kept for the
 * program's life. 0 if there is no such file, it
 * cannot be read or is not such a font: use wfont_regular() then. A bold
 * face (NAMEB) takes missing glyphs from NAME. Not thread-safe (the first
 * call per font should come from one thread). */
const struct font *wfont_find(const char *name);

/* (41c-3) A TrueType or OpenType font file (.ttf, .otf: TrueType or CFF
 * outlines; the FreeFont faces in WFONT_DIR, FreeSans.ttf ...) at `size`
 * pixels an em: `path` a file's path, or a bare file name in WFONT_DIR.
 * Its glyphs are drawn smooth (each pixel by how much of it the glyph
 * covers, blended over what is there) at any size from 4 to 1000, the pen
 * moving by the exact advances (a glyph at the nearest quarter pixel);
 * wfont_width is its widest advance, wfont_height its ascent, descent and
 * line gap. Read the first time it is asked for at that size, then kept for
 * the program's life; 0 when there is no such file or it is not a font this
 * reader knows. Its functions may be called from several threads. (41d) A
 * size of 0 is the user's: text_size in ~/.config/windows (else
 * /system/config/windows; 8-64, 15 if not set). */
const struct font *wfont_open(const char *path, int size);

/* A font's character cell in pixels: every glyph of a fixed-width font is
 * this wide (a proportional font's widest; measure text with
 * wfont_text_width), and every line this tall. */
int wfont_width(const struct font *font);
int wfont_height(const struct font *font);

/* The width in pixels wfont_text takes for utf8 with this font. */
int wfont_text_width(const struct font *font, const char *utf8);

/* (41d) Text over several lines, with any font. wfont_wrap gives how many
 * bytes of utf8 its first line takes when lines are at most `width` pixels
 * wide: a line breaks after the spaces following a word (spaces at its end
 * do not count to its width), inside a word only when the word alone is
 * wider than a line, and after a newline (its byte counted); at least one
 * character. The next line starts that many bytes on. wfont_layout draws
 * utf8 so wrapped at x, y, a line wfont_height below the one before (as
 * wfont_text, newlines not drawn), and returns its height; buf 0 only
 * measures it. */
int wfont_wrap(const struct font *font, const char *utf8, int width);
int wfont_layout(uint32_t *buf, uint32_t stride, uint32_t bw, uint32_t bh, const struct font *font, int x,
                 int y, int width, const char *utf8, uint32_t fg, uint32_t bg);

/* Decode one UTF-8 character at *s and advance past it (0 at the end). */
uint32_t utf8_next(const char **s);

/* Draw a UTF-8 string into the pixel buffer `buf` (bw x bh pixels, `stride`
 * pixels per row) with its top-left at (x, y). Colors are native pixels; bg
 * WFONT_TRANSPARENT leaves the background. Returns the x after the text. */
int wfont_text(uint32_t *buf, uint32_t stride, uint32_t bw, uint32_t bh,
               const struct font *font, int x, int y, const char *utf8,
               uint32_t fg, uint32_t bg);

#endif
