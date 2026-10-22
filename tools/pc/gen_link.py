#!/usr/bin/env python3
"""Generate the host link inputs from the game objects.

Inputs (made earlier):
  build/pc-host/func_table_all.json   tools/pc/gen_func_table.py
  build/pc-host/pe_abs_syms.json      tools/pc/gen_abs_syms.py
  host game objects (*.c.o)           tools/pc/host_compile.sh
  shim objects                        pc/host/psyq/*.c compiled for the host

Outputs in OUT_DIR:
  psyq_rename.txt   objcopy --redefine-syms list: every PSY-Q function name the
                    game uses -> pe_psyq_<name>, so the game never binds to the
                    host C library, libgcc or PsyZ by accident.
  abs_syms.ld       NAME = 0xADDR; for every data symbol the game references.
  func_table.c      retail address -> host function, for the jump stubs.
  psyq_traps.c      retail addresses of PSY-Q functions with no host version
                    (an int3 goes there, so a call reports itself).
  psyq_stubs.c      pe_psyq_<name> stubs for every used PSY-Q function that no
                    shim object defines yet; each reports its first call.

Usage: gen_link.py OUT_DIR --game-objs LIST.txt [--psyq-objs LIST.txt] [--shim-objs LIST.txt]

--psyq-objs are decomp PSY-Q units compiled for the host (pure software:
libc, libmath, libgte, the libgpu primitive helpers). They are renamed like
the game's references, so they provide pe_psyq_<name> themselves.
"""
import json
import pathlib
import re
import subprocess
import sys


NM = 'nm'
PREFIX = ''  # C symbol prefix of the target ("_" for 32-bit Windows)


def unprefix(n):
    return n[len(PREFIX):] if PREFIX and n.startswith(PREFIX) else n


def nm(objs, defined):
    args = [NM, '--defined-only'] if defined else [NM, '-u']
    out = set()
    for i in range(0, len(objs), 200):
        res = subprocess.run(args + objs[i:i + 200], check=True, capture_output=True, text=True).stdout
        for line in res.splitlines():
            p = line.split()
            if not p or line.endswith(':'):
                continue
            if defined and len(p) >= 3:
                out.add((p[1], unprefix(p[2])))
            elif not defined:
                out.add(unprefix(p[-1]))
    return out


def read_list(path):
    return [l.strip() for l in pathlib.Path(path).read_text().splitlines() if l.strip()]


def main():
    global NM, PREFIX
    if '--nm' in sys.argv:
        NM = sys.argv[sys.argv.index('--nm') + 1]
    if '--sym-prefix' in sys.argv:
        PREFIX = sys.argv[sys.argv.index('--sym-prefix') + 1]
    out = pathlib.Path(sys.argv[1])
    out.mkdir(parents=True, exist_ok=True)
    game_objs = read_list(sys.argv[sys.argv.index('--game-objs') + 1])
    shim_objs = read_list(sys.argv[sys.argv.index('--shim-objs') + 1]) if '--shim-objs' in sys.argv else []
    psyq_objs = read_list(sys.argv[sys.argv.index('--psyq-objs') + 1]) if '--psyq-objs' in sys.argv else []

    ft = json.load(open('build/pc-tables/func_table_all.json'))
    absmap = json.load(open('build/pc-tables/pe_abs_syms.json'))
    unit = {e['name']: e['unit'] for e in ft['entries']}
    psyq_all = {e['name'] for e in ft['entries'] if e['unit'].startswith('psyq/')}

    guest_objs = game_objs + psyq_objs
    game_def = nm(guest_objs, True)
    game_def_names = {n for _, n in game_def}
    psyq_def_names = {n for t, n in nm(psyq_objs, True) if t in 'TtWw'} if psyq_objs else set()
    guest_undef = nm(guest_objs, False) - game_def_names
    if '--extra-refs' in sys.argv:
        # Names the overlays leave to the main link (tools/pc/gen_overlays.py).
        guest_undef |= set(read_list(sys.argv[sys.argv.index('--extra-refs') + 1])) - game_def_names
    host_provided = {'pe_mips_gpr', 'pe_gte_data_read', 'pe_gte_data_write', 'pe_gte_ctrl_read',
                     'pe_gte_ctrl_write', 'pe_cop0_status', 'pe_asm_break', 'Psyz_GteDataRead', 'Psyz_GteDataWrite', 'Psyz_GteCtrlRead',
                     'Psyz_GteCtrlWrite', 'Psyz_GteCommand'}

    def addr_of(s):
        if s in absmap:
            return absmap[s], 'map'
        m = re.fullmatch(r'pe_abs_([0-9A-F]{8})', s)
        if m:
            return int(m.group(1), 16), 'label'
        m = re.fullmatch(r'(?:D|jtbl|g|B|L)_([0-9A-Fa-f]{8})', s)
        if m:
            return int(m.group(1), 16), 'name'
        return None, None

    # Direct calls from the executable into overlay code (func_801xxxxx with
    # no definition in the executable) go to the overlay window address,
    # where the jump for the loaded overlay's host function is written.
    overlay_calls = {}
    by_addr = {e['addr']: e for e in ft['entries']}
    alias = {}
    psyq_used, data, unresolved = set(), {}, []
    by_name = 0
    # Every PSY-Q function a guest object defines is renamed too.
    psyq_used |= psyq_def_names & psyq_all
    for s in sorted(guest_undef):
        # Host functions that host patches of game code call (pe_host_*) are
        # the port's own, not PSY-Q names.
        if s in host_provided or s.startswith('pe_host_'):
            continue
        if s in psyq_all:
            psyq_used.add(s)
            continue
        if s in unit:
            unresolved.append(s + ' (game function without host object)')
            continue
        m = re.fullmatch(r'func_(80[0-9A-Fa-f]{6})', s)
        if m and int(m.group(1), 16) in by_addr:
            # An address-named reference to a known function.
            target = by_addr[int(m.group(1), 16)]
            alias[s] = target['name']
            if target['unit'].startswith('psyq/'):
                psyq_used.add(target['name'])
            continue
        if m and int(m.group(1), 16) >= 0x800C0000:
            overlay_calls[s] = int(m.group(1), 16)
            data[s] = int(m.group(1), 16)
            continue
        a, how = addr_of(s)
        if a is not None:
            data[s] = a
            by_name += how == 'name'
            continue
        # Functions from PSY-Q objects the PS1 build could not produce here
        # (sprintf, ratan2, ...) and other PSY-Q names.
        psyq_used.add(s)
    for s in psyq_def_names - psyq_all:
        # PSY-Q helpers that are not in the retail table (static/local names).
        pass

    # Data the game objects define themselves (tentative definitions and the
    # few initialised ones) also lives at its retail address: a linker-script
    # assignment overrides the object's own definition, and the object's
    # references follow it. The retail image already holds the initial values.
    host_only_data = []
    for t, n in sorted(game_def):
        if t in 'CDBRG' and n not in data:
            a, how = addr_of(n)
            if a is not None:
                data[n] = a
            else:
                host_only_data.append(n)

    P = PREFIX
    (out / 'psyq_rename.txt').write_text(''.join('%s%s %spe_psyq_%s\n' % (P, s, P, s) for s in sorted(psyq_used)) +
                                         '%smain %spe_game_main\n' % (P, P))
    (out / 'alias_rename.txt').write_text(''.join('%s%s %s%s\n' % (P, k, P, v) for k, v in sorted(alias.items())))
    (out / 'abs_syms.ld').write_text('/* Generated by tools/pc/gen_link.py */\n' +
                                     ''.join('%s%s = 0x%08X;\n' % (P, s, data[s]) for s in sorted(data)))

    shim_def = {n for t, n in nm(shim_objs, True) if t in 'TtWw'} if shim_objs else set()
    implemented = {s for s in psyq_used if 'pe_psyq_' + s in shim_def or s in psyq_def_names}
    stubs = sorted(psyq_used - implemented)
    stubs_set = set(stubs)

    # Function table: game functions, plus used PSY-Q functions (shim or stub).
    rows = []
    for e in ft['entries']:
        if e['unit'].startswith('psyq/'):
            if e['name'] in psyq_used and (e['name'] in implemented or e['name'] in stubs_set):
                rows.append((e['addr'], 'pe_psyq_' + e['name']))
        elif e['name'] in game_def_names:
            rows.append((e['addr'], e['name']))
        else:
            unresolved.append(e['name'] + ' (no host definition)')
    rows.sort()
    lines = ['/* Generated by tools/pc/gen_link.py */', '#include "pe_host_func_table.h"', '']
    lines += ['extern void %s(void);' % n for _, n in rows]
    lines += ['', 'const PeHostFunc pe_main_func_table[] = {']
    lines += ['    {0x%08Xu, (void *)%s},' % (a, n) for a, n in rows]
    lines += ['};', 'const unsigned int pe_main_func_count = %d;' % len(rows), '']
    (out / 'func_table.c').write_text('\n'.join(lines))

    traps = sorted(e['addr'] for e in ft['entries'] if e['unit'].startswith('psyq/') and e['name'] not in psyq_used)
    summary_psyq_objs = len(psyq_objs)
    names = {e['addr']: e['name'] for e in ft['entries']}
    tl = ['/* Generated by tools/pc/gen_link.py */', '#include "pe_host_traps.h"', '',
          'const PeHostTrap pe_psyq_trap_table[] = {']
    tl += ['    {0x%08Xu, "%s"},' % (a, names[a]) for a in traps]
    tl += ['};', 'const unsigned int pe_psyq_trap_count = %d;' % len(traps), '']
    (out / 'psyq_traps.c').write_text('\n'.join(tl))

    sl = ['/* Generated by tools/pc/gen_link.py: PSY-Q functions with no host version yet. */',
          'void pe_host_stub_hit(const char *name);', '']
    for s in stubs:
        sl.append('int pe_psyq_%s(void) { pe_host_stub_hit("%s"); return 0; }' % (s, s))
    (out / 'psyq_stubs.c').write_text('\n'.join(sl) + '\n')

    summary = {'game_objects': len(game_objs), 'psyq_objects': summary_psyq_objs, 'psyq_used': len(psyq_used),
               'psyq_implemented': len(implemented), 'psyq_stubs': len(stubs),
               'data_symbols': len(data), 'data_from_name_only': by_name,
               'host_only_data': host_only_data,
               'overlay_calls': sorted(overlay_calls),
               'function_table': len(rows), 'psyq_traps': len(traps), 'unresolved': unresolved}
    (out / 'link_summary.json').write_text(json.dumps(summary, indent=1))
    print(json.dumps({k: (v if not isinstance(v, list) else len(v)) for k, v in summary.items()}))
    return 1 if unresolved else 0


if __name__ == '__main__':
    sys.exit(main())
