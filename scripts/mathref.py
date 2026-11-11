#!/usr/bin/env python3
"""The exact values of math.h's functions (step 22s), from Python's decimal
at 120 digits (720 for reducing large angles; pi by Machin's formula), to
check that the programs' library gives the nearest double.

Usage: mathref.py < LINES, each "mathtest: exact NAME X Y RESULT" (hex
floats, as user/test/mathtest prints them with the argument exact); prints
one line per function: how many results were not the nearest double, and
the worst case in ulps.
"""
import sys, struct
from decimal import Decimal as D, getcontext
f = None
getcontext().prec = 120
def pi_digits(prec):
    getcontext().prec = prec + 10
    def atan_inv(n):
        x = D(1)/n; x2=x*x; t=x; s=D(0); k=1
        lim = D(10) ** -(prec + 5)
        while True:
            term=t/k
            if abs(term) < lim: break
            s += term if (k//2)%2==0 else -term
            t*=x2; k+=2
        return s
    return 16*atan_inv(5)-4*atan_inv(239)
PI = pi_digits(700)
getcontext().prec = 120
def sin_red(x):
    getcontext().prec = 720
    y = x % (2*PI)
    getcontext().prec = 120
    y = +y
    s=D(0); t=y; n=1; sign=1
    while t != 0 and (s == 0 or abs(t) > abs(s) * D(10)**-110):
        s += sign*t; t = t*y*y/((n+1)*(n+2)); n+=2; sign=-sign
    return s
def cos_red(x):
    getcontext().prec = 720
    y = (x + PI/2) % (2*PI)
    getcontext().prec = 120
    return sin_red(+y)
def atan(x):
    if x < 0: return -atan(-x)
    inv = x > 1
    if inv: x = 1/x
    for _ in range(4): x = x/(1+(1+x*x).sqrt())
    s=D(0); t=x; k=1; x2=x*x
    while t != 0 and (s == 0 or abs(t) > abs(s) * D(10)**-110):
        s += t/k if (k//2)%2==0 else -t/k; t*=x2; k+=2
    r = 16*s
    return (PI/2 - r) if inv else r
def ln(x): return x.ln()


# (22z) erf, erfc, lgamma, tgamma; fma, remainder, nextafter exactly.
from fractions import Fraction
import math as _m
_B = None
def bernoulli(n):
    b = [Fraction(1)]
    for m in range(1, n + 1):
        total, c = Fraction(0), 1
        for j in range(m):
            total += c * b[j]
            c = c * (m + 1 - j) // (j + 1)
        b.append(-total / (m + 1))
    return b
def erf_series(x):
    """erf(x) = 2/sqrt(pi) e^-x^2 sum 2^n x^(2n+1)/(2n+1)!! (all terms positive)."""
    x2 = x * x
    t = x; s = x; n = 1
    lim = D(10) ** -(getcontext().prec + 5)
    while True:
        t = t * 2 * x2 / (2 * n + 1)
        s += t
        n += 1
        if t < s * lim:
            break
    return 2 / PI.sqrt() * (-x2).exp() * s
def lgamma_pos(x):
    """ln Gamma(x), x > 0: Stirling's series at y = x + n >= 60."""
    global _B
    if _B is None:
        _B = bernoulli(80)
    y = x; prod = D(1)
    while y < 60:
        prod *= y; y += 1
    s = (y - D('0.5')) * y.ln() - y + (2 * PI).ln() / 2
    for k in range(1, 40):
        b = _B[2 * k]
        s += D(b.numerator) / D(b.denominator) / (2 * k * (2 * k - 1)) / y ** (2 * k - 1)
    return s - prod.ln()
def lgamma_ref(x):
    if x > 0:
        return lgamma_pos(x)
    s = sin_red(PI * x) if False else None
    getcontext().prec = 720
    r = x - 2 * (x / 2).to_integral_value()
    getcontext().prec = 120 + _EXTRA
    sp = sin_red(PI * r)
    getcontext().prec = 120 + _EXTRA
    return (PI / abs(sp)).ln() - lgamma_pos(1 - x)
_EXTRA = 0
def exp(x): return x.exp()
def ref(x, y):
    global _EXTRA
    if f in ('lgamma', 'tgamma') and x <= 0 and x == x.to_integral_value():
        raise ValueError('a pole')               # (inf or NaN there: nothing to compare)
    if f in ('erf', 'erfc', 'lgamma', 'tgamma'):
        _EXTRA = 0
        if f == 'erfc' and x > 0:
            _EXTRA = int(x * x / D('2.3')) + 10
        if f in ('lgamma', 'tgamma'):
            near = min(abs(x - 1), abs(x - 2), abs(x - x.to_integral_value()) if x < 0 else D(1))
            if near != 0 and near < 1:
                _EXTRA = int(-near.log10()) + 10
        getcontext().prec = 120 + _EXTRA
    if f == 'erf': return erf_series(abs(x)) * (1 if x >= 0 else -1) if abs(x) < 30 else D(1 if x > 0 else -1)
    if f == 'erfc': return 1 - erf_series(abs(x)) * (1 if x >= 0 else -1) if abs(x) < 30 else D(0 if x > 0 else 2)
    if f == 'lgamma': return lgamma_ref(x)
    if f == 'tgamma':
        l = lgamma_ref(x)
        if x > 0:
            return l.exp()
        getcontext().prec = 720
        r = x - 2 * (x / 2).to_integral_value()
        getcontext().prec = 120 + _EXTRA
        return l.exp() * (1 if sin_red(PI * r) > 0 else -1)
    if f == 'fma1': return D(float(Fraction(x) * Fraction(y) + Fraction(x)))
    if f == 'fma2':
        z = -(float(x) * float(y))
        return D(float(Fraction(x) * Fraction(y) + Fraction(z)))
    if f == 'remainder': return D(_m.remainder(float(x), float(y)))
    if f == 'nextafter': return D(_m.nextafter(float(x), float(y)))
    if f=='sin': return sin_red(x)
    if f=='cos': return cos_red(x)
    if f=='tan': return sin_red(x)/cos_red(x)
    if f=='asin': return atan(x/(1-x*x).sqrt()) if abs(x)<1 else (PI/2 if x>0 else -PI/2)
    if f=='acos': return PI/2 - (atan(x/(1-x*x).sqrt()) if abs(x)<1 else (PI/2 if x>0 else -PI/2))
    if f=='atan': return atan(x)
    if f=='atanh': return (ln(1+x)-ln(1-x))/2
    if f=='asinh': return (x - x*x*x/6) if abs(x) < D(10)**-30 else ln(abs(x)+(x*x+1).sqrt()) * (1 if x>=0 else -1)
    if f=='acosh': return ln(x+(x*x-1).sqrt())
    if f=='cbrt': return (abs(x) ** (D(1)/3)) * (1 if x>=0 else -1)
    if f=='sinh': return (exp(x)-exp(-x))/2
    if f=='cosh': return (exp(x)+exp(-x))/2
    if f=='tanh': return (exp(2*x)-1)/(exp(2*x)+1) if x < 200 else D(1) if x > 0 else D(-1)
    if f=='exp': return exp(x)
    if f=='expm1': return exp(x)-1
    if f=='exp2': return exp(x*ln(D(2)))
    if f=='log': return ln(x)
    if f=='log2': return ln(x)/ln(D(2))
    if f=='log10': return x.log10()
    if f=='log1p': return ln(1+x)
    if f=='pow': return exp(y*ln(x)) if x > 0 else D(0)
    if f=='hypot': return (x*x+y*y).sqrt()
    if f=='atan2':
        if x > 0: return atan(y/x)
        if x < 0: return atan(y/x) + (PI if y >= 0 else -PI)
        return PI/2 if y > 0 else -PI/2
def o(v):
    i=struct.unpack('<q',struct.pack('<d',v))[0]
    return i if i>=0 else -(2**63)-i
def to_double(d):
    try: return float(d)
    except OverflowError: return float('inf') if d > 0 else float('-inf')
if __name__ == '__main__':
    results = {}
    order = []
    for line in sys.stdin:
        words = line.split()
        if len(words) != 6 or words[1] != 'exact':
            continue
        f = words[2]
        if f not in results:
            results[f] = [0, 0, 0, '']
            order.append(f)
        xs = [float.fromhex(v) for v in words[3:]]
        x, y, r = D(xs[0]), D(xs[1]), xs[2]
        if r != r:
            continue
        getcontext().prec = 120
        try:
            e = to_double(ref(x, y))
        except Exception as exc:
            if __import__('os').environ.get('MATHREF_DEBUG'):
                print('mathref: skipped', f, words[3], words[4], repr(exc))
            continue
        u = abs(o(r) - o(e))
        entry = results[f]
        entry[0] += 1
        if u:
            entry[1] += 1
        if u > entry[2]:
            entry[2], entry[3] = u, f"{words[3]} {words[4]}: {words[5]}, exact {e.hex()}"
    for f in order:
        n, bad, worst, case = results[f]
        print(f"mathref: {f}: {n} values, {bad} not the nearest" + (f" (worst {worst} ulps: {case})" if bad else ""))
