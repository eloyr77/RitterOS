"""
neonas.py ROOT WORK [AS] (32f): our assembler's AArch64 vector (NEON)
instructions against clang's. Every form user/bin/as/aarch64.c's
neon_instruction takes is written here in its arrangements, with registers
0, 1, 15, 16 and 31 in each place, the ends of every immediate's range,
lanes, lists of 1 to 4 registers (also wrapping past v31) and the memory
forms; AS (build/host/as) and clang assemble them and the .text bytes are
compared. On a difference, the instructions are assembled one by one and
the first 40 wrong ones printed.
"""
import os, struct, subprocess, sys

root, work = sys.argv[1], sys.argv[2]
as_path = sys.argv[3] if len(sys.argv) > 3 else os.path.join(root, 'build/host/as')

ARR = ['8b', '16b', '4h', '8h', '2s', '4s', '1d', '2d']
ESZ = {'8b': 0, '16b': 0, '4h': 1, '8h': 1, '2s': 2, '4s': 2, '1d': 3, '2d': 3}
REGS = [(0, 1, 2), (31, 15, 16), (16, 31, 0), (1, 16, 31), (15, 0, 15)]
lines = []


def arrs(sizes, d1=False):
    """the arrangements of element sizes in the bit mask sizes (1d only if d1)"""
    return [a for a in ARR if sizes >> ESZ[a] & 1 and (a != '1d' or d1)]


three = [('add', 15), ('sub', 15), ('cmeq', 15), ('cmtst', 15), ('cmhi', 15), ('cmhs', 15), ('cmgt', 15),
         ('cmge', 15), ('umax', 7), ('umin', 7), ('smax', 7), ('smin', 7), ('uqadd', 15), ('sqadd', 15),
         ('uqsub', 15), ('sqsub', 15), ('addp', 15), ('umaxp', 7), ('uminp', 7), ('smaxp', 7), ('sminp', 7),
         ('mul', 7), ('uhadd', 7), ('urhadd', 7), ('ushl', 15), ('sshl', 15)]
for name, sizes in three:
    for a in arrs(sizes):
        for d, n, m in REGS:
            lines.append('%s v%d.%s, v%d.%s, v%d.%s' % (name, d, a, n, a, m, a))
for name in ('and', 'bic', 'orr', 'orn', 'eor', 'bsl', 'bit', 'bif'):
    for a in ('8b', '16b'):
        for d, n, m in REGS:
            lines.append('%s v%d.%s, v%d.%s, v%d.%s' % (name, d, a, n, a, m, a))
two = [('rev64', 7), ('rev32', 3), ('rev16', 1), ('cnt', 1), ('not', 1), ('mvn', 1), ('rbit', 1), ('cls', 7),
       ('clz', 7), ('abs', 15), ('neg', 15)]
for name, sizes in two:
    for a in arrs(sizes):
        for d, n, m in REGS:
            lines.append('%s v%d.%s, v%d.%s' % (name, d, a, n, a))
for name in ('cmgt', 'cmge', 'cmeq', 'cmle', 'cmlt'):
    for a in arrs(15):
        for d, n, m in REGS:
            lines.append('%s v%d.%s, v%d.%s, #0' % (name, d, a, n, a))
for name in ('xtn', 'uqxtn', 'sqxtn'):
    for lo, hi in (('8b', '8h'), ('4h', '4s'), ('2s', '2d')):
        up = {'8b': '16b', '4h': '8h', '2s': '4s'}[lo]
        for d, n, m in REGS:
            lines.append('%s v%d.%s, v%d.%s' % (name, d, lo, n, hi))
            lines.append('%s2 v%d.%s, v%d.%s' % (name, d, up, n, hi))
for name, longer in (('addv', 0), ('umaxv', 0), ('uminv', 0), ('smaxv', 0), ('sminv', 0), ('uaddlv', 1),
                     ('saddlv', 1)):
    for a in ('8b', '16b', '4h', '8h', '4s'):
        for d, n, m in REGS:
            lines.append('%s %s%d, v%d.%s' % (name, 'bhsd'[ESZ[a] + longer], d, n, a))
for name, kind in (('shl', 1), ('sli', 1), ('ushr', 0), ('sshr', 0), ('usra', 0), ('ssra', 0), ('sri', 0),
                   ('urshr', 0), ('srshr', 0)):
    for a in arrs(15):
        bits = 8 << ESZ[a]
        for sh in ((1, bits // 2, bits) if kind == 0 else (0, 1, bits - 1)):
            for d, n, m in REGS[:2]:
                lines.append('%s v%d.%s, v%d.%s, #%d' % (name, d, a, n, a, sh))
for name in ('shrn', 'rshrn'):
    for lo, hi in (('8b', '8h'), ('4h', '4s'), ('2s', '2d')):
        up = {'8b': '16b', '4h': '8h', '2s': '4s'}[lo]
        bits = 8 << ESZ[lo]
        for sh in (1, bits // 2, bits):
            for d, n, m in REGS[:2]:
                lines.append('%s v%d.%s, v%d.%s, #%d' % (name, d, lo, n, hi, sh))
                lines.append('%s2 v%d.%s, v%d.%s, #%d' % (name, d, up, n, hi, sh))
for name in ('ushll', 'sshll'):
    for lo, hi in (('8b', '8h'), ('4h', '4s'), ('2s', '2d')):
        up = {'8b': '16b', '4h': '8h', '2s': '4s'}[lo]
        bits = 8 << ESZ[lo]
        for sh in (0, 1, bits - 1):
            for d, n, m in REGS[:2]:
                lines.append('%s v%d.%s, v%d.%s, #%d' % (name, d, hi, n, lo, sh))
                lines.append('%s2 v%d.%s, v%d.%s, #%d' % (name, d, hi, n, up, sh))
        for d, n, m in REGS[:2]:
            lines.append('%sxtl v%d.%s, v%d.%s' % (name[0], d, hi, n, lo))
            lines.append('%sxtl2 v%d.%s, v%d.%s' % (name[0], d, hi, n, up))
for a in arrs(15):
    e = 'bhsd'[ESZ[a]]
    for d, n, m in REGS:
        lines.append('dup v%d.%s, %s%d' % (d, a, 'x' if e == 'd' else 'w', n if n != 31 else 30))
        lane = (16 >> ESZ[a]) - 1
        for l in (0, lane):
            lines.append('dup v%d.%s, v%d.%s[%d]' % (d, a, n, e, l))
for e in 'bhsd':
    top = (16 >> 'bhsd'.index(e)) - 1
    for d, n, m in REGS:
        r = 'x' if e == 'd' else 'w'
        for l in (0, top):
            lines.append('mov v%d.%s[%d], %s%d' % (d, e, l, r, n if n != 31 else 30))
            lines.append('ins v%d.%s[%d], %s%d' % (d, e, l, r, n if n != 31 else 30))
            lines.append('mov v%d.%s[%d], v%d.%s[%d]' % (d, e, l, n, e, top - l))
            lines.append('umov %s%d, v%d.%s[%d]' % (r, d if d != 31 else 30, n, e, l))
            if e in 'sd':
                lines.append('mov %s%d, v%d.%s[%d]' % (r, d if d != 31 else 30, n, e, l))
            if e in 'bh':
                lines.append('smov w%d, v%d.%s[%d]' % (d if d != 31 else 30, n, e, l))
            if e in 'bhs':
                lines.append('smov x%d, v%d.%s[%d]' % (d if d != 31 else 30, n, e, l))
for d, n, m in REGS:
    lines.append('mov v%d.16b, v%d.16b' % (d, n))
    lines.append('mov v%d.8b, v%d.8b' % (d, n))
    lines.append('fmov x%d, v%d.d[1]' % (d if d != 31 else 30, n))
    lines.append('fmov v%d.d[1], x%d' % (d, n if n != 31 else 30))
for d in (0, 31, 16):
    for v in (0, 1, 0x5a, 0xff):
        lines += ['movi v%d.8b, #%d' % (d, v), 'movi v%d.16b, #%d' % (d, v)]
        for a in ('4h', '8h'):
            for sh in (0, 8):
                lines.append('movi v%d.%s, #%d, lsl #%d' % (d, a, v, sh))
                lines.append('mvni v%d.%s, #%d, lsl #%d' % (d, a, v, sh))
        for a in ('2s', '4s'):
            for sh in (0, 8, 16, 24):
                lines.append('movi v%d.%s, #%d, lsl #%d' % (d, a, v, sh))
                lines.append('mvni v%d.%s, #%d, lsl #%d' % (d, a, v, sh))
    for v in ('0', '0xff', '0xff00ff00ff00ff00', '0xffffffffffffffff'):
        lines += ['movi d%d, #%s' % (d, v), 'movi v%d.2d, #%s' % (d, v)]
for d, n, m in REGS:
    for i in (0, 7):
        lines.append('ext v%d.8b, v%d.8b, v%d.8b, #%d' % (d, n, m, i))
    for i in (0, 1, 15):
        lines.append('ext v%d.16b, v%d.16b, v%d.16b, #%d' % (d, n, m, i))
for d, n, m in REGS:
    for k in range(1, 5):
        lst = ', '.join('v%d.16b' % ((n + j) % 32) for j in range(k))
        for a in ('8b', '16b'):
            lines.append('tbl v%d.%s, {%s}, v%d.%s' % (d, a, lst, m, a))
            lines.append('tbx v%d.%s, {%s}, v%d.%s' % (d, a, lst, m, a))
for name in ('uzp1', 'trn1', 'zip1', 'uzp2', 'trn2', 'zip2'):
    for a in arrs(15):
        for d, n, m in REGS[:3]:
            lines.append('%s v%d.%s, v%d.%s, v%d.%s' % (name, d, a, n, a, m, a))
bases = ['x0', 'x15', 'sp', 'x30']
for op in ('ld', 'st'):
    for a in ARR:
        size = 16 if a in ('16b', '8h', '4s', '2d') else 8
        for k in range(1, 5):
            for t in (0, 30, 31):
                lst = ', '.join('v%d.%s' % ((t + j) % 32, a) for j in range(k))
                for b in bases:
                    lines.append('%s1 {%s}, [%s]' % (op, lst, b))
                lines.append('%s1 {%s}, [x3], #%d' % (op, lst, k * size))
                lines.append('%s1 {%s}, [x3], x9' % (op, lst))
                if k > 1 and a != '1d':
                    lines.append('%s%d {%s}, [x5]' % (op, k, lst))
                    lines.append('%s%d {%s}, [x5], #%d' % (op, k, lst, k * size))
                    lines.append('%s%d {%s}, [sp], x2' % (op, k, lst))
        if op == 'ld':
            for k in range(1, 5):
                lst = ', '.join('v%d.%s' % ((7 + j) % 32, a) for j in range(k))
                lines.append('ld%dr {%s}, [x1]' % (k, lst))
                lines.append('ld%dr {%s}, [x1], #%d' % (k, lst, k << ESZ[a]))
                lines.append('ld%dr {%s}, [sp], x4' % (k, lst))
lines.append('ld1 {v0.16b-v3.16b}, [x0]')
lines.append('st1 {v30.4s - v1.4s}, [x2], #64')


def text_of(path):
    d = open(path, 'rb').read()
    shoff = struct.unpack_from('<Q', d, 0x28)[0]
    se, sn, ss = struct.unpack_from('<HHH', d, 0x3A)
    secs = [struct.unpack_from('<IIQQQQIIQQ', d, shoff + i * se) for i in range(sn)]
    st = secs[ss]
    for s in secs:
        if d[st[4] + s[0]:].split(b'\0')[0] == b'.text':
            return d[s[4]:s[4] + s[5]]


def both(body, tag):
    s = os.path.join(work, tag + '.s')
    open(s, 'w').write('\t.text\n' + body)
    a = subprocess.run([as_path, '-target', 'aarch64', '-o', s + '.ours.o', s], capture_output=True, text=True)
    b = subprocess.run([os.path.join(root, 'tools/zig/zig'), 'clang', '-target', 'aarch64-unknown-none-elf', '-c',
                        s, '-o', s + '.clang.o'], capture_output=True, text=True)
    return a, b, (text_of(s + '.ours.o') if a.returncode == 0 else None), \
        (text_of(s + '.clang.o') if b.returncode == 0 else None)


a, b, ours, clang = both(''.join('\t%s\n' % l for l in lines), 'all')
if a.returncode == 0 and b.returncode == 0 and ours == clang:
    print('neonas: the same bytes: %d instructions, %d bytes' % (len(lines), len(ours)))
    sys.exit(0)
bad = 0
for i in range(0, len(lines), 64):
    chunk = lines[i:i + 64]
    a, b, ours, clang = both(''.join('\t%s\n' % l for l in chunk), 'chunk')
    if a.returncode == 0 and b.returncode == 0 and ours == clang:
        continue
    for l in chunk:
        a, b, ours, clang = both('\t%s\n' % l, 'one')
        if b.returncode != 0:
            print('clang refuses: %s: %s' % (l, [x for x in b.stderr.splitlines() if 'error' in x][:1]))
        elif a.returncode != 0:
            print('ours refuses: %s: %s' % (l, a.stderr.strip()))
        elif ours != clang:
            print('differs: %s: ours %s clang %s' % (l, ours.hex(), clang.hex()))
        else:
            continue
        bad += 1
        if bad >= 40:
            sys.exit(1)
print('neonas: %d instructions, %d wrong' % (len(lines), bad))
sys.exit(1 if bad else 0)
