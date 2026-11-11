#!/usr/bin/env python3
"""The exact values of complex.h's functions (step 22x), from Python's
decimal, to check the programs' library: each part's distance in ulps from
the exact value (for cpow, the distance of the result from the exact one in
ulps of its larger part, divided by 1 + |w log z|: the rounding of w log z
alone moves the result that much). Digits: 150, more for tiny arguments (catanh of 1e-300 needs
300 more); pi from mathref.py (Machin's formula). Signed zeros are kept
(decimal has them), so the branch cuts are C's.

Usage: complexref.py LIMIT [NAME=LIMIT]... < LINES, each "complextest:
exact NAME X Y U V RE IM" (hex floats); prints one line per function:
values, how many were over its limit in ulps, and the worst case.
"""
import sys, struct
from decimal import Decimal as D, getcontext, localcontext
import os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mathref  # noqa: E402

PI = mathref.PI
P = 150


def pos(v):
    return +v


def sin_(x):
    with localcontext() as c:
        c.prec = P + 40
        y = x % (2 * PI)
        s, t, n = D(0), y, 1
        lim = D(10) ** -(P + 20)
        while abs(t) > lim:
            s += t
            t = -t * y * y / ((n + 1) * (n + 2))
            n += 2
    return pos(s)


def cos_(x):
    with localcontext() as c:
        c.prec = P + 40
        return sin_(x + PI / 2)


def sinh_(x):
    with localcontext() as c:
        c.prec = P + 40
        if abs(x) < D('1e-3'):
            s, t, n = D(0), x, 1
            while t != 0 and abs(t) > D(10) ** -(P + 20) * abs(x):
                s += t
                t = t * x * x / ((n + 1) * (n + 2))
                n += 2
            return pos(s)
        return pos((x.exp() - (-x).exp()) / 2)


def cosh_(x):
    with localcontext() as c:
        c.prec = P + 40
        return pos((x.exp() + (-x).exp()) / 2)


def atan_(x):
    with localcontext() as c:
        c.prec = P + 40
        if x < 0:
            return -atan_(-x)
        inv = x > 1
        if inv:
            x = 1 / x
        for _ in range(6):
            x = x / (1 + (1 + x * x).sqrt())
        s, t, k, x2 = D(0), x, 1, x * x
        while t != 0 and (s == 0 or abs(t) > abs(s) * D(10) ** -(P + 20)):
            s += t / k if (k // 2) % 2 == 0 else -t / k
            t *= x2
            k += 2
        r = 64 * s
        return pos(PI / 2 - r if inv else r)


def atan2_(y, x):
    """C's atan2, signed zeros included."""
    if x.is_zero() and y.is_zero():
        r = PI if x.is_signed() else D(0)
        return -r if y.is_signed() else r
    if x > 0:
        return atan_(y / x) if not y.is_zero() else y
    if x < 0:
        r = atan_(abs(y) / -x)
        r = PI - r
        return -r if y.is_signed() else r
    return -PI / 2 if y.is_signed() else PI / 2


# Complex numbers as (re, im) pairs of decimals. Signs of zeros matter
# (they choose a cut's side): decimal's -0 is +0 (its context rounds the
# negation), so negation is copy_negate, and a real number added to or
# taken from z leaves z's imaginary part alone, as C's z + 1 and 1 - z.
def neg(v): return v.copy_negate()
def add(a, b): return (a[0] + b[0], a[1] + b[1])
def mul(a, b): return (a[0] * b[0] - a[1] * b[1], a[0] * b[1] + a[1] * b[0])
def plus_real(z, r): return (z[0] + r, z[1])
def real_minus(r, z): return (r - z[0], neg(z[1]))
def times_i(a): return (neg(a[1]), a[0])          # i a
def times_minus_i(a): return (a[1], neg(a[0]))    # -i a


def csqrt_(z):
    x, y = z
    if x.is_zero() and y.is_zero():
        return (D(0), y)
    t = ((abs(x) + (x * x + y * y).sqrt()) / 2).sqrt()
    if not x.is_signed():
        return (t, y / (2 * t))
    return (abs(y) / (2 * t), -t if y.is_signed() else t)


def clog_(z):
    x, y = z
    return ((x * x + y * y).ln() / 2, atan2_(y, x))


def cexp_(z):
    x, y = z
    e = x.exp()
    if y.is_zero():
        return (e, y)
    return (e * cos_(y), e * sin_(y))


def casinh_(z):
    x, y = z
    if x.is_signed():                         # (odd: the cut's sides by x's sign)
        r = casinh_((neg(x), neg(y)))
        return (neg(r[0]), neg(r[1]))
    return clog_(add(z, csqrt_(plus_real(mul(z, z), D(1)))))


def catanh_(z):
    a, b = clog_(plus_real(z, D(1))), clog_(real_minus(D(1), z))
    return ((a[0] - b[0]) / 2, (a[1] - b[1]) / 2)


def ref(f, x, y, u, v):
    z = (x, y)
    if f == 'cexp': return cexp_(z)
    if f == 'clog': return clog_(z)
    if f == 'csqrt': return csqrt_(z)
    if f == 'csinh': return (sinh_(x) * cos_(y), cosh_(x) * sin_(y))
    if f == 'ccosh': return (cosh_(x) * cos_(y), sinh_(x) * sin_(y))
    if f == 'csin': return (sin_(x) * cosh_(y), cos_(x) * sinh_(y))
    if f == 'ccos': return (cos_(x) * cosh_(y), neg(sin_(x) * sinh_(y)))
    if f == 'ctanh':
        d = cosh_(2 * x) + cos_(2 * y)
        return (sinh_(2 * x) / d, sin_(2 * y) / d)
    if f == 'ctan':
        d = cos_(2 * x) + cosh_(2 * y)
        return (sin_(2 * x) / d, sinh_(2 * y) / d)
    if f == 'casinh': return casinh_(z)
    if f == 'casin': return times_minus_i(casinh_(times_i(z)))
    if f == 'cacos':
        s = times_minus_i(casinh_(times_i(z)))
        return (PI / 2 - s[0], neg(s[1]))
    if f == 'cacosh':
        return clog_(add(z, mul(csqrt_(plus_real(z, D(1))), csqrt_(plus_real(z, D(-1))))))
    if f == 'catanh': return catanh_(z)
    if f == 'catan': return times_minus_i(catanh_(times_i(z)))
    if f == 'cpow':
        if x.is_zero() and y.is_zero():
            return (D(0), D(0))
        return cexp_(mul((u, v), clog_(z)))


def growth(f, x, y, u, v):
    """cpow: 1 + |w log z|, the result's own sensitivity to rounding."""
    if f != 'cpow' or (x.is_zero() and y.is_zero()):
        return 1
    lz = clog_((x, y))
    p = mul((u, v), lz)
    return 1 + float((p[0] * p[0] + p[1] * p[1]).sqrt())


def to_double(d):
    try:
        return float(d)
    except OverflowError:
        return float('inf') if d > 0 else float('-inf')


def ulp(v):
    v = abs(v)
    if v < 2.0 ** -1022:
        return 2.0 ** -1074
    m, e = __import__('math').frexp(v)
    return 2.0 ** (e - 53)


def main():
    global P
    limit = float(sys.argv[1]) if len(sys.argv) > 1 else 4
    limits = dict((a.split('=')[0], float(a.split('=')[1])) for a in sys.argv[2:])
    results, order = {}, []
    for line in sys.stdin:
        words = line.split()
        if len(words) != 9 or words[1] != 'exact':
            continue
        f = words[2]
        if f not in results:
            results[f] = [0, 0.0, '', 0]
            order.append(f)
        xs = [float.fromhex(w) for w in words[3:]]
        x, y, u, v = (D(t) for t in xs[:4])
        re, im = xs[4], xs[5]
        if re != re or im != im or abs(re) == float('inf') or abs(im) == float('inf'):
            continue
        small = min([abs(t) for t in xs[:4] if t != 0] or [1.0])
        P = 150 + (int(-__import__('math').log10(small)) + 10 if small < 1 else 0)
        getcontext().prec = P
        try:
            er, ei = ref(f, x, y, u, v)
        except Exception:
            continue
        big = max(abs(er), abs(ei))              # (our 0 where the exact part is 0 and
        if re == 0 and abs(er) < big * D(10) ** -(P - 30):     # decimal left digits)
            er = D(0)
        if im == 0 and abs(ei) < big * D(10) ** -(P - 30):
            ei = D(0)
        dr, di = to_double(er), to_double(ei)
        if abs(dr) == float('inf') or abs(di) == float('inf'):
            continue
        if f == 'cpow':
            unit = ulp(max(abs(dr), abs(di)))
            dist = max(abs(D(re) - er), abs(D(im) - ei)) / D(unit) / D(growth(f, x, y, u, v))
        else:
            dist = max(abs(D(re) - er) / D(ulp(dr)), abs(D(im) - ei) / D(ulp(di)))
        dist = float(dist)
        entry = results[f]
        entry[0] += 1
        if dist > entry[1]:
            entry[1] = dist
            entry[2] = f"{words[3]} {words[4]}" + (f" ^ {words[5]} {words[6]}" if f == 'cpow' else '') + \
                f": {words[7]} {words[8]}, exact {dr.hex()} {di.hex()}"
        if dist > limits.get(f, limit):
            entry[3] += 1
    for f in order:
        n, worst, case, over = results[f]
        print(f"complexref: {f}: {n} values, {over} over {limits.get(f, limit):g} ulps"
              + (f" (worst {worst:.3g} ulps: {case})" if over else ''))


if __name__ == '__main__':
    main()
