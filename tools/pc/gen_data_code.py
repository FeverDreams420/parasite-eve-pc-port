#!/usr/bin/env python3
"""C for code the decomp keeps as data words.

A few routines of the main executable sit in its data section and the
decomp lists them only as words (`dlabel func_800E0060` in
asm/USA/main/data/main/dtail_0D0860.data.s); the game's C calls them by
address. This disassembles each range listed in pc/host/data_code_ranges.txt
from your retail executable, translates it with tools/pc/mips_to_c.py (one
group per range, so calls inside it share registers like the original), and
writes a table of entry points whose jumps the host installs at start-up.

Entry points: every `dlabel func_` inside the range, plus every jal target
inside it.

Not part of the build: the program runs these routines from the loaded
executable (pc/host/pe_host_datacode.c). The C made here is the reference
pc/tests/datacode_test.c checks that interpreter against.

  gen_data_code.py EXE OUT.c [--objdump mipsel-none-elf-objdump]
"""
import argparse
import os
import re
import struct
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mips_to_c  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
REGS = r'\b(zero|at|v[01]|a[0-3]|t[0-9]|s[0-8]|k[01]|gp|sp|fp|ra)\b'
BRANCH = ('b', 'beq', 'bne', 'beqz', 'bnez', 'blez', 'bgtz', 'bltz', 'bgez', 'j')


def read_ranges(path):
    out = []
    for line in open(path):
        line = line.split('#')[0].split()
        if len(line) >= 2:
            out.append((int(line[0], 0), int(line[1], 0)))
    return out


def dlabels():
    names = {}
    d = os.path.join(ROOT, 'asm/USA/main/data')
    for dirpath, _, files in os.walk(d):
        for f in files:
            for m in re.finditer(r'^dlabel func_([0-9A-Fa-f]{8})\b', open(os.path.join(dirpath, f)).read(), re.M):
                names[int(m.group(1), 16)] = 'func_' + m.group(1).upper()
    return names


def exe_bytes(exe, start, end):
    d = open(exe, 'rb').read()
    t_addr, t_size = struct.unpack_from('<II', d, 0x18)
    if not (t_addr <= start < end <= t_addr + t_size):
        raise SystemExit('range 0x%08X-0x%08X is outside the executable' % (start, end))
    return d[0x800 + start - t_addr:0x800 + end - t_addr]


def disassemble(objdump, data, base):
    with tempfile.NamedTemporaryFile(suffix='.bin', delete=False) as f:
        f.write(data)
        path = f.name
    try:
        txt = subprocess.check_output([objdump, '-D', '-b', 'binary', '-m', 'mips:3000', '-M', 'no-aliases', '-z',
                                       '--adjust-vma=0x%X' % base, path], text=True)
    finally:
        os.unlink(path)
    insns = []
    for line in txt.splitlines():
        m = re.match(r'^\s*([0-9a-f]+):\s+[0-9a-f]{8}\s+(\S+)\s*(.*)$', line)
        if m:
            insns.append((int(m.group(1), 16), m.group(2), m.group(3).strip()))
    if len(insns) != len(data) // 4:
        raise SystemExit('disassembly has %d instructions for %d words' % (len(insns), len(data) // 4))
    return insns


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('exe')
    ap.add_argument('out')
    ap.add_argument('--objdump', default='mipsel-none-elf-objdump')
    ap.add_argument('--ranges', default=os.path.join(ROOT, 'pc/host/data_code_ranges.txt'))
    args = ap.parse_args()
    known = dlabels()
    src = [mips_to_c.prelude(), '#include "pe_host_mem.h"\n']
    table = []
    for gi, (start, end) in enumerate(read_ranges(args.ranges)):
        insns = disassemble(args.objdump, exe_bytes(args.exe, start, end), start)
        entries = {a for a in known if start <= a < end}
        for addr, mn, ops in insns:
            if mn == 'jal':
                t = int(ops, 0)
                if start <= t < end:
                    entries.add(t)
        name = {a: 'pe_datacode_%08X' % a for a in entries}
        targets = set()
        lines = []
        for addr, mn, ops in insns:
            ops = re.sub(REGS, r'$\1', ops)
            if mn in BRANCH or mn == 'jal':
                parts = [p.strip() for p in ops.split(',')]
                t = int(parts[-1], 0)
                if mn == 'jal':
                    if t not in name:
                        raise SystemExit('jal 0x%08X leaves the range at 0x%08X' % (t, addr))
                    parts[-1] = name[t]
                else:
                    if not (start <= t < end):
                        raise SystemExit('%s 0x%08X leaves the range at 0x%08X' % (mn, t, addr))
                    parts[-1] = '.L%08X' % t
                    targets.add(t)
                ops = ', '.join(parts)
            if mn == 'c2':  # GTE command word (cop2 with a 25-bit immediate)
                lines.append((addr, '.word 0x%08X' % (0x4A000000 | int(ops, 0))))
                continue
            lines.append((addr, '%s %s' % (mn, ops) if ops else mn))
        funcs = []
        order = sorted(entries)
        for k, e in enumerate(order):
            stop = order[k + 1] if k + 1 < len(order) else end
            body = []
            for addr, text in lines:
                if e <= addr < stop:
                    if addr in targets:
                        body.append('.L%08X:' % addr)
                    body.append(text)
            funcs.append((name[e], '\n'.join(body)))
        # All entries of a range are one C function (mips_to_c group), so
        # branches between them and falling through from one into the next
        # behave as in the original.
        c, warns = mips_to_c.translate_group('datacode%d' % gi, funcs, stack_args=4)
        for w in warns:
            sys.stderr.write('warning: %s\n' % w)
        src.append(c)
        table += [(e, name[e]) for e in order]
    src.append('const PeHostFunc pe_data_code_table[] = {\n')
    src += ['    {0x%08Xu, (void *)%s},\n' % (a, n) for a, n in table]
    src.append('};\nconst unsigned int pe_data_code_count = %d;\n' % len(table))
    open(args.out, 'w').write(''.join(src))
    print('%d entry points in %s' % (len(table), args.out))


if __name__ == '__main__':
    main()
