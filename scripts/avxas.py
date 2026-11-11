"""
avxas.py ROOT WORK [AS] (32c): our assembler's SSE and AVX instructions against
clang's. Every entry of user/bin/as/x86_64.c's simd_table is written in its
forms (SSE and AVX, xmm and ymm, registers 0-15 where they change the
prefix, LLVM's operand swaps, memory with and without index, base r8-r15,
rip-relative, immediates), plus FMA, the compares by predicate name,
vmovd/vmovq, vmovss/vmovsd and vzeroupper; AS (build/host/as) and clang
assemble them all and the .text bytes are compared. On a difference, the
instructions are assembled one by one and the first 40 wrong ones printed.
"""
import re, struct, subprocess, sys, os

root, work = sys.argv[1], sys.argv[2]
as_path = sys.argv[3] if len(sys.argv) > 3 else os.path.join(root, 'build/host/as')
src = open(os.path.join(root, 'user/bin/as/x86_64.c')).read()
rows = re.findall(r'\{ "(\w+)", (0x[0-9A-F]+|0), (\d), (0x[0-9A-F]+), (F_\w+), ([^,]+), (\d+), (0x[0-9A-F]+|0) \}', src)
assert rows, 'no table'

gpr32 = ['eax', 'ecx', 'edx', 'ebx', 'esp', 'ebp', 'esi', 'edi'] + ['r%dd' % i for i in range(8, 16)]
gpr64 = ['rax', 'rcx', 'rdx', 'rbx', 'rsp', 'rbp', 'rsi', 'rdi'] + ['r%d' % i for i in range(8, 16)]
mems = ['(%rax)', '8(%rsp)', '-128(%rbp)', '0x1000(%r12,%r13,4)', '(%r8)', '64(%rax,%r9)', 'lbl(%rip)']
triples = [(0, 1, 2), (8, 1, 2), (1, 9, 2), (1, 2, 10), (8, 9, 10), (15, 3, 4), (3, 15, 4), (9, 0, 1), (12, 13, 5)]
pairs = [(0, 1), (8, 1), (1, 8), (15, 14), (9, 2), (5, 13)]

def vec(L, i):
    return '%%%smm%d' % ('y' if L else 'x', i)

lines = []
for name, pp, mp, op, form, flags, ext, store in rows:
    fl = set(f.strip() for f in flags.split('|')) - {'0'}
    modes = []
    if 'S_SSE' not in fl:
        modes.append(True)
    if 'S_VEX' not in fl:
        modes.append(False)
    for vex in modes:
        mn = name if ('S_VEX' in fl or not vex) else 'v' + name
        Ls = [0] if not vex else ([0] if 'S_128' in fl else [1] if 'S_256' in fl else [0, 1])
        imms = ['$0', '$0x5a'] if 'S_IMM' in fl else ['']
        if 'S_CMP' in fl:
            imms = ['$%d' % k for k in range(8 if not vex else 32)] if name[-2] == 'p' else ['$0', '$3', '$17']
        narrow = name in ('vcvtpd2ps', 'vcvtpd2dq', 'vcvttpd2dq')
        for L in Ls:
            def rmreg(i):
                if 'S_GPR' in fl:
                    return '%' + (gpr64 if 'S_W1' in fl else gpr32)[i]
                return vec(0 if 'S_HALF' in fl else L, i)
            def regreg(i):
                if 'S_GREG' in fl:
                    return '%' + gpr32[i]
                return vec(0 if narrow else L, i)
            for imm in imms:
                pre = [imm] if imm else []
                cases = []
                if form in ('F_RVM', 'F_RVMR', 'F_MVR') and vex:
                    for (a, b, c) in triples:
                        rms = ([] if 'S_MEM' in fl else [rmreg(a)]) + ([] if 'S_REG' in fl else [mems[a % len(mems)]])
                        for rm in rms:
                            if form == 'F_RVM':
                                cases.append(pre + [rm, vec(L, b), regreg(c)])
                            elif form == 'F_RVMR':
                                cases.append(pre + [vec(L, (a + 3) % 16), rm, vec(L, b), regreg(c)])
                            else:
                                cases.append(pre + [regreg(c), vec(L, b), rm])
                elif form == 'F_VMI':
                    for (a, b) in pairs:
                        cases.append(pre + [rmreg(a)] + ([vec(L, b)] if vex else []))
                else:
                    for (a, b) in pairs:
                        rms = ([] if 'S_MEM' in fl else [rmreg(a)]) + ([] if 'S_REG' in fl else [mems[a % len(mems)]])
                        for rm in rms:
                            if form == 'F_MR':
                                cases.append(pre + [regreg(b), rm])
                            else:
                                if name in ('pblendvb', 'blendvps', 'blendvpd'):
                                    cases.append(['%xmm0', rm, regreg(b)])
                                else:
                                    cases.append(pre + [rm, regreg(b)])
                                if store != '0' and not rm.startswith('%'):
                                    cases.append([regreg(b), rm])
                if form in ('F_RVMR', 'F_MVR') and not vex:
                    cases = []
                for c in cases:
                    lines.append('%s %s' % (mn, ', '.join(c)))
# compares by predicate name
for k, p in enumerate(['eq', 'lt', 'le', 'unord', 'neq', 'nlt', 'nle', 'ord', 'eq_uq', 'nge', 'ngt', 'false',
                       'neq_oq', 'ge', 'gt', 'true', 'eq_os', 'lt_oq', 'le_oq', 'unord_s', 'neq_us', 'nlt_uq',
                       'nle_uq', 'ord_s', 'eq_us', 'nge_uq', 'ngt_uq', 'false_os', 'neq_os', 'ge_oq', 'gt_oq',
                       'true_us']):
    for t in ('ps', 'pd', 'ss', 'sd'):
        if k < 8:
            lines.append('cmp%s%s %%xmm9, %%xmm3' % (p, t))
        lines.append('vcmp%s%s %%xmm9, %%xmm2, %%xmm3' % (p, t))
        if t[0] == 'p':
            lines.append('vcmp%s%s (%%rax), %%ymm2, %%ymm11' % (p, t))
# FMA
for base, packed_only in (('fmaddsub', True), ('fmsubadd', True), ('fmadd', False), ('fmsub', False),
                          ('fnmadd', False), ('fnmsub', False)):
    for order in ('132', '213', '231'):
        for t in ('ps', 'pd', 'ss', 'sd'):
            if packed_only and t[0] == 's':
                continue
            for L in ([0, 1] if t[0] == 'p' else [0]):
                for (a, b, c) in triples:
                    lines.append('v%s%s%s %s, %s, %s' % (base, order, t, vec(L, a), vec(L, b), vec(L, c)))
                lines.append('v%s%s%s %s, %s, %s' % (base, order, t, '8(%rdi,%r10,2)', vec(L, 4), vec(L, 9)))
# the rest
lines += ['vzeroupper', 'vzeroall']
for (a, b) in pairs:
    lines += ['vmovd %%%s, %%xmm%d' % (gpr32[a], b), 'vmovd %%xmm%d, %%%s' % (b, gpr32[a]),
              'vmovq %%%s, %%xmm%d' % (gpr64[a], b), 'vmovq %%xmm%d, %%%s' % (b, gpr64[a]),
              'vmovq %%xmm%d, %%xmm%d' % (a, b), 'vmovd %s, %%xmm%d' % (mems[a % 7], b),
              'vmovd %%xmm%d, %s' % (b, mems[a % 7]), 'vmovq %s, %%xmm%d' % (mems[a % 7], b),
              'vmovq %%xmm%d, %s' % (b, mems[a % 7])]
    for t in ('ss', 'sd'):
        lines += ['vmov%s %s, %%xmm%d' % (t, mems[a % 7], b), 'vmov%s %%xmm%d, %s' % (t, b, mems[a % 7])]
for (a, b, c) in triples:
    for t in ('ss', 'sd'):
        lines.append('vmov%s %%xmm%d, %%xmm%d, %%xmm%d' % (t, a, b, c))
# (32h) the conversions with an integer side
for (a, b, c) in triples:
    for t in ('ss', 'sd'):
        for sfx, regs in (('l', gpr32), ('q', gpr64)):
            lines.append('vcvtsi2%s%s %%%s, %%xmm%d, %%xmm%d' % (t, sfx, regs[a], b, c))
            lines.append('vcvtsi2%s%s %s, %%xmm%d, %%xmm%d' % (t, sfx, mems[a % 7], b, c))
            for op in ('cvtt', 'cvt'):
                lines.append('v%s%s2si%s %%xmm%d, %%%s' % (op, t, sfx, a, regs[c]))
                lines.append('v%s%s2si %s, %%%s' % (op, t, mems[b % 7], regs[c]))

text = '\t.text\n' + ''.join('\t%s\n' % l for l in lines) + 'lbl:\n\tnop\n'
open(os.path.join(work, 'all.s'), 'w').write(text)

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
    open(s, 'w').write('\t.text\n' + body + 'lbl:\n\tnop\n')
    a = subprocess.run([as_path, '-target', 'x86_64', '-o', s + '.ours.o', s],
                       capture_output=True, text=True)
    b = subprocess.run([os.path.join(root, 'tools/zig/zig'), 'cc', '-target', 'x86_64-freestanding', '-c',
                        '-mavx2', '-mfma', s, '-o', s + '.clang.o'], capture_output=True, text=True)
    return a, b, (text_of(s + '.ours.o') if a.returncode == 0 else None), \
        (text_of(s + '.clang.o') if b.returncode == 0 else None)

a, b, ours, clang = both(''.join('\t%s\n' % l for l in lines), 'all')
if a.returncode == 0 and b.returncode == 0 and ours == clang:
    print('avxas: the same bytes: %d instructions, %d bytes' % (len(lines), len(ours)))
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
            print('clang refuses: %s: %s' % (l, b.stderr.strip().splitlines()[0] if b.stderr else ''))
        elif a.returncode != 0:
            print('ours refuses: %s: %s' % (l, a.stderr.strip()))
        elif ours != clang:
            print('differs: %s: ours %s clang %s' % (l, ours.hex(), clang.hex()))
        else:
            continue
        bad += 1
        if bad >= 40:
            sys.exit(1)
print('avxas: %d instructions, %d wrong' % (len(lines), bad))
sys.exit(1 if bad else 0)
