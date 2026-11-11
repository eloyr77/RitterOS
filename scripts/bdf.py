"""Minimal reader for BDF bitmap fonts (Glyph Bitmap Distribution Format 2.1).

load_bdf() returns a Font whose glyphs are already placed in the font's
character cell: one integer per cell row, with bit (width - 1) the leftmost
pixel. Used by check_screen.py (test); the build has scripts/bdf2c.c (63).
"""


class Font:
    def __init__(self, width, height, ascent, glyphs, default_char):
        self.width = width            # cell width in pixels
        self.height = height          # cell height in pixels
        self.ascent = ascent          # pixels above the baseline
        self.glyphs = glyphs          # code point -> list of `height` row ints
        self.default_char = default_char

    def glyph(self, codepoint):
        """Rows for codepoint, falling back to U+FFFD and then the default char."""
        for c in (codepoint, 0xFFFD, self.default_char):
            if c in self.glyphs:
                return self.glyphs[c]
        return [0] * self.height


def load_bdf(path):
    with open(path, encoding="latin-1") as f:
        lines = f.read().splitlines()

    width = height = offset_y = ascent = None
    default_char = 0
    glyphs = {}
    i = 0
    while i < len(lines):
        words = lines[i].split()
        key = words[0] if words else ""
        if key == "FONTBOUNDINGBOX":
            width, height, _, offset_y = map(int, words[1:5])
        elif key == "FONT_ASCENT":
            ascent = int(words[1])
        elif key == "DEFAULT_CHAR":
            default_char = int(words[1])
        elif key == "STARTCHAR":
            encoding, bbx, rows = None, None, []
            i += 1
            while not lines[i].startswith("ENDCHAR"):
                w = lines[i].split()
                if w[0] == "ENCODING":
                    encoding = int(w[1])
                elif w[0] == "BBX":
                    bbx = tuple(map(int, w[1:5]))
                elif w[0] == "BITMAP":
                    i += 1
                    while not lines[i].startswith("ENDCHAR"):
                        rows.append(int(lines[i], 16))
                        i += 1
                    break
                i += 1
            if encoding is not None and encoding >= 0 and bbx is not None:
                glyphs[encoding] = place(rows, bbx, width, height, offset_y)
        i += 1

    if width is None or ascent is None:
        raise ValueError(f"{path}: missing FONTBOUNDINGBOX or FONT_ASCENT")
    return Font(width, height, ascent, glyphs, default_char)


def place(rows, bbx, cell_w, cell_h, cell_offset_y):
    """Position a glyph's bitmap (BBX w h xoff yoff) inside the font cell."""
    w, h, xoff, yoff = bbx
    row_bits = (w + 7) // 8 * 8                  # BDF pads each row to whole bytes
    # Rows count downward from the cell top; the cell bottom is at cell_offset_y.
    top = (cell_h + cell_offset_y) - (h + yoff)
    cell = [0] * cell_h
    for r, bits in enumerate(rows):
        y = top + r
        if not 0 <= y < cell_h:
            continue
        for c in range(w):
            if bits >> (row_bits - 1 - c) & 1:
                x = xoff + c
                if 0 <= x < cell_w:
                    cell[y] |= 1 << (cell_w - 1 - x)
    return cell
