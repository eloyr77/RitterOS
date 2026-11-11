#!/usr/bin/env python3
"""The apps' icons (41g-3): 32x32 RGBA PNGs drawn here from simple shapes
(ours: no picture taken from elsewhere), in the window system's Nord
colours, written as user/apps/NAME.png beside each app's NAME.info.

    python3 scripts/mkicons.py [DIR]      (DIR: user/apps by default)

The PNGs are kept in git (RitterOS's own build has no Python): run this
after changing a drawing and commit what it writes. Each pixel is the mean
of 4x4 samples, each the colour of the last shape that covers it (a shape
list paints back to front); uncovered samples are transparent.
"""
import math
import os
import struct
import sys
import zlib

SIZE, SUB = 32, 4

NORD = {
    'night': (0x2E, 0x34, 0x40), 'grey': (0x4C, 0x56, 0x6A), 'snow': (0xEC, 0xEF, 0xF4),
    'frost': (0x88, 0xC0, 0xD0), 'blue': (0x81, 0xA1, 0xC1), 'deep': (0x5E, 0x81, 0xAC),
    'red': (0xBF, 0x61, 0x6A), 'orange': (0xD0, 0x87, 0x70), 'yellow': (0xEB, 0xCB, 0x8B),
    'green': (0xA3, 0xBE, 0x8C),
}


def rounded(x0, y0, x1, y1, r):
    """Inside a rectangle with corners rounded by r."""
    def inside(x, y):
        if not (x0 <= x <= x1 and y0 <= y <= y1):
            return False
        cx = min(max(x, x0 + r), x1 - r)
        cy = min(max(y, y0 + r), y1 - r)
        return (x - cx) ** 2 + (y - cy) ** 2 <= r * r
    return inside


def circle(cx, cy, r):
    return lambda x, y: (x - cx) ** 2 + (y - cy) ** 2 <= r * r


def ring(cx, cy, r, w):
    return lambda x, y: (r - w) ** 2 <= (x - cx) ** 2 + (y - cy) ** 2 <= r * r


def ellipse_ring(cx, cy, rx, ry, w):
    def inside(x, y):
        d = ((x - cx) / rx) ** 2 + ((y - cy) / ry) ** 2
        d2 = ((x - cx) / max(rx - w, 0.01)) ** 2 + ((y - cy) / max(ry - w, 0.01)) ** 2
        return d <= 1 and d2 >= 1
    return inside


def polygon(points):
    """Inside a polygon (even-odd crossings)."""
    def inside(x, y):
        c = False
        for i in range(len(points)):
            (ax, ay), (bx, by) = points[i], points[i - 1]
            if (ay > y) != (by > y) and x < (bx - ax) * (y - ay) / (by - ay) + ax:
                c = not c
        return c
    return inside


def both(a, b):
    return lambda x, y: a(x, y) and b(x, y)


def stroke(x0, y0, x1, y1, w):
    """A line from (x0, y0) to (x1, y1), w wide, square ends."""
    dx, dy = x1 - x0, y1 - y0
    n = math.hypot(dx, dy)
    nx, ny = -dy / n * w / 2, dx / n * w / 2
    return polygon([(x0 + nx, y0 + ny), (x1 + nx, y1 + ny), (x1 - nx, y1 - ny), (x0 - nx, y0 - ny)])


ICONS = {
    # a terminal: a dark screen in a grey frame, a green prompt and a cursor
    'term': [
        (rounded(2, 4, 30, 28, 3), 'grey'),
        (rounded(4, 6, 28, 26, 2), 'night'),
        (stroke(8, 11, 13, 15.5, 2.2), 'green'),
        (stroke(13, 15.5, 8, 20, 2.2), 'green'),
        (rounded(15, 19, 22, 21, 0.5), 'snow'),
    ],
    # owl, the file browser: a folder
    'owl': [
        (rounded(3, 6, 14, 12, 1.5), 'orange'),
        (rounded(3, 9, 29, 27, 2), 'orange'),
        (rounded(3, 12, 29, 27, 2), 'yellow'),
    ],
    # the image viewer: a picture of a hill under a sun, in a light frame
    'imageview': [
        (rounded(2, 5, 30, 27, 2), 'snow'),
        (rounded(4, 7, 28, 25, 1), 'blue'),
        (circle(21, 12.5, 3), 'yellow'),
        (both(rounded(4, 7, 28, 25, 1), polygon([(4, 25), (4, 20), (12, 13), (20, 21), (24, 18), (28, 22), (28, 25)])),
         'green'),
    ],
    # the video player: a red disc with a white play triangle
    'reel': [
        (circle(16, 16, 14), 'red'),
        (polygon([(12.5, 9.5), (12.5, 22.5), (23, 16)]), 'snow'),
    ],
    # iBrowser: a globe with its lines
    'ibrowser': [
        (circle(16, 16, 14), 'deep'),
        (ring(16, 16, 14, 1.6), 'frost'),
        (ellipse_ring(16, 16, 6.5, 14, 1.4), 'frost'),
        (both(circle(16, 16, 13), rounded(2, 15.3, 30, 16.7, 0)), 'frost'),
        (both(circle(16, 16, 13), rounded(2, 9.3, 30, 10.6, 0)), 'frost'),
        (both(circle(16, 16, 13), rounded(2, 21.4, 30, 22.7, 0)), 'frost'),
        (both(circle(16, 16, 13), rounded(15.3, 2, 16.7, 30, 0)), 'frost'),
    ],
}


def render(shapes):
    rows = []
    for py in range(SIZE):
        row = bytearray()
        for px in range(SIZE):
            r = g = b = a = 0
            for sy in range(SUB):
                for sx in range(SUB):
                    x, y = px + (sx + 0.5) / SUB, py + (sy + 0.5) / SUB
                    hit = None
                    for inside, colour in shapes:
                        if inside(x, y):
                            hit = colour
                    if hit:
                        c = NORD[hit]
                        r, g, b, a = r + c[0], g + c[1], b + c[2], a + 1
            n = SUB * SUB
            if a:                                   # colour of the covered part, alpha its share
                row += bytes((round(r / a), round(g / a), round(b / a), round(255 * a / n)))
            else:
                row += bytes(4)
        rows.append(bytes(row))
    return rows


def png(rows):
    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))
    raw = b''.join(b'\0' + r for r in rows)
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', SIZE, SIZE, 8, 6, 0, 0, 0)) +
            chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), '..', 'user', 'apps')
    for name, shapes in sorted(ICONS.items()):
        with open(os.path.join(out, name + '.png'), 'wb') as f:
            f.write(png(render(shapes)))
        print('mkicons: %s.png' % name)


if __name__ == '__main__':
    main()
