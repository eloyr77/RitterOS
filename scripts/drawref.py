#!/usr/bin/env python3
"""
drawref.py DRAWTEST: libdraw's anti-aliasing against exact areas (41b).

DRAWTEST is drawtest built on the Mac (-DDRAW_HOST); its `read` mode draws
the shapes given on standard input, white on black, and prints each
canvas's red channel. Here every shape is the same pieces libdraw makes
(a rectangle's outline: the rectangle less the inner one; a line: its
band; a polygon's outline: a band per edge and a bevel per corner, each
wound the same way), and each pixel's coverage is computed exactly: the
signed area of every piece clipped to the pixel's square (Sutherland and
Hodgman, in doubles), summed, its absolute value at most 1. The two must
agree within 1/255 for every pixel of every shape: fixed cases (edges on
pixel borders, vertical and horizontal ones, shapes smaller than a pixel,
outside the canvas, clipped, a star wound twice in its middle) and random
ones (seed 41).

  python3 scripts/drawref.py build/host/drawtest [COUNT]
  python3 scripts/drawref.py --glyphs TTFTEST FONT...   (41c-3: glyph masks)
"""
import math
import random
import subprocess
import sys


def area(pts):
    s = 0.0
    for i in range(len(pts)):
        x0, y0 = pts[i]
        x1, y1 = pts[(i + 1) % len(pts)]
        s += x0 * y1 - x1 * y0
    return s / 2


def clip(pts, inside, cross):
    out = []
    for i in range(len(pts)):
        a, b = pts[i - 1], pts[i]
        ia, ib = inside(a), inside(b)
        if ib:
            if not ia:
                out.append(cross(a, b))
            out.append(b)
        elif ia:
            out.append(cross(a, b))
    return out


def in_square(pts, x, y):
    """pts clipped to the square (x, y)-(x + 1, y + 1): its signed area."""
    def at_x(v):
        return lambda a, b: (v, a[1] + (b[1] - a[1]) * (v - a[0]) / (b[0] - a[0]))

    def at_y(v):
        return lambda a, b: (a[0] + (b[0] - a[0]) * (v - a[1]) / (b[1] - a[1]), v)
    for inside, cross in ((lambda p: p[0] >= x, at_x(x)), (lambda p: p[0] <= x + 1, at_x(x + 1)),
                          (lambda p: p[1] >= y, at_y(y)), (lambda p: p[1] <= y + 1, at_y(y + 1))):
        pts = clip(pts, inside, cross)
        if len(pts) < 3:
            return 0.0
    return area(pts)


def positive(pts):
    return pts if area(pts) >= 0 else pts[::-1]


def side(a, b, half):
    dx, dy = b[0] - a[0], b[1] - a[1]
    n = math.hypot(dx, dy)
    if n == 0:
        return None
    return (-dy / n * half, dx / n * half)


def pieces(shape):
    kind = shape[0]
    if kind == 'rect':
        x, y, w, h, line = shape[1:]
        if not (w > 0 and h > 0):
            return []
        out = [[(x, y), (x + w, y), (x + w, y + h), (x, y + h)]]
        if line > 0 and line * 2 < w and line * 2 < h:
            l = line
            out.append([(x + l, y + l), (x + l, y + h - l), (x + w - l, y + h - l), (x + w - l, y + l)])
        return out
    if kind == 'gradient':                              # (41b-3: its rectangle)
        return pieces(('rect',) + tuple(shape[1:5]) + (0,))
    if kind == 'image':                                 # (its rectangle turned about its centre)
        x, y, w, h, angle = shape[4:]
        if not (w > 0 and h > 0):
            return []
        a = math.radians(angle)
        c, s = math.cos(a), math.sin(a)
        cx, cy = x + w / 2, y + h / 2
        return [[(cx + X * c - Y * s, cy + X * s + Y * c) for X, Y in
                 ((-w / 2, -h / 2), (w / 2, -h / 2), (w / 2, h / 2), (-w / 2, h / 2))]]
    if kind == 'line':
        x0, y0, x1, y1, width = shape[1:]
        n = side((x0, y0), (x1, y1), width / 2) if width > 0 else None
        if not n:
            return []
        nx, ny = n
        return [positive([(x0 + nx, y0 + ny), (x1 + nx, y1 + ny), (x1 - nx, y1 - ny), (x0 - nx, y0 - ny)])]
    line, pts = shape[1], shape[2]
    if len(pts) < 3:
        return []
    if not line > 0:
        return [pts]
    out, half = [], line / 2
    for i in range(len(pts)):
        a, b, c = pts[i], pts[(i + 1) % len(pts)], pts[(i + 2) % len(pts)]
        n = side(a, b, half)
        if not n:
            continue
        nx, ny = n
        out.append(positive([(a[0] + nx, a[1] + ny), (b[0] + nx, b[1] + ny), (b[0] - nx, b[1] - ny),
                             (a[0] - nx, a[1] - ny)]))
        m = side(b, c, half)
        if not m:
            continue
        turn = (b[0] - a[0]) * (c[1] - b[1]) - (b[1] - a[1]) * (c[0] - b[0])
        if turn == 0:
            continue
        s = -1 if turn > 0 else 1
        out.append(positive([b, (b[0] + s * nx, b[1] + s * ny), (b[0] + s * m[0], b[1] + s * m[1])]))
    return out


def disk_below(r, x, y):
    """The area of the disk of radius r around 0 where X <= x and Y <= y."""
    if x <= -r or y <= -r:
        return 0.0
    t = min(x, r)
    h = lambda v: math.sqrt(max(r * r - v * v, 0.0))
    prim = lambda v: (v * h(v) + r * r * math.asin(max(-1.0, min(1.0, v / r)))) / 2   # (the integral of h)
    if y >= r:
        return 2 * (prim(t) - prim(-r))
    a = math.sqrt(r * r - y * y)
    total = 0.0
    for lo, hi, kind in ((-r, -a, 'outer'), (-a, a, 'inner'), (a, r, 'outer')):
        hi = min(hi, t)
        if hi <= lo:
            continue
        if kind == 'inner':                             # (the disk's part below y)
            total += y * (hi - lo) + prim(hi) - prim(lo)
        elif y >= 0:                                     # (all of it is below y)
            total += 2 * (prim(hi) - prim(lo))
    return total


def disk_in_rect(cx, cy, r, x0, y0, x1, y1):
    if x1 <= x0 or y1 <= y0 or r <= 0:
        return 0.0
    f = lambda x, y: disk_below(r, x - cx, y - cy)
    return max(0.0, f(x1, y1) - f(x0, y1) - f(x1, y0) + f(x0, y0))


def ellipse_in_rect(cx, cy, rx, ry, x0, y0, x1, y1):
    k = ry / rx                                          # (across scaled to a circle of radius ry)
    return disk_in_rect(cx, cy, ry, cx + (x0 - cx) * k, y0, cx + (x1 - cx) * k, y1) / k


def rect_in_rect(a, b):
    return max(0.0, min(a[2], b[2]) - max(a[0], b[0])) * max(0.0, min(a[3], b[3]) - max(a[1], b[1]))


def round_rect_in_rect(x, y, w, h, r, box):
    if r <= 0:
        return rect_in_rect((x, y, x + w, y + h), box)
    total = rect_in_rect((x, y + r, x + w, y + h - r), box)
    total += rect_in_rect((x + r, y, x + w - r, y + r), box)
    total += rect_in_rect((x + r, y + h - r, x + w - r, y + h), box)
    for cx, cy, sq in ((x + r, y + r, (x, y, x + r, y + r)), (x + w - r, y + r, (x + w - r, y, x + w, y + r)),
                       (x + w - r, y + h - r, (x + w - r, y + h - r, x + w, y + h)),
                       (x + r, y + h - r, (x, y + h - r, x + r, y + h))):
        part = (max(sq[0], box[0]), max(sq[1], box[1]), min(sq[2], box[2]), min(sq[3], box[3]))
        total += disk_in_rect(cx, cy, r, *part)
    return total


def curved(shape):
    """(41b-2) A curved shape as its box and its area in a box (an outline:
    the shape's less the inner one's), or None for another shape."""
    kind = shape[0]
    if kind in ('circle', 'ellipse'):
        if kind == 'circle':
            cx, cy, r, line = shape[1:]
            rx = ry = r
        else:
            cx, cy, rx, ry, line = shape[1:]
        if not (rx > 0 and ry > 0):
            return (0, 0, 0, 0), lambda b: 0.0
        inner = line > 0 and line < rx and line < ry

        def area(b):
            a = ellipse_in_rect(cx, cy, rx, ry, *b)
            return a - ellipse_in_rect(cx, cy, rx - line, ry - line, *b) if inner else a
        return (cx - rx, cy - ry, cx + rx, cy + ry), area
    if kind == 'rrect':
        x, y, w, h, radius, line = shape[1:]
        if not (w > 0 and h > 0):
            return (0, 0, 0, 0), lambda b: 0.0
        r = min(max(radius, 0.0), w / 2, h / 2)
        inner = line > 0 and line * 2 < w and line * 2 < h

        def area(b):
            a = round_rect_in_rect(x, y, w, h, r, b)
            if inner:
                a -= round_rect_in_rect(x + line, y + line, w - 2 * line, h - 2 * line, max(r - line, 0.0), b)
            return a
        return (x, y, x + w, y + h), area
    return None


def coverage(shape, w, h, box):
    c = curved(shape)
    if c:
        (bx0, by0, bx1, by1), area = c
        cx0, cy0, cx1, cy1 = box
        cov = [[0.0] * w for _ in range(h)]
        for y in range(max(cy0, math.floor(by0)), min(cy1, math.ceil(by1))):
            for x in range(max(cx0, math.floor(bx0)), min(cx1, math.ceil(bx1))):
                cov[y][x] = min(abs(area((x, y, x + 1, y + 1))), 1.0)
        return cov
    return polygon_coverage(shape, w, h, box)


def polygon_coverage(shape, w, h, box):
    cx0, cy0, cx1, cy1 = box
    cov = [[0.0] * w for _ in range(h)]
    for piece in pieces(shape):
        xs = [p[0] for p in piece]
        ys = [p[1] for p in piece]
        for y in range(max(cy0, math.floor(min(ys))), min(cy1, math.ceil(max(ys)))):
            for x in range(max(cx0, math.floor(min(xs))), min(cx1, math.ceil(max(xs)))):
                cov[y][x] += in_square(piece, x, y)
    return [[min(abs(v), 1.0) for v in row] for row in cov]


def text(shape):
    f = lambda v: '%.4f' % v
    if shape[0] == 'poly':
        return 'poly %s %d %s' % (f(shape[1]), len(shape[2]), ' '.join(f(v) for p in shape[2] for v in p))
    if shape[0] == 'image':
        return 'image %d %d %d %s' % (shape[1], shape[2], shape[3], ' '.join(f(v) for v in shape[4:]))
    return ' '.join([shape[0]] + [f(v) for v in shape[1:]])


def cases(count):
    out = [  # (w, h, clip or None, shape)
        (16, 16, None, ('rect', 2, 3, 9, 7, 0)),
        (16, 16, None, ('rect', 2.25, 3.5, 9.5, 7.75, 0)),
        (16, 16, None, ('rect', 2, 2, 12, 12, 1)),
        (16, 16, None, ('rect', 1.5, 1.5, 12, 11, 2.5)),
        (16, 16, None, ('rect', 4.3, 4.6, 0.4, 0.3, 0)),
        (16, 16, None, ('line', 1, 8, 15, 8, 1)),
        (16, 16, None, ('line', 1, 8.5, 15, 8.5, 1)),
        (16, 16, None, ('line', 8, 1, 8, 15, 2)),
        (16, 16, None, ('line', 1, 1, 15, 14, 1.5)),
        (24, 24, None, ('poly', 0, [(12, 1), (16, 22), (2, 8), (22, 8), (6, 22)])),
        (24, 24, None, ('poly', 2, [(3, 3), (20, 3), (20, 20), (3, 20)])),
        (24, 24, None, ('poly', 1.5, [(3, 4), (21, 9), (8, 20)])),
        (16, 16, None, ('rect', -5, -5, 10, 30, 0)),
        (16, 16, None, ('rect', 10, 10, 20, 20, 0)),
        (16, 16, None, ('poly', 0, [(-100, -50), (300, 2), (8, 400)])),
        (16, 16, (3, 4, 7, 6), ('rect', 0, 0, 16, 16, 0)),
        (16, 16, (5, 5, 20, 20), ('poly', 0, [(1, 1), (15, 2.5), (7, 15)])),
        (32, 32, None, ('circle', 16, 16, 12.5, 0)),
        (32, 32, None, ('circle', 15.5, 16.25, 13, 2)),
        (8, 8, None, ('circle', 4.3, 4.6, 0.4, 0)),
        (16, 16, None, ('circle', 8, 8, 6, 9)),
        (40, 24, None, ('ellipse', 20, 12, 18, 9.5, 0)),
        (32, 36, None, ('ellipse', 16, 18, 14, 16, 1.5)),
        (24, 32, None, ('rrect', 3, 3, 18, 26, 5, 0)),
        (28, 36, None, ('rrect', 2.5, 2.5, 22, 30, 40, 1)),
        (24, 24, None, ('rrect', 2, 2, 20, 20, 0, 2)),
        (32, 32, None, ('circle', 16, 1010, 1000, 2)),
        (24, 24, (4, 6, 10, 9), ('circle', 12, 12, 11, 0)),
        (48, 16, None, ('gradient', 2, 2, 44, 12, 2, 0, 46, 0)),
        (24, 40, None, ('gradient', 2.5, 2.25, 19, 36, 0, 4, 0, 36)),
        (32, 32, None, ('gradient', 1, 1, 30, 30, 4, 4, 28, 20)),
        (16, 16, None, ('gradient', 1, 1, 14, 14, 5, 5, 5, 5)),
        (24, 24, None, ('image', 0, 8, 8, 4, 4, 16, 10, 0)),
        (32, 32, None, ('image', 0, 8, 8, 6.5, 8.25, 18, 12, 30)),
        (32, 32, None, ('image', 0, 8, 8, 6, 8, 20, 14, 135)),
        (24, 20, None, ('image', 1, 16, 12, 4, 4, 16, 12, 0)),
        (24, 24, None, ('image', 1, 16, 12, 4, 6, 16, 12, 90)),
        (24, 20, None, ('image', 1, 16, 12, 4, 4, 16, 12, 180)),
        (24, 24, None, ('image', 1, 16, 12, 4, 6, 16, 12, 270)),
        (24, 24, None, ('image', 1, 16, 12, 4, 6, 16, 12, -90)),
    ]
    rnd = random.Random(41)
    for _ in range(count):
        w, h = rnd.randint(4, 32), rnd.randint(4, 32)
        r = lambda lo, hi: round(rnd.uniform(lo, hi), 2)
        k = rnd.random()
        if k < 0.12:
            shape = ('rect', r(-4, w), r(-4, h), r(0.1, w), r(0.1, h), rnd.choice([0, 0, r(0.2, 4)]))
        elif k < 0.2:
            shape = ('circle', r(-4, w + 4), r(-4, h + 4), r(0.2, 20), rnd.choice([0, 0, r(0.2, 5)]))
        elif k < 0.28:
            shape = ('ellipse', r(-4, w + 4), r(-4, h + 4), r(0.2, 20), r(0.2, 20), rnd.choice([0, 0, r(0.2, 5)]))
        elif k < 0.36:
            shape = ('rrect', r(-4, w), r(-4, h), r(0.1, w), r(0.1, h), r(0, 12), rnd.choice([0, 0, r(0.2, 4)]))
        elif k < 0.44:
            shape = ('gradient', r(-4, w), r(-4, h), r(0.1, w), r(0.1, h), r(-4, w + 4), r(-4, h + 4),
                     r(-4, w + 4), r(-4, h + 4))
        elif k < 0.52:
            shape = ('image', 0, rnd.randint(1, 9), rnd.randint(1, 9), r(-4, w), r(-4, h), r(0.5, w),
                     r(0.5, h), r(-360, 360))
        elif k < 0.65:
            shape = ('line', r(-4, w + 4), r(-4, h + 4), r(-4, w + 4), r(-4, h + 4), r(0.2, 5))
        else:
            n = rnd.randint(3, 8)
            shape = ('poly', rnd.choice([0, 0, r(0.3, 3)]), [(r(-6, w + 6), r(-6, h + 6)) for _ in range(n)])
        clip_box = None
        if rnd.random() < 0.2:
            clip_box = (rnd.randint(-2, w), rnd.randint(-2, h), rnd.randint(0, w), rnd.randint(0, h))
        out.append((w, h, clip_box, shape))
    return out


def expected(shape, w, h, box):
    """Each pixel's red channel as drawn white on black (or, for 41b-3,
    black to white, or the gray pattern), and how far it may be off."""
    cov = coverage(shape, w, h, box)
    values = [[int(v * 255 + 0.5) for v in row] for row in cov]
    if shape[0] == 'gradient':                          # (the colour where each centre falls)
        x0, y0, x1, y1 = shape[5:9]
        dx, dy = x1 - x0, y1 - y0
        n = dx * dx + dy * dy
        for y in range(h):
            for x in range(w):
                t = ((x + 0.5 - x0) * dx + (y + 0.5 - y0) * dy) / n if n > 0 else 0
                g = int(255 * min(max(t, 0.0), 1.0) + 0.5)
                values[y][x] = (g * values[y][x] + 127) // 255
    if shape[0] == 'image' and shape[1] == 1:           # (the pattern copied: right angles, own size)
        kind, iw, ih, x0, y0, dw, dh, angle = shape[1:]
        q = int(angle) // 90 % 4
        c, s = (1, 0, -1, 0)[q], (0, 1, 0, -1)[q]
        cx, cy = x0 + dw / 2, y0 + dh / 2
        for y in range(h):
            for x in range(w):
                if values[y][x] == 255:
                    px, py = x + 0.5 - cx, y + 0.5 - cy
                    u, v = px * c + py * s + iw / 2, py * c - px * s + ih / 2
                    values[y][x] = (7 * math.floor(u) + 13 * math.floor(v)) & 255
        return values, 0
    return values, 1


def glyph_outlines(ttftest, font):
    """(41c-3) A font's outlines as ttftest dumps them: {glyph: [contour]},
    a contour its start and segments (L, Q, C) in font units; and its map."""
    out = subprocess.run([ttftest, 'dump', font], capture_output=True, text=True).stdout.split('\n')
    glyphs, chars, g, start, segs = {}, {}, None, None, []
    for line in out:
        w = line.split()
        if not w:
            continue
        if w[0] == 'font':
            upem = int(w[2])
        elif w[0] == 'glyph':
            g = int(w[1])
            glyphs[g] = []
        elif w[0] == 'M':
            start, segs = (float(w[1]), float(w[2])), []
        elif w[0] in ('L', 'Q', 'C'):
            v = list(map(float, w[1:]))
            segs.append((w[0], [(v[i], v[i + 1]) for i in range(0, len(v), 2)]))
        elif w[0] == 'Z':
            glyphs[g].append((start, segs))
        elif w[0] == 'char':
            chars[int(w[1])] = int(w[2])
    return upem, glyphs, chars


def flatten(contour, scale, dx):
    """A contour in pixels (y down), each curve in pieces straying at most
    0.0001 pixel (a quadratic's 1/n strays dd / 8n^2, a cubic's 3dd / 4n^2)."""
    tf = lambda p: (dx + p[0] * scale, -p[1] * scale)
    at = tf(contour[0])
    pts = [at]
    for kind, ps in contour[1]:
        ps = [tf(p) for p in ps]
        if kind == 'L':
            pts.append(ps[0])
        else:
            q = [at] + ps
            dd = max(math.hypot(q[i][0] - 2 * q[i + 1][0] + q[i + 2][0], q[i][1] - 2 * q[i + 1][1] + q[i + 2][1])
                     for i in range(len(q) - 2))
            pieces = max(4, math.ceil(math.sqrt((0.75 if kind == 'C' else 0.125) * dd / 0.0001)))
            for k in range(1, pieces + 1):
                t = k / pieces
                u = 1 - t
                if kind == 'Q':
                    (cx, cy), (x, y) = ps
                    pts.append((u * u * at[0] + 2 * u * t * cx + t * t * x, u * u * at[1] + 2 * u * t * cy + t * t * y))
                else:
                    (ax, ay), (bx, by), (x, y) = ps
                    pts.append((u ** 3 * at[0] + 3 * u * u * t * ax + 3 * u * t * t * bx + t ** 3 * x,
                                u ** 3 * at[1] + 3 * u * u * t * ay + 3 * u * t * t * by + t ** 3 * y))
        at = ps[-1]
    return pts


def glyph_coverage(polys, x0, y0, w, h):
    """Each pixel's exact area in the box: the contours (wound as they are)
    clipped to each row, then to each pixel; summed, at most 1."""
    def at_x(v):
        return lambda a, b: (v, a[1] + (b[1] - a[1]) * (v - a[0]) / (b[0] - a[0]))

    def at_y(v):
        return lambda a, b: (a[0] + (b[0] - a[0]) * (v - a[1]) / (b[1] - a[1]), v)
    cov = [[0.0] * w for _ in range(h)]
    for y in range(h):
        top, bottom = y0 + y, y0 + y + 1
        rows = []
        for poly in polys:
            r = clip(poly, lambda p: p[1] >= top, at_y(top))
            r = clip(r, lambda p: p[1] <= bottom, at_y(bottom)) if len(r) >= 3 else []
            if len(r) >= 3:
                rows.append(r)
        for x in range(w):
            left, right = x0 + x, x0 + x + 1
            total = 0.0
            for r in rows:
                q = clip(r, lambda p: p[0] >= left, at_x(left))
                q = clip(q, lambda p: p[0] <= right, at_x(right)) if len(q) >= 3 else []
                if len(q) >= 3:
                    total += area(q)
            cov[y][x] = min(abs(total), 1.0)
    return cov


def glyphs_main(ttftest, fonts):
    """drawref.py --glyphs TTFTEST FONT...: the reader's glyph masks (41c-3)
    against the exact areas of its outlines, at 9, 17 and 40 pixels an em
    and the pen on a whole pixel and half one."""
    text = 'ag@&esW8f' + chr(0x3A9) + chr(0x436) + chr(0x20AC)
    bad = checked = worst = 0
    for font in fonts:
        upem, outlines, chars = glyph_outlines(ttftest, font)
        ids = sorted({chars.get(ord(c), 0) for c in text})
        for size in (9, 17, 40):
            for dx in (0.0, 0.5):
                out = subprocess.run([ttftest, 'masks', font, str(size), str(dx)] + [str(g) for g in ids],
                                     capture_output=True, text=True).stdout.split('\n')
                k = 0
                while k < len(out) and out[k].startswith('mask'):
                    _, g, ok, left, top, w, h = out[k].split()
                    g, left, top, w, h = int(g), int(left), int(top), int(w), int(h)
                    rows = out[k + 1:k + 1 + h]
                    k += 1 + h
                    polys = [flatten(c, size / upem, dx) for c in outlines[g]]
                    xs = [p[0] for poly in polys for p in poly]
                    ys = [p[1] for poly in polys for p in poly]
                    if xs and (min(xs) < left - 0.01 or max(xs) > left + w + 0.01 or
                               min(ys) < top - 0.01 or max(ys) > top + h + 0.01):
                        bad += 1
                        print('drawref: %s glyph %d at %d: outside its box' % (font.split('/')[-1], g, size))
                        continue
                    want = glyph_coverage(polys, left, top, w, h)
                    for y in range(h):
                        for x in range(w):
                            got = int(rows[y][2 * x:2 * x + 2], 16)
                            exact = int(want[y][x] * 255 + 0.5)
                            checked += 1
                            worst = max(worst, abs(got - exact))
                            if abs(got - exact) > 1:
                                bad += 1
                                if bad <= 5:
                                    print('drawref: %s glyph %d at %d (+%g) pixel (%d, %d): %d, exactly %d'
                                          % (font.split('/')[-1], g, size, dx, x, y, got, exact))
    if bad:
        print('drawref: glyphs: %d of %d pixels more than 1/255 from the exact area' % (bad, checked))
        return 1
    print('drawref: glyphs: %d fonts, %d pixels, all within 1/255 of the exact area (largest difference %d/255)'
          % (len(fonts), checked, worst))
    return 0


def main():
    if len(sys.argv) > 3 and sys.argv[1] == '--glyphs':
        return glyphs_main(sys.argv[2], sys.argv[3:])
    count = int(sys.argv[2]) if len(sys.argv) > 2 else 400
    all_cases = cases(count)
    lines = []
    for w, h, clip_box, shape in all_cases:
        lines.append('canvas %d %d' % (w, h))
        if clip_box:
            lines.append('clip %d %d %d %d' % clip_box)
        lines.append(text(shape))
        lines.append('dump')
    run = subprocess.run([sys.argv[1], 'read'], input='\n'.join(lines) + '\n', capture_output=True,
                         text=True)
    if run.returncode:
        print('drawref: drawtest read failed (%d): %s' % (run.returncode, run.stderr[:200]))
        return 1
    dumps = run.stdout.split('canvas ')[1:]
    if len(dumps) != len(all_cases):
        print('drawref: %d canvases back for %d shapes' % (len(dumps), len(all_cases)))
        return 1
    bad, pixels, worst = 0, 0, 0
    for (w, h, clip_box, shape), dump in zip(all_cases, dumps):
        rows = dump.split('\n')[1:1 + h]
        box = (0, 0, w, h)
        if clip_box:
            x, y, cw, ch = clip_box
            box = (max(0, x), max(0, y), min(w, x + cw), min(h, y + ch))
        # (the shape's coordinates as the C side read them: floats of 4 decimals)
        want, slack = expected(shape, w, h, box)
        for y in range(h):
            for x in range(w):
                got = int(rows[y][2 * x:2 * x + 2], 16)
                exact = want[y][x]
                pixels += 1
                worst = max(worst, abs(got - exact))
                if abs(got - exact) > slack:
                    bad += 1
                    if bad <= 5:
                        print('drawref: %s on %dx%d at (%d, %d): %d, exactly %d' % (text(shape), w, h, x, y,
                                                                                   got, exact))
    if bad:
        print('drawref: %d of %d pixels more than 1/255 from the exact area (a copied pattern: off at all)'
              % (bad, pixels))
        return 1
    print('drawref: %d shapes, %d pixels, all within 1/255 of the exact area (largest difference %d/255)'
          % (len(all_cases), pixels, worst))
    return 0


if __name__ == '__main__':
    sys.exit(main())
