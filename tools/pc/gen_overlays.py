#!/usr/bin/env python3
"""Generate the host link inputs for every overlay (rooms, scenes, effects, menus).

An overlay is code and data the game copies from PE.IMG into a fixed RAM
window (most rooms and scenes share 0x8018EFE8) and then calls at fixed
addresses. Many overlays reuse the same C (src/overlays/room_lib, ...), each
compiled at its own addresses. On the host every overlay gets its own copy
of its functions, renamed pe_ovl_<overlay>__<name>, and its own data names,
pe_ovl_<overlay>__<name> = retail address. When the game has loaded an
overlay the runtime identifies it by SHA-1 and writes jumps at its function
addresses to that overlay's host functions.

Function addresses come from configs/USA/overlays/<overlay>.yaml plus the PS1
objects of its C units (build/USA-ovl-objs, compiled with
tools/scripts/cc.sh), and are cross-checked against the "type:func" entries
of configs/USA/overlays/sym.<overlay>.txt. (Checking against the retail
overlay bytes needs PE.IMG from the disc and is done separately.)

Usage: gen_overlays.py OUT_DIR --host-objs DIR [--nm NM] [--sym-prefix P]
"""
import json
import os
import pathlib
import re
import subprocess
import sys

import yaml

READELF = 'mipsel-none-elf-readelf'
NM = 'nm'
PREFIX = ''


def sh(*a):
    return subprocess.run(a, check=True, capture_output=True, text=True).stdout


def unprefix(n):
    return n[len(PREFIX):] if PREFIX and n.startswith(PREFIX) else n


def ps1_sections(obj):
    out = []
    for line in sh(READELF, '-SW', str(obj)).splitlines():
        m = re.match(r'\s*\[\s*(\d+)\]\s+(\S+)\s+(\S+)\s+([0-9a-f]+)\s+([0-9a-f]+)\s+([0-9a-f]+)\s+\S+\s+(\S*)\s+\d+\s+\d+\s+(\d+)', line)
        if m:
            out.append({'idx': int(m.group(1)), 'name': m.group(2), 'size': int(m.group(6), 16),
                        'flags': m.group(7), 'align': int(m.group(8))})
    return out


def ps1_symbols(obj):
    syms = []
    for line in sh(READELF, '-sW', str(obj)).splitlines():
        p = line.split()
        if len(p) >= 8 and p[6].isdigit():
            syms.append({'value': int(p[1], 16), 'type': p[3], 'bind': p[4], 'shndx': int(p[6]), 'name': p[7]})
    return syms


def host_syms(obj):
    """(defined {name: type}, undefined set) of a host object."""
    d, u = {}, set()
    for line in sh(NM, str(obj)).splitlines():
        p = line.split()
        if len(p) == 2 and p[0] == 'U':
            u.add(unprefix(p[1]))
        elif len(p) == 3:
            d[unprefix(p[2])] = p[1]
    return d, u


def read_sym_txt(path):
    out = {}
    if path.exists():
        for m in re.finditer(r'^\s*([A-Za-z_.$][\w.$]*)\s*=\s*(0x[0-9A-Fa-f]+)\s*;(.*)$', path.read_text(), re.M):
            out[m.group(1)] = (int(m.group(2), 16), 'type:func' in m.group(3))
    return out


def safe(n):
    return re.sub(r'[^A-Za-z0-9_]', '_', n)


def main():
    global NM, PREFIX
    out = pathlib.Path(sys.argv[1])
    host_dir = pathlib.Path(sys.argv[sys.argv.index('--host-objs') + 1])
    if '--nm' in sys.argv:
        NM = sys.argv[sys.argv.index('--nm') + 1]
    if '--sym-prefix' in sys.argv:
        PREFIX = sys.argv[sys.argv.index('--sym-prefix') + 1]
    out.mkdir(parents=True, exist_ok=True)

    main_ft = json.load(open('build/pc-tables/func_table_all.json'))
    main_funcs = {e['name'] for e in main_ft['entries']}
    main_abs = json.load(open('build/pc-tables/pe_abs_syms.json'))

    overlays = []
    all_funcs = {}  # name -> list of (overlay, addr)
    for cfgp in sorted(pathlib.Path('configs/USA/overlays').glob('*.yaml')):
        cfg = yaml.safe_load(cfgp.read_text())
        opt = cfg['options']
        name = opt['basename']
        src_path = opt['src_path']
        segs = [s for s in cfg['segments'] if isinstance(s, dict) and 'subsegments' in s]
        ends = [s[0] for s in cfg['segments'] if isinstance(s, list) and s]
        units, funcs, cdata = [], [], {}
        problems = []
        for seg in segs:
            fs, vram = seg.get('start', 0), seg['vram']
            subalign = seg.get('subalign', opt.get('subalign', 4))
            for sub in seg['subsegments']:
                if isinstance(sub, dict):
                    off, kind, uname = sub.get('start'), sub.get('type'), sub.get('name')
                elif len(sub) >= 3:
                    off, kind, uname = sub[0], sub[1], sub[2]
                else:
                    continue
                if kind not in ('c', '.data', '.rodata'):
                    continue
                src = os.path.normpath(os.path.join(src_path, uname)) + '.c'
                obj = pathlib.Path('build/USA-ovl-objs') / (src + '.o')
                if not obj.exists():
                    problems.append('no PS1 object for ' + src)
                    continue
                secs = ps1_sections(obj)
                syms = ps1_symbols(obj)
                want = {'c': '.text', '.data': '.data', '.rodata': '.rodata'}[kind]
                addr = vram + (off - fs)
                base = {}
                for s in secs:
                    if s['name'] == want or s['name'].startswith(want + '.'):
                        a = min(max(s['align'], 1), subalign)
                        addr = (addr + a - 1) // a * a
                        base[s['idx']] = addr
                        addr += s['size']
                for s in syms:
                    if s['shndx'] not in base:
                        continue
                    a = base[s['shndx']] + s['value']
                    if kind == 'c' and s['type'] == 'FUNC':
                        funcs.append({'name': s['name'], 'addr': a, 'bind': s['bind'], 'src': src})
                    elif kind != 'c' and s['type'] in ('OBJECT', 'NOTYPE') and s['bind'] == 'GLOBAL':
                        cdata[s['name']] = a
                if kind == 'c':
                    units.append(src)
        size = max(ends) if ends else 0
        symtxt = read_sym_txt(pathlib.Path('configs/USA/overlays/sym.%s.txt' % name))
        mism = []
        for f in funcs:
            if f['name'] in symtxt and symtxt[f['name']][1] and symtxt[f['name']][0] != f['addr']:
                mism.append('%s computed 0x%08X, sym.txt 0x%08X' % (f['name'], f['addr'], symtxt[f['name']][0]))
        m = re.search(r'extract locally with: dd if=\S*PE\.IMG \S+ bs=(\d+) skip=(\d+) count=(\d+)', cfgp.read_text())
        img_off = int(m.group(1)) * int(m.group(2)) if m else -1
        ov = {'name': name, 'sha1': cfg.get('sha1'), 'img_off': img_off, 'vram': segs[0]['vram'] if segs else 0,
              'size': size, 'units': sorted(set(units)), 'funcs': funcs, 'cdata': cdata,
              'symtxt': {k: v[0] for k, v in symtxt.items()}, 'problems': problems, 'mismatch': mism}
        overlays.append(ov)
        for f in funcs:
            if f['bind'] == 'GLOBAL':
                all_funcs.setdefault(f['name'], []).append((name, f['addr']))

    # Names any overlay's sym.txt gives one address for (cross-overlay data and
    # functions of overlays without C, e.g. the FieldEng_* window).
    sym_global = {}
    for ov in overlays:
        for k, a in ov['symtxt'].items():
            sym_global.setdefault(k, set()).add(a)
    sym_unique = {k: next(iter(v)) for k, v in sym_global.items() if len(v) == 1}
    host_provided = {'pe_mips_gpr', 'Psyz_GteDataRead', 'Psyz_GteDataWrite', 'Psyz_GteCtrlRead',
                     'Psyz_GteCtrlWrite', 'Psyz_GteCommand'}
    psyq_all = {e['name'] for e in main_ft['entries'] if e['unit'].startswith('psyq/')}
    global_refs = set()

    summary = {'overlays': len(overlays), 'with_code': 0, 'functions': 0, 'addr_mismatch': [],
               'unresolved': {}, 'problems': {}}
    abs_lines = ['/* Generated by tools/pc/gen_overlays.py */']
    table_c = ['/* Generated by tools/pc/gen_overlays.py */', '#include "pe_host_overlays.h"', '']
    table_rows = []
    for ov in overlays:
        n = ov['name']
        tag = 'pe_ovl_%s__' % safe(n)
        if ov['problems']:
            summary['problems'][n] = ov['problems']
        summary['addr_mismatch'] += ['%s: %s' % (n, m) for m in ov['mismatch']]
        if not ov['units']:
            table_rows.append((ov, []))
            continue
        summary['with_code'] += 1
        defined, undef = {}, set()
        objs = []
        for src in ov['units']:
            o = host_dir / (src + '.o')
            objs.append(str(o))
            d, u = host_syms(o)
            defined.update(d)
            undef |= u
        own_funcs = {f['name']: f['addr'] for f in ov['funcs']}
        rename = {}
        unresolved = []
        for s, t in defined.items():
            if t in 'TtWw' and s in own_funcs:
                rename[s] = tag + s
            elif t in 'CDBR':
                a = ov['cdata'].get(s, ov['symtxt'].get(s))
                if a is None:
                    m = re.fullmatch(r'(?:D|jtbl|g|B)_([0-9A-Fa-f]{8})', s)
                    a = int(m.group(1), 16) if m else None
                if a is not None:
                    rename[s] = tag + s
                    abs_lines.append('%s%s%s = 0x%08X;' % (PREFIX, tag, s, a))
        for s in sorted(undef - set(defined)):
            if s in own_funcs:
                rename[s] = tag + s
                continue
            if s in host_provided:
                continue
            if s in main_funcs or s in main_abs or s.startswith('pe_abs_'):
                global_refs.add(s)
                continue
            a = ov['symtxt'].get(s)
            if a is None:
                m = re.fullmatch(r'(?:D|jtbl|g|B|func)_([0-9A-Fa-f]{8})', s)
                if m:
                    a = int(m.group(1), 16)
            if a is None and s in all_funcs:
                addrs = {x[1] for x in all_funcs[s]}
                if len(addrs) == 1:
                    a = addrs.pop()
            if a is None and s in sym_unique:
                a = sym_unique[s]
            if a is not None:
                rename[s] = tag + s
                abs_lines.append('%s%s%s = 0x%08X;' % (PREFIX, tag, s, a))
            else:
                # Left for the main link: PSY-Q functions (pe_psyq_ rename)
                # and anything gen_link.py resolves.
                global_refs.add(s)
        if unresolved:
            summary['unresolved'][n] = unresolved
        (out / ('rename_%s.txt' % n)).write_text(''.join('%s%s %s%s\n' % (PREFIX, k, PREFIX, v) for k, v in sorted(rename.items())))
        (out / ('objs_%s.txt' % n)).write_text('\n'.join(objs) + '\n')
        host_defined = {k for k, t in defined.items() if t in 'TW'}
        gl = sorted((f['addr'], tag + f['name']) for f in ov['funcs']
                    if f['bind'] == 'GLOBAL' and f['name'] in host_defined)
        no_host = sorted(f['name'] for f in ov['funcs'] if f['bind'] == 'GLOBAL' and f['name'] not in host_defined)
        if no_host:
            summary.setdefault('no_host_version', {})[n] = no_host
        summary['functions'] += len(gl)
        table_rows.append((ov, gl))

    for ov, gl in table_rows:
        for _, hn in gl:
            table_c.append('extern void %s(void);' % hn)
    for ov, gl in table_rows:
        if gl:
            table_c.append('static const PeHostFunc ovl_funcs_%s[] = {' % safe(ov['name']))
            table_c += ['    {0x%08Xu, (void *)%s},' % (a, hn) for a, hn in gl]
            table_c.append('};')
    table_c.append('const PeHostOverlay pe_overlay_table[] = {')
    for ov, gl in table_rows:
        sha = ov['sha1'] or '0' * 40
        table_c.append('    {"%s", 0x%08Xu, 0x%Xu, %d, {%s}, %s, %d},' % (
            ov['name'], ov['vram'], ov['size'], ov['img_off'],
            ', '.join('0x%s' % sha[i:i + 2] for i in range(0, 40, 2)),
            ('ovl_funcs_%s' % safe(ov['name'])) if gl else '0', len(gl)))
    table_c.append('};')
    table_c.append('const unsigned int pe_overlay_count = %d;' % len(table_rows))
    (out / 'overlay_global_refs.txt').write_text(''.join(x + '\n' for x in sorted(global_refs)))
    (out / 'overlay_table.c').write_text('\n'.join(table_c) + '\n')
    (out / 'overlay_abs.ld').write_text('\n'.join(abs_lines) + '\n')
    (out / 'overlay_summary.json').write_text(json.dumps(summary, indent=1))
    print(json.dumps({'overlays': summary['overlays'], 'with_code': summary['with_code'],
                      'functions': summary['functions'], 'addr_mismatch': len(summary['addr_mismatch']),
                      'overlays_with_unresolved': len(summary['unresolved']),
                      'problems': len(summary['problems']),
                      'no_host_version': sum(len(v) for v in summary.get('no_host_version', {}).values())}))


if __name__ == '__main__':
    main()
