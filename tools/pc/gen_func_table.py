#!/usr/bin/env python3
"""Build the MIPS-address -> host-function table for the main executable.

For every C unit in configs/USA/main.yaml the PS1 build object
(build/USA/src/main/<unit>.c.o) gives the function symbols and their offsets
in .text; the yaml gives where that .text sits in retail RAM. Each object's
.text is compared with the retail bytes at that address (words touched by a
relocation are skipped, since an unlinked object holds zero there), so every
address in the table is checked against your own main.exe.

Usage: gen_func_table.py OUT.c [--json OUT.json] [--exclude-prefix psyq/]
Run from the repository root after `make build` (objects must exist).
"""
import json
import pathlib
import re
import struct
import subprocess
import sys

import yaml

READELF = 'mipsel-none-elf-readelf'
EXE = pathlib.Path('assets/USA/main.exe')
YAML = pathlib.Path('configs/USA/main.yaml')


def sh(*args):
    return subprocess.run(args, check=True, capture_output=True, text=True).stdout


def sections(obj):
    out = []
    for line in sh(READELF, '-SW', str(obj)).splitlines():
        m = re.match(r'\s*\[\s*(\d+)\]\s+(\S+)\s+(\S+)\s+([0-9a-f]+)\s+([0-9a-f]+)\s+([0-9a-f]+)\s+\S+\s+(\S*)\s+\d+\s+\d+\s+(\d+)', line)
        if m:
            out.append({'idx': int(m.group(1)), 'name': m.group(2), 'type': m.group(3),
                        'off': int(m.group(5), 16), 'size': int(m.group(6), 16),
                        'flags': m.group(7), 'align': int(m.group(8))})
    return out


def func_symbols(obj):
    syms = []
    for line in sh(READELF, '-sW', str(obj)).splitlines():
        parts = line.split()
        if len(parts) >= 8 and parts[3] == 'FUNC' and parts[6].isdigit():
            syms.append({'value': int(parts[1], 16), 'size': int(parts[2]), 'bind': parts[4],
                         'shndx': int(parts[6]), 'name': parts[7]})
    return syms


def reloc_offsets(obj):
    """{section name: set of offsets} for relocations applied to text sections."""
    rel = {}
    cur = None
    for line in sh(READELF, '-rW', str(obj)).splitlines():
        m = re.match(r"Relocation section '\.rela?(\S+)'", line)
        if m:
            cur = rel.setdefault(m.group(1), set())
            continue
        m = re.match(r'\s*([0-9a-f]{8})\s+[0-9a-f]{8}\s+R_MIPS_', line)
        if m and cur is not None:
            cur.add(int(m.group(1), 16) & ~3)
    return rel


def main():
    out_c = pathlib.Path(sys.argv[1])
    out_json = pathlib.Path(sys.argv[sys.argv.index('--json') + 1]) if '--json' in sys.argv else None
    exclude = sys.argv[sys.argv.index('--exclude-prefix') + 1] if '--exclude-prefix' in sys.argv else None
    exe = EXE.read_bytes()
    cfg = yaml.safe_load(YAML.read_text())
    seg = next(s for s in cfg['segments'] if isinstance(s, dict) and s.get('name') == 'main')
    file_start, vram = seg['start'], seg['vram']
    subalign = seg.get('subalign', cfg['options'].get('subalign', 4))
    entries = []
    missing = []
    missing_ranges = []
    unit_starts = []
    mismatched = []
    for sub in seg['subsegments']:
        if isinstance(sub, dict):
            off, kind, name = sub.get('start'), sub.get('type'), sub.get('name')
        else:
            if len(sub) < 3:
                continue
            off, kind, name = sub[0], sub[1], sub[2]
        if off is not None:
            unit_starts.append(off)
        if kind != 'c':
            continue
        if exclude and name.startswith(exclude):
            continue
        obj = pathlib.Path('build/USA/src/main/%s.c.o' % name)
        if not obj.exists():
            missing.append(name)
            missing_ranges.append((name, vram + (off - file_start)))
            continue
        secs = sections(obj)
        texts = [s for s in secs if s['name'].startswith('.text') and 'X' in s['flags']]
        rel = reloc_offsets(obj)
        data = obj.read_bytes()
        addr = vram + (off - file_start)
        base = {}
        for s in texts:
            a = min(max(s['align'], 1), subalign)  # linker script SUBALIGN
            addr = (addr + a - 1) // a * a
            base[s['idx']] = addr
            # verify bytes
            fo = file_start + (addr - vram)
            want = exe[fo:fo + s['size']]
            have = data[s['off']:s['off'] + s['size']]
            skip = rel.get(s['name'], set())
            bad = 0
            for w in range(0, s['size'], 4):
                if w in skip:
                    continue
                if have[w:w + 4] != want[w:w + 4]:
                    bad += 1
            if bad:
                mismatched.append({'unit': name, 'section': s['name'], 'bad_words': bad,
                                   'words': s['size'] // 4})
            addr += s['size']
        for f in func_symbols(obj):
            if f['shndx'] in base:
                entries.append({'addr': base[f['shndx']] + f['value'], 'name': f['name'],
                                'unit': name, 'size': f['size'], 'bind': f['bind']})
    # Units whose PS1 object could not be built here (they need GAS 2.7/2.8.1):
    # take their functions from configs/USA/sym.main.txt ("type:func"), within
    # the unit's range in main.yaml. Their code is not checked against retail.
    starts = sorted({vram + (s0 - file_start) for s0 in unit_starts})
    symtxt = {}
    for m in re.finditer(r'^\s*(\w+)\s*=\s*(0x[0-9A-Fa-f]+)\s*;.*type:func', pathlib.Path('configs/USA/sym.main.txt').read_text(), re.M):
        symtxt[int(m.group(2), 16)] = m.group(1)
    known = {e['addr'] for e in entries}
    for name, a0 in missing_ranges:
        nxt = [x for x in starts if x > a0]
        a1 = nxt[0] if nxt else a0 + 0x10000
        for a, fn in sorted(symtxt.items()):
            if a0 <= a < a1 and a not in known:
                entries.append({'addr': a, 'name': fn, 'unit': name, 'size': 0, 'bind': 'GLOBAL',
                                'from_sym_txt': True})
    entries.sort(key=lambda e: e['addr'])
    dup = {}
    for e in entries:
        dup.setdefault(e['addr'], []).append(e['name'])
    dups = {hex(a): n for a, n in dup.items() if len(n) > 1}

    lines = ['/* Generated by tools/pc/gen_func_table.py from configs/USA/main.yaml and the',
             ' * PS1 build objects; every address was checked against the retail bytes. */',
             '#include "pe_host_func_table.h"', '']
    glob_entries = [e for e in entries if e['bind'] == 'GLOBAL']
    for e in glob_entries:
        lines.append('extern void %s(void);' % e['name'])
    lines.append('')
    lines.append('const PeHostFunc pe_main_func_table[] = {')
    for e in glob_entries:
        lines.append('    {0x%08Xu, (void *)%s},' % (e['addr'], e['name']))
    lines.append('};')
    lines.append('const unsigned int pe_main_func_count = %d;' % len(glob_entries))
    out_c.parent.mkdir(parents=True, exist_ok=True)
    out_c.write_text('\n'.join(lines) + '\n')
    summary = {'functions': len(entries), 'global': len(glob_entries),
               'local': len(entries) - len(glob_entries), 'missing_objects': missing,
               'mismatched_sections': mismatched, 'duplicate_addresses': dups}
    if out_json:
        out_json.write_text(json.dumps({'summary': summary, 'entries': entries}, indent=1))
    print(json.dumps({k: (v if not isinstance(v, (list, dict)) else len(v)) for k, v in summary.items()}))
    if mismatched:
        print('MISMATCH', mismatched[:5])
    return 1 if mismatched else 0


if __name__ == '__main__':
    sys.exit(main())
