#!/usr/bin/env python3
"""Check a QEMU screendump (PPM) of RitterOS.

Usage: check_screen.py screen.ppm WIDTH HEIGHT MODE
  ok       the console after a normal boot: green status strip
  crash    the console after a crash report: red status strip
  terminal the console after the test suite's terminal session
  term-drawn, term-hidden, term-after, term-cleared
           the term test program's escape sequences (scripts/test.sh,
           term_session): drawn with the cursor, with the cursor hidden,
           the shell after the program ended, and after clear
  edit     the editor after typing two lines and saving (edit_session)
  term-ctrl-l after a line typed in UTF-8 and Ctrl-L (typing_session)

Positions and colors mirror kernel/main.c and kernel/console.[ch]; keep them
in sync when changing the layout.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ppm2png import read_ppm  # noqa: E402
from bdf import load_bdf  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REGULAR = load_bdf(os.path.join(ROOT, "fonts/9x15.bdf"))
BOLD = load_bdf(os.path.join(ROOT, "fonts/9x15B.bdf"))

TITLE_BAR = (30, 34, 60)

# kernel/console.[ch]
TEXT_LEFT, TEXT_TOP, STATUS_HEIGHT = 12, 48, 16
COLOR_BACKGROUND = (0x0C, 0x0E, 0x1C)
COLOR_TEXT = (0xC8, 0xC8, 0xC8)
COLOR_HEADING = (0x80, 0xC8, 0xFF)
COLOR_OK = (0x50, 0xDC, 0x78)
COLOR_ERROR = (0xFF, 0x60, 0x60)
STATUS_OK = (0x00, 0xC8, 0x50)
STATUS_CRASH = (0xDC, 0x00, 0x00)


def glyph_rows(font, codepoint):
    """Mirror of font_glyph() in kernel/font.c: bold falls back to regular."""
    if codepoint in font.glyphs:
        return font.glyphs[codepoint]
    return REGULAR.glyph(codepoint)


class Screen:
    def __init__(self, path):
        self.w, self.h, self.rgb = read_ppm(path)
        self.failures = 0

    def pixel(self, x, y):
        i = (y * self.w + x) * 3
        return tuple(self.rgb[i:i + 3])

    def report(self, ok, message):
        print(f"{'PASS' if ok else 'FAIL'}  screen: {message}")
        self.failures += not ok

    def check(self, name, actual, expected):
        self.report(actual == expected,
                    f"{name} = {actual}" + ("" if actual == expected else f", expected {expected}"))

    def text_errors(self, x0, y0, text, font, fg, bg):
        """Number of pixels in the text's cells that differ from fg/bg."""
        wrong = 0
        for n, ch in enumerate(text):
            rows = glyph_rows(font, ord(ch))
            for r in range(font.height):
                for c in range(font.width):
                    on = rows[r] >> (font.width - 1 - c) & 1
                    if self.pixel(x0 + n * font.width + c, y0 + r) != (fg if on else bg):
                        wrong += 1
        return wrong

    def check_text(self, name, x0, y0, text, font, fg, bg):
        wrong = self.text_errors(x0, y0, text, font, fg, bg)
        self.report(wrong == 0, f"text '{name}', {len(text)} characters"
                    + ("" if wrong == 0 else f", {wrong} wrong pixels"))
        return x0 + len(text) * font.width

    def check_console_line(self, text, font, fg):
        """The text must appear, pixel-exact, at the start of some console row."""
        rows = (self.h - TEXT_TOP - STATUS_HEIGHT - 4) // REGULAR.height
        for row in range(rows):
            y = TEXT_TOP + row * REGULAR.height
            if self.text_errors(TEXT_LEFT, y, text, font, fg, COLOR_BACKGROUND) == 0:
                self.report(True, f"console row {row}: '{text}'")
                return
        self.report(False, f"console line '{text}' not found")


def cell(row, col):
    """Top left corner of a text cell (kernel/console.c)."""
    return TEXT_LEFT + col * REGULAR.width, TEXT_TOP + row * REGULAR.height


def check_cell(s, name, row, col, text, font=REGULAR, fg=COLOR_TEXT, bg=COLOR_BACKGROUND):
    x, y = cell(row, col)
    s.check_text(f"{name} at row {row}, column {col}", x, y, text, font, fg, bg)


# The cursor: the cell with its colors swapped (a space: all foreground).
def check_cursor(s, name, row, col, shown):
    check_cell(s, name, row, col, " ", REGULAR,
               COLOR_BACKGROUND if shown else COLOR_TEXT,
               COLOR_TEXT if shown else COLOR_BACKGROUND)


def check_term(s, mode):
    """user/test/term.c's screen; the rows and columns are its sequences'
    (1-based there, 0-based here)."""
    cols = min((s.w - 2 * TEXT_LEFT) // REGULAR.width, 480)
    if mode == "term-drawn":
        check_cell(s, "marker", 0, 0, "term: drawn ")
        check_cell(s, "cursor position (3;5H)", 2, 0, "    here    ")
        check_cell(s, "reverse (7m)", 4, 0, "REV", REGULAR, COLOR_BACKGROUND, COLOR_TEXT)
        check_cell(s, "normal again (0m)", 4, 3, "  ")
        check_cell(s, "green (32m)", 5, 0, "green ", REGULAR, COLOR_OK)
        check_cell(s, "bold (1m)", 5, 6, "bold", BOLD)
        check_cell(s, "to the end of the line erased (K)", 6, 0, "XX        ")
        check_cell(s, "the whole line erased (2K)", 7, 0, "        ")
        check_cell(s, "a sequence split between writes", 8, 0, "split ")
        check_cell(s, "cursor moves (A, B, C, D)", 10, 10, " m ")
        check_cell(s, "the last column", 13, cols - 1, "W")
        check_cell(s, "wrapped by the next character, not by 0m", 14, 0, "Z ")
        check_cursor(s, "cursor shown", 15, 19, True)
    elif mode == "term-hidden":
        check_cell(s, "marker", 0, 0, "term: hidden ")
        check_cursor(s, "cursor hidden (?25l)", 15, 19, False)
    elif mode == "term-ctrl-l":
        # A screen full of a listing, then a line typed in UTF-8 and
        # Ctrl-L: only the prompt and that line, at the top (its glyphs
        # drawn whole), the cursor after it.
        line = "ritteros:/$ echo Gr\u00fc\u00dfe \u2013 \u2713 \u20ac"
        check_cell(s, "the prompt and the line again, at the top", 0, 0, line)
        check_cursor(s, "the cursor after the line", 0, len(line), True)
        for row in (1, 2, 6, 10):
            check_cell(s, f"row {row} empty", row, 0, " " * 20)
    elif mode == "term-cleared":
        # clear, then "echo again": only those at the top, the rest empty
        # (ls / filled rows before).
        check_cell(s, "the prompt after clear, at the top", 0, 0, "ritteros:/$ echo again ")
        check_cell(s, "its output", 1, 0, "again ")
        check_cell(s, "the next prompt", 2, 0, "ritteros:/$ ")
        check_cursor(s, "the cursor after it", 2, 12, True)
        for row in (3, 4, 8, 12):
            check_cell(s, f"row {row} empty", row, 0, " " * 20)
    else:
        # The program left reverse video on and the cursor hidden; the
        # kernel undid both when it ended.
        check_cell(s, "the shell's prompt, normal colors", 0, 0, "ritteros:/$ echo after ")
        check_cell(s, "its output", 1, 0, "after ")
        check_cell(s, "the next prompt", 2, 0, "ritteros:/$ ")
        check_cursor(s, "the cursor shown again", 2, 12, True)


def check_edit(s):
    """user/bin/edit.c after typing "Hello", Enter, "world" and Ctrl-S
    (scripts/test.sh, edit_session): the text, the bar in reverse video one
    row above the last, the message on the last row, the cursor after
    "world"."""
    cols = min((s.w - 2 * TEXT_LEFT) // REGULAR.width, 480)
    rows = (s.h - TEXT_TOP - STATUS_HEIGHT - 4) // REGULAR.height
    bar, place = rows - 2, "line 2/3, column 6 "
    check_cell(s, "the first line", 0, 0, "Hello ")
    check_cell(s, "the second line", 1, 0, "world")
    check_cell(s, "below the text: empty", 2, 0, "      ")
    check_cell(s, "the bar: name", bar, 0, " edit: /notes.txt   ", REGULAR, COLOR_BACKGROUND, COLOR_TEXT)
    check_cell(s, "the bar: the cursor's place", bar, cols - len(place), place, REGULAR,
               COLOR_BACKGROUND, COLOR_TEXT)
    check_cell(s, "the message", rows - 1, 0, "Saved: 2 lines, 12 bytes. ")
    check_cursor(s, "the cursor after 'world'", 1, 5, True)


def check_pixel(s, mode):
    """px-X-Y-R-G-B: the pixel at (X, Y) is that color."""
    _, x, y, r, g, b = mode.split("-")
    want = (int(r), int(g), int(b))
    s.report(s.pixel(int(x), int(y)) == want,
             f"pixel {int(x)},{int(y)} = {s.pixel(int(x), int(y))}, wanted {want}")


def check_pixels(s, mode):
    """pixels-X.Y.R.G.B_X.Y.R.G.B...: each of those pixels is that color
    (many points in one look, e.g. a page of test stripes)."""
    for point in mode[len("pixels-"):].split("_"):
        x, y, r, g, b = (int(v) for v in point.split("."))
        got = s.pixel(x, y)
        s.report(got == (r, g, b), f"pixel {x},{y} = {got}, wanted {(r, g, b)}")


def check_not_pixel(s, mode):
    """notpx-X-Y-R-G-B: the pixel at (X, Y) is NOT that color."""
    _, x, y, r, g, b = mode.split("-")
    nope = (int(r), int(g), int(b))
    s.report(s.pixel(int(x), int(y)) != nope,
             f"pixel {int(x)},{int(y)} = {s.pixel(int(x), int(y))}, wanted anything but {nope}")


def check_region(s, mode):
    """region-X-Y-W-H-R-G-B: at least one pixel of that color is in the region
    (used to confirm text was drawn, without pinning exact glyph pixels)."""
    _, x, y, w, h, r, g, b = mode.split("-")
    want = (int(r), int(g), int(b))
    x, y, w, h = int(x), int(y), int(w), int(h)
    found = sum(1 for yy in range(y, y + h) for xx in range(x, x + w) if s.pixel(xx, yy) == want)
    s.report(found > 0, f"region {x},{y} {w}x{h} has {found} pixel(s) of {want}")


def check_text_at(s, mode):
    """textat-X-Y-FONT-R.G.B-R.G.B-TEXT (41g-4): TEXT (the rest, spaces and
    dashes allowed) drawn pixel-exact at (X, Y) in FONT (regular or bold),
    the first colour on the second."""
    _, x, y, font, fg, bg, text = mode.split("-", 6)
    s.check_text(text, int(x), int(y), text, BOLD if font == "bold" else REGULAR,
                 tuple(int(v) for v in fg.split(".")), tuple(int(v) for v in bg.split(".")))


def check_fill(s, mode):
    """fill-R-G-B: the whole screen is that solid color (a grid of samples)."""
    _, r, g, b = mode.split("-")
    want = (int(r), int(g), int(b))
    bad = 0
    for yy in range(8):
        for xx in range(8):
            if s.pixel(s.w * (2 * xx + 1) // 16, s.h * (2 * yy + 1) // 16) != want:
                bad += 1
    s.report(bad == 0, f"screen filled with {want} ({bad}/64 samples wrong)")


def check_windowd(s):
    """windowd composited the test client's window (200x150 green at outer
    100,100 with a title bar and border) on the desktop."""
    s.check("desktop background", s.pixel(10, 10), (46, 52, 64))        # DESKTOP_BG 0x2E3440
    s.check("window title bar", s.pixel(150, 108), (94, 129, 172))      # TITLEBAR_BG 0x5E81AC
    s.check("window content", s.pixel(160, 170), (0, 200, 80))          # the client's green


def check_restored(s):
    """The text console's title bar is back (the display was given back)."""
    x = s.check_text("title (bold)", 16, 12, "RitterOS", BOLD, (255, 255, 255), TITLE_BAR)
    s.check_text("title", x, 12, " kernel · console", REGULAR, (170, 190, 230), TITLE_BAR)


def check_console(s, crashed):
    x = s.check_text("title (bold)", 16, 12, "RitterOS", BOLD, (255, 255, 255), TITLE_BAR)
    s.check_text("title", x, 12, " kernel · console", REGULAR, (170, 190, 230), TITLE_BAR)
    if crashed:
        s.check_console_line("=== RitterOS ", BOLD, COLOR_ERROR)   # "crash: ..." or "panic: ..."
        s.check_console_line("System halted.", REGULAR, COLOR_ERROR)
        s.check("status strip (crash)", s.pixel(s.w // 2, s.h - 8), STATUS_CRASH)
    else:
        s.check_console_line("RitterOS kernel: boot complete", REGULAR, COLOR_OK)
        s.check("status strip (ok)", s.pixel(s.w // 2, s.h - 8), STATUS_OK)


def check_terminal(s):
    """After the test's terminal session (scripts/test.sh, terminal_session): it
    ends in the kernel's rescue console."""
    s.check_console_line("rescue> ", REGULAR, COLOR_HEADING)
    s.check_console_line("unknown command 'nosuch' (type 'help')", REGULAR, COLOR_TEXT)
    s.check("status strip (ok)", s.pixel(s.w // 2, s.h - 8), STATUS_OK)


def main():
    mode = sys.argv[4] if len(sys.argv) == 5 else None
    if not mode or (mode not in ("ok", "crash", "terminal", "term-drawn", "term-hidden",
                                 "term-after", "term-cleared", "term-ctrl-l", "edit", "restored",
                                 "windowd")
                    and not mode.startswith("fill-") and not mode.startswith("px-")
                    and not mode.startswith("notpx-") and not mode.startswith("region-")
                    and not mode.startswith("pixels-") and not mode.startswith("textat-")):
        sys.exit(__doc__)
    s = Screen(sys.argv[1])
    s.check("resolution", (s.w, s.h), (int(sys.argv[2]), int(sys.argv[3])))
    if s.failures == 0:
        if mode == "terminal":
            check_terminal(s)
        elif mode == "edit":
            check_edit(s)
        elif mode == "restored":
            check_restored(s)
        elif mode == "windowd":
            check_windowd(s)
        elif mode.startswith("fill-"):
            check_fill(s, mode)
        elif mode.startswith("notpx-"):
            check_not_pixel(s, mode)
        elif mode.startswith("px-"):
            check_pixel(s, mode)
        elif mode.startswith("pixels-"):
            check_pixels(s, mode)
        elif mode.startswith("region-"):
            check_region(s, mode)
        elif mode.startswith("textat-"):
            check_text_at(s, mode)
        elif mode.startswith("term-"):
            check_term(s, mode)
        else:
            check_console(s, mode == "crash")
    return 1 if s.failures else 0


if __name__ == "__main__":
    sys.exit(main())
