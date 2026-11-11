"""
cctest_simd.py (32d): writes user/test/cctest_simd.h, cctest's intrinsics
part, from user/bin/cc/include/immintrin.h: every intrinsic there called on
fixed vectors (and every immediate in a few values), each result printed in
hex. cctest is built by clang (with its own <immintrin.h>) and by RitterOS's
cc (with ours), so the two must print the same lines. Run it again after
changing the header:

    python3 scripts/cctest_simd.py
"""
import os, re

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
header = open(os.path.join(root, 'user/bin/cc/include/immintrin.h')).read()

funcs = re.findall(r'^__RI ([\w ]+?) (_mm\w+)\(([^)]*)\) \{', header, re.M)
macros = re.findall(r'^#define (_mm\w+)\(([\w, ]+)\) __extension__', header, re.M)
aliases = re.findall(r'^#define (_mm\w+)\(([\w, ]+)\) (_mm\w+)\(', header, re.M)

vec = {'__m256i': 'i256', '__m256': 'f256', '__m256d': 'd256', '__m128i': 'i128', '__m128': 'f128', '__m128d': 'd128'}
skip = {'_mm256_undefined_si256', '_mm256_undefined_ps', '_mm256_undefined_pd', '_mm_undefined_si128'}
calls = []


def value(ty, k, name, pos):
    ty = ty.replace('const ', '').strip()
    if ty in vec:
        return '%s[%d]' % (vec[ty], k % 3)
    if ty == 'char':
        return '(char)%d' % ((37 * pos + 11 * k) % 256 - 128)
    if ty == 'short':
        return '(short)%d' % ((4099 * pos + 7 * k) % 65536 - 32768)
    if ty == 'int':
        return '%d' % ((100003 * pos + 17 * k) % 200001 - 100000)
    if ty == 'long long':
        return '%dLL' % ((1000000007 * pos + 13 * k) % 4000000001 - 2000000000)
    if ty == 'float':
        return '%.3ff' % (pos * 1.25 - 3 + k)
    if ty == 'double':
        return '%.3f' % (pos * 2.5 - 7 + k)
    raise SystemExit('no value for %s in %s' % (ty, name))


undefined_upper = ('_mm256_castsi128_si256', '_mm256_castps128_ps256', '_mm256_castpd128_pd256')


def show(ret, expr, label):
    ret = ret.strip()
    if ret in vec:
        size = '16' if label.split('#')[0] in undefined_upper else 'sizeof(__v)'   # (the rest undefined)
        return '    { %s __v = %s; show("%s", &__v, %s); }' % (ret, expr, label, size)
    if ret in ('int', 'long long'):
        return '    printf("intrinsics: %s: %%lld\\n", (long long)(%s));' % (label, expr)
    if ret == 'float':
        return '    { float __f = %s; show("%s", &__f, 4); }' % (expr, label)
    if ret == 'double':
        return '    { double __f = %s; show("%s", &__f, 8); }' % (expr, label)
    raise SystemExit('no way to show %s' % ret)


for ret, name, params in funcs:
    if name in skip:
        continue
    ps = [p.strip() for p in params.split(',')] if params.strip() not in ('', 'void') else []
    types = [re.sub(r'\s*\*?\s*__\w+$', '', p) + (' *' if '*' in p else '') for p in ps]
    if ret.strip() == 'void' and not any('*' in t for t in types):
        calls.append('    %s();' % name)                                 # (zeroupper, fences)
        continue
    if any('*' in t for t in types):
        # loads and stores: through the aligned buffers, a vector's worth
        base = types[0].replace('const ', '').replace('*', '').strip()
        args = []
        for i, t in enumerate(types):
            if '*' in t:
                args.append('(%s *)buffer' % base)
            else:
                args.append(value(t, i, name, i + 1))
        if ret.strip() == 'void':
            calls.append('    fill(); %s(%s); show("%s", buffer, 64);' % (name, ', '.join(args), name))
        else:
            calls.append('    fill();')
            calls.append(show(ret, '%s(%s)' % (name, ', '.join(args)), name))
        continue
    shifts = name.endswith(('slli_epi16', 'slli_epi32', 'slli_epi64', 'srli_epi16', 'srli_epi32', 'srli_epi64',
                            'srai_epi16', 'srai_epi32'))
    rounds = [(3, 31, 33)] if shifts else [None]
    for k in range(2 if len(types) and all(t.replace('const ', '').strip() in vec for t in types) else 1):
        for counts in rounds:
            for c in (counts or [None]):
                args = []
                for i, t in enumerate(types):
                    if shifts and i == 1:
                        args.append(str(c))
                    elif t.startswith('const int'):
                        args.append('%d' % (1 - k))                 # (an index valid for every width)
                    else:
                        args.append(value(t, k + i, name, i + 1))
                label = '%s#%d%s' % (name, k, '' if c is None else '/%d' % c)
                calls.append(show(ret, '%s(%s)' % (name, ', '.join(args)), label))

returns = {}
for ret, name, params in funcs:
    returns[name] = ret.strip()


def macro_type(name):
    if '_ps' in name or name.endswith('_ps') or 'dp_ps' in name:
        return '__m256' if name.startswith('_mm256') else '__m128'
    if '_pd' in name:
        return '__m256d' if name.startswith('_mm256') else '__m128d'
    return '__m256i' if name.startswith('_mm256') else '__m128i'


for name, params in macros:
    n = len(params.split(','))
    T = macro_type(name)
    if 'cmp_' in name:
        imms = [0, 5, 17, 30]
    elif 'round_' in name:
        imms = [1, 2, 3, 4]
    elif 'permute2' in name:
        imms = [0x21, 0x13, 0x08]
    else:
        imms = [3, 0x1b, 0x93]
    # the immediates' ranges clang checks
    if name in ('_mm_blend_pd', '_mm_permute_pd', '_mm_shuffle_pd'):
        mask = 3
    elif name in ('_mm256_blend_pd', '_mm256_permute_pd', '_mm256_shuffle_pd', '_mm_blend_ps', '_mm_blend_epi32') \
            or 'round_' in name:
        mask = 15
    elif 'cmp_' in name:
        mask = 31
    else:
        mask = 255
    for imm in sorted(set(i & mask for i in imms)):
        args = [value(T, i, name, i + 1) for i in range(n - 1)] + ['0x%x' % imm]
        calls.append(show(T, '%s(%s)' % (name, ', '.join(args)), '%s/0x%x' % (name, imm)))
for name, params, target in aliases:
    n = len(params.split(','))
    T = macro_type(name)
    imm = 'imm' in params
    args = [value(T, i, name, i + 1) for i in range(n - 1 if imm else n)] + (['3'] if imm else [])
    calls.append(show(T, '%s(%s)' % (name, ', '.join(args)), name))

text = '''/*
 * cctest's intrinsics part (32d), written by scripts/cctest_simd.py from
 * user/bin/cc/include/immintrin.h (do not edit: run it again): every
 * intrinsic on fixed vectors, each result in hex. clang's build and cc's
 * must print the same; on a processor without AVX2 and FMA both say so.
 */
#if defined(__x86_64__)
#include <immintrin.h>

#define SIMD __attribute__((target("avx2,fma")))

static __m256i i256[3];
static __m256 f256[3];
static __m256d d256[3];
static __m128i i128[3];
static __m128 f128[3];
static __m128d d128[3];
static unsigned char buffer[96] __attribute__((aligned(32)));

static void show(const char *name, const void *p, int n)
{
    const unsigned char *b = p;
    printf("intrinsics: %s:", name);
    for (int i = 0; i < n; i++)
        printf("%s%02x", i % 8 ? "" : " ", b[i]);
    printf("\\n");
}

static void fill(void)
{
    for (int i = 0; i < (int)sizeof(buffer); i++)
        buffer[i] = (unsigned char)(i * 37 + 5);
}

SIMD static void simd_values(void)
{
    static const float fs[8] = { 1.5f, -2.25f, 1e30f, 0.0f, -0.0f, 3.0f, 0.1f, -7.75f };
    static const double ds[4] = { 2.5, -1e-300, 0.75, -3.0 };
    for (int k = 0; k < 3; k++) {
        unsigned char raw[32];
        float f[8];
        double d[4];
        for (int i = 0; i < 32; i++)
            raw[i] = (unsigned char)(i * 29 + k * 101 + (i & 1 ? 0x80 : 3));
        for (int i = 0; i < 8; i++)
            f[i] = fs[(i + k) % 8] * (float)(k + 1);
        for (int i = 0; i < 4; i++)
            d[i] = ds[(i + k) % 4] * (k + 1);
        memcpy(&i256[k], raw, 32);
        memcpy(&i128[k], raw + 8, 16);
        memcpy(&f256[k], f, 32);
        memcpy(&f128[k], f + 2, 16);
        memcpy(&d256[k], d, 32);
        memcpy(&d128[k], d + 1, 16);
    }
}

SIMD static void simd_calls(void)
{
''' + '\n'.join(calls) + '''
}

static int simd_supported(void)
{
    unsigned a, b, c, d, lo, hi;
    __asm__("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
    if (!(c >> 27 & 1) || !(c >> 28 & 1) || !(c >> 12 & 1))     /* OSXSAVE, AVX, FMA */
        return 0;
    __asm__("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
    if ((lo & 6) != 6)
        return 0;
    __asm__("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(7), "c"(0));
    return b >> 5 & 1;                                          /* AVX2 */
}

static void intrinsics(void)
{
    if (!simd_supported()) {
        printf("intrinsics: no AVX2 and FMA here\\n");
        return;
    }
    simd_values();
    simd_calls();
    printf("intrinsics: %d calls\\n", @COUNT@);
}
#else
static void intrinsics(void)
{
    printf("intrinsics: x86-64's\\n");
}
#endif
'''.replace('@COUNT@', str(len(calls)))
open(os.path.join(root, 'user/test/cctest_simd.h'), 'w').write(text)
print('cctest_simd.h: %d calls' % len(calls))
