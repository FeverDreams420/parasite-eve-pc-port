#!/usr/bin/env python3
"""Copy the decomp sources into a host build tree, rewriting MIPS inline asm.

The PS1 sources keep GCC 2.7.2 matching crutches and GTE instructions as
inline asm. A host (x86) compiler cannot accept them. This tool rewrites each
asm construct into plain C with the same meaning, or leaves it untouched and
reports it when the meaning cannot be established with certainty.

Rewrites:
  * register pins   `register int x asm("$4")`        -> `register int x`
  * symbol aliases  `extern T x asm("g_Name")`        -> host-prefixed label
  * scratch aliases `extern T x asm("0x1F800008")`    -> label pe_abs_1F800008
  * empty barriers  `asm("" : "=r"(a) : "0"(b))`      -> `a = b` (or nothing)
  * GTE/CPU asm     lwc2/swc2/mtc2/mfc2/ctc2/cfc2, COP2 `.word` commands and
                    the small CPU subset used around them (lw, lh, lhu, sh,
                    sw, sll, or, move, ...) -> C calls to the PsyZ GTE
                    register interface (Psyz_GteDataRead/Write,
                    Psyz_GteCtrlRead/Write, Psyz_GteCommand).

It works on preprocessed C (gcc -E output), so register pins declared in a
function are visible when a GTE macro from a header expands inside it, and
string-macro templates are already expanded.

CPU registers named in asm ($8, $12, ...) become the global virtual register
file pe_mips_gpr[32] (see pc/host/pe_host_prelude.h). That keeps values that
retail code carries between asm statements (for example gte_pushrotcol0 /
gte_poprotcol0) and the value of pinned variables that an asm statement reads
by register name.

Usage: prep_host_sources.py IN.i OUT.c [--report REPORT.json]
"""
import json
import pathlib
import re
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import mips_to_c  # noqa: E402


ABI = {'zero': 0, 'at': 1, 'v0': 2, 'v1': 3, 'a0': 4, 'a1': 5, 'a2': 6, 'a3': 7,
       't0': 8, 't1': 9, 't2': 10, 't3': 11, 't4': 12, 't5': 13, 't6': 14, 't7': 15,
       's0': 16, 's1': 17, 's2': 18, 's3': 19, 's4': 20, 's5': 21, 's6': 22, 's7': 23,
       't8': 24, 't9': 25, 'k0': 26, 'k1': 27, 'gp': 28, 'sp': 29, 'fp': 30, 's8': 30,
       'ra': 31}


def reg_number(tok):
    """'$12' / '$t4' / 't4' -> 12, else None."""
    t = tok.strip()
    if t.startswith('$'):
        t = t[1:]
    if t.isdigit() and 0 <= int(t) < 32:
        return int(t)
    return ABI.get(t)


class Unsupported(Exception):
    pass


# ---------------------------------------------------------------- scanning

def find_asm_spans(text):
    """Yield ('enter', pos) when brace depth goes 0 -> 1 (a function body or
    a file-scope initializer starts), and ('asm', start, end, inner, depth)
    for each asm(...) in code."""
    i, n = 0, len(text)
    depth = 0
    while i < n:
        c = text[i]
        if c == '{':
            depth += 1
            if depth == 1:
                yield ('enter', i)
        elif c == '}':
            depth -= 1
        if c == '#' and (i == 0 or text[i - 1] == '\n'):
            j = text.find('\n', i)
            i = n if j < 0 else j
            continue
        if text.startswith('//', i):
            j = text.find('\n', i)
            i = n if j < 0 else j
            continue
        if text.startswith('/*', i):
            j = text.find('*/', i + 2)
            i = n if j < 0 else j + 2
            continue
        if c == '"' or c == "'":
            i = skip_literal(text, i)
            continue
        if c.isalpha() or c == '_':
            j = i
            while j < n and (text[j].isalnum() or text[j] == '_'):
                j += 1
            word = text[i:j]
            if word in ('asm', '__asm__', '__asm') and (i == 0 or not (text[i - 1].isalnum() or text[i - 1] == '_')):
                k = j
                while True:
                    k = skip_ws(text, k)
                    m = re.match(r'(volatile|__volatile__|__volatile)\b', text[k:])
                    if not m:
                        break
                    k += m.end()
                if k < n and text[k] == '(':
                    end = match_paren(text, k)
                    yield ('asm', i, end, text[k + 1:end - 1], depth)
                    i = end
                    continue
            i = j
            continue
        i += 1


def skip_ws(text, k):
    while k < len(text):
        if text[k] in ' \t\r\n':
            k += 1
        elif text.startswith('\\\n', k):
            k += 2
        elif text.startswith('\\\r\n', k):
            k += 3
        else:
            break
    return k


def skip_literal(text, i):
    q = text[i]
    i += 1
    while i < len(text) and text[i] != q:
        if text[i] == '\\':
            i += 1
        i += 1
    return i + 1


def match_paren(text, k):
    depth = 0
    i = k
    while i < len(text):
        c = text[i]
        if c == '"' or c == "'":
            i = skip_literal(text, i)
            continue
        if c == '(':
            depth += 1
        elif c == ')':
            depth -= 1
            if depth == 0:
                return i + 1
        i += 1
    raise ValueError('unbalanced asm parentheses')


def split_top(s, sep):
    out, depth, cur, i = [], 0, '', 0
    while i < len(s):
        c = s[i]
        if c == '"' or c == "'":
            j = skip_literal(s, i)
            cur += s[i:j]
            i = j
            continue
        if c in '([{':
            depth += 1
        elif c in ')]}':
            depth -= 1
        if c == sep and depth == 0:
            out.append(cur)
            cur = ''
        else:
            cur += c
        i += 1
    out.append(cur)
    return out


def clean(s):
    return re.sub(r'\\\r?\n', ' ', s).strip()


def unescape(s):
    return (s.replace('\\n', '\n').replace('\\t', '\t').replace('\\"', '"')
             .replace('\\\\', '\\'))


def template_text(tpl):
    tpl = clean(tpl)
    out = ''
    for m in re.finditer(r'"((?:[^"\\]|\\.)*)"|(\w+)|(\S)', tpl):
        if m.group(1) is not None:
            out += unescape(m.group(1))
        elif m.group(2) is not None:
            raise Unsupported('template uses identifier ' + m.group(2))
        else:
            raise Unsupported('unexpected token in template: ' + m.group(3))
    return out


def parse_operands(s):
    s = clean(s)
    if not s:
        return []
    ops = []
    for part in split_top(s, ','):
        m = re.match(r'\s*(?:\[\w+\]\s*)?"([^"]*)"\s*\((.*)\)\s*$', part, re.S)
        if not m:
            raise Unsupported('cannot parse operand: ' + part.strip())
        ops.append((m.group(1), m.group(2).strip()))
    return ops




# ---------------------------------------------------------------- translate

def label_for(name):
    if re.fullmatch(r'0[xX][0-9A-Fa-f]+', name):
        return 'pe_abs_%08X' % int(name, 16)
    return name


def plain_ident(expr):
    e = expr.strip()
    while e.startswith('(') and e.endswith(')') and match_paren(e, 0) == len(e):
        e = e[1:-1].strip()
    return e if re.fullmatch(r'[A-Za-z_]\w*', e) else None


def translate_statement(tpl, outputs, inputs, pins, notes):
    """Return C text equivalent to one asm statement, or raise Unsupported."""
    ops = outputs + inputs
    nout = len(outputs)

    bound = {}
    for idx, (_, expr) in enumerate(ops):
        name = plain_ident(expr)
        if name is not None and name in pins:
            bound[idx] = pins[name]
    for idx, r in bound.items():
        tpl = re.sub(r'%%%d(?!\d)' % idx, '$%d' % r, tpl)

    lines = []
    for raw in re.split(r'[\n;]', tpl):
        raw = raw.split('#')[0].strip()
        if raw:
            lines.append(raw)

    def con_of(idx):
        return ops[idx][0].replace('&', '').replace('%', '').strip()

    # matching inputs: output index -> input expression
    init = {}
    for idx in range(nout):
        if '+' in con_of(idx):
            init[idx] = ops[idx][1]
    for j in range(nout, len(ops)):
        c = con_of(j)
        if c.isdigit():
            k = int(c)
            if k >= nout:
                raise Unsupported('matching constraint to non-output %d' % k)
            init[k] = ops[j][1]

    if all(l.lower() == 'nop' for l in lines):
        # Empty (or nop-only) template: no data moves through instructions.
        # Only matched inputs ("0"(x)) carry a value into an output.
        keep = []
        for idx in range(nout):
            src = init.get(idx)
            dst = ops[idx][1]
            if src is not None and re.sub(r'\s+', '', src) != re.sub(r'\s+', '', dst):
                keep.append('(%s) = (__typeof__(%s))(%s);' % (dst, dst, src))
            elif src is None and 'm' not in con_of(idx):
                notes.append('barrier output %r has no matching input; left unchanged' % dst)
        return 'do { %s } while (0)' % ' '.join(keep) if keep else 'do { } while (0)'

    for idx in range(len(ops)):
        c = con_of(idx)
        if idx < nout:
            if c.lstrip('=+') not in ('r', 'd', 'm'):
                raise Unsupported('output constraint %r' % ops[idx][0])
        elif not (c in ('r', 'd', 'm', 'i', 'n', 'g') or c.isdigit()):
            raise Unsupported('input constraint %r' % ops[idx][0])

    pre, body, post = [], [], []
    for idx in range(len(ops)):
        c = con_of(idx)
        expr = ops[idx][1]
        if idx >= nout and (c.isdigit() or c in ('i', 'n', 'g')):
            continue
        if 'm' in c:
            continue
        if idx in bound:
            r = bound[idx]
            src = expr if idx >= nout else init.get(idx)
            if src is not None:
                pre.append('PE_R(%d) = (unsigned int)(uintptr_t)(%s);' % (r, src))
            continue
        if idx < nout:
            src = init.get(idx)
            pre.append('unsigned int __pe_o%d = %s;' % (
                idx, '(unsigned int)(uintptr_t)(%s)' % src if src else '0u'))
        else:
            pre.append('unsigned int __pe_o%d = (unsigned int)(uintptr_t)(%s);' % (idx, expr))
    # outputs bound to a register whose initial value comes from a matched input
    for idx in range(nout):
        if idx in bound and idx in init and '+' not in con_of(idx):
            pass  # already emitted above via init

    written = set()
    first_read = []
    out_written = set()

    def opnd(tok):
        m = re.fullmatch(r'%(\d+)', tok.strip())
        return int(m.group(1)) if m else None

    def rd(tok):
        k = opnd(tok)
        if k is not None:
            if k >= len(ops):
                raise Unsupported('operand %%%d out of range' % k)
            if 'm' in con_of(k) or con_of(k) in ('i', 'n', 'g'):
                if con_of(k) in ('i', 'n'):
                    return '(unsigned int)(%s)' % ops[k][1]
                raise Unsupported('memory operand %%%d used as a register' % k)
            if k < nout and k not in init and k not in out_written:
                raise Unsupported('reads output %%%d before it is set' % k)
            return '__pe_o%d' % k
        r = reg_number(tok)
        if r is None:
            raise Unsupported('bad register operand ' + tok)
        if r == 0:
            return '0u'
        if r not in written and r not in first_read:
            first_read.append(r)
        return 'PE_R(%d)' % r

    def wr(tok):
        k = opnd(tok)
        if k is not None:
            if k >= nout:
                raise Unsupported('writes input operand %%%d' % k)
            if 'm' in con_of(k):
                raise Unsupported('writes memory operand %%%d as a register' % k)
            out_written.add(k)
            return '__pe_o%d' % k
        r = reg_number(tok)
        if r is None:
            raise Unsupported('bad register operand ' + tok)
        if r == 0:
            return None
        if r in (28, 29, 31):
            raise Unsupported('writes $%d (gp/sp/ra)' % r)
        written.add(r)
        return 'PE_R(%d)' % r

    def mem(tok):
        k = opnd(tok)
        if k is not None and 'm' in con_of(k):
            return '((uintptr_t)&(%s))' % ops[k][1]
        m = re.fullmatch(r'\s*(-?(?:0[xX][0-9A-Fa-f]+|\d+))?\s*\(\s*([^)]+?)\s*\)\s*', tok)
        if not m:
            raise Unsupported('bad memory operand ' + tok)
        off = m.group(1) or '0'
        return '((uintptr_t)%s + (%s))' % (rd(m.group(2)), off)

    def gte_reg(tok):
        r = reg_number(tok)
        if r is None or not tok.strip().startswith('$'):
            raise Unsupported('bad GTE register ' + tok)
        return r

    def assign(dst, val):
        d = wr(dst)
        if d is not None:
            body.append('%s = %s;' % (d, val))

    for line in lines:
        m = re.match(r'(\.?[A-Za-z0-9_.]+)\s*(.*)$', line)
        if not m:
            raise Unsupported('cannot parse line ' + line)
        mn, rest = m.group(1).lower(), m.group(2)
        args = [a.strip() for a in split_top(rest, ',')] if rest.strip() else []
        if mn == 'nop':
            continue
        if mn == '.word':
            if len(args) != 1:
                raise Unsupported('.word with %d values' % len(args))
            w = int(args[0], 0)
            if (w >> 25) != 0x25:
                raise Unsupported('.word 0x%08X is not a COP2 command' % w)
            body.append('Psyz_GteCommand(0x%08Xu);' % w)
        elif mn == 'lwc2':
            body.append('pe_gte_data_write(%d, *(unsigned int *)%s);' % (gte_reg(args[0]), mem(args[1])))
        elif mn == 'swc2':
            body.append('*(unsigned int *)%s = pe_gte_data_read(%d);' % (mem(args[1]), gte_reg(args[0])))
        elif mn == 'mtc2':
            body.append('pe_gte_data_write(%d, %s);' % (gte_reg(args[1]), rd(args[0])))
        elif mn == 'mfc2':
            assign(args[0], 'pe_gte_data_read(%d)' % gte_reg(args[1]))
        elif mn == 'ctc2':
            body.append('pe_gte_ctrl_write(%d, %s);' % (gte_reg(args[1]), rd(args[0])))
        elif mn == 'cfc2':
            assign(args[0], 'pe_gte_ctrl_read(%d)' % gte_reg(args[1]))
        elif mn in ('lw', 'lh', 'lhu', 'lb', 'lbu'):
            a = mem(args[1])
            val = {'lw': '*(unsigned int *)%s',
                   'lh': '(unsigned int)(int)*(short *)%s',
                   'lhu': '(unsigned int)*(unsigned short *)%s',
                   'lb': '(unsigned int)(int)*(signed char *)%s',
                   'lbu': '(unsigned int)*(unsigned char *)%s'}[mn] % a
            assign(args[0], val)
        elif mn in ('sw', 'sh', 'sb'):
            v = rd(args[0])
            t = {'sw': 'unsigned int', 'sh': 'unsigned short', 'sb': 'unsigned char'}[mn]
            body.append('*(%s *)%s = (%s)%s;' % (t, mem(args[1]), t, v))
        elif mn in ('sll', 'srl', 'sra'):
            sa = int(args[2], 0)
            v = rd(args[1])
            val = {'sll': '(%s << %d)', 'srl': '(%s >> %d)',
                   'sra': '(unsigned int)((int)%s >> %d)'}[mn] % (v, sa)
            assign(args[0], val)
        elif mn in ('or', 'and', 'xor', 'addu', 'subu', 'nor'):
            a, b = rd(args[1]), rd(args[2])
            op = {'or': '|', 'and': '&', 'xor': '^', 'addu': '+', 'subu': '-'}.get(mn)
            val = '~(%s | %s)' % (a, b) if mn == 'nor' else '(%s %s %s)' % (a, op, b)
            assign(args[0], val)
        elif mn in ('addiu', 'ori', 'andi', 'xori'):
            a = rd(args[1])
            imm = int(args[2], 0)
            if mn == 'addiu':
                val = '(%s + (unsigned int)(%d))' % (a, imm)
            else:
                op = {'ori': '|', 'andi': '&', 'xori': '^'}[mn]
                val = '(%s %s 0x%Xu)' % (a, op, imm & 0xFFFF)
            assign(args[0], val)
        elif mn == 'lui':
            assign(args[0], '0x%08Xu' % ((int(args[1], 0) & 0xFFFF) << 16))
        elif mn == 'move':
            assign(args[0], rd(args[1]))
        else:
            raise Unsupported('instruction ' + line)

    # Registers read before being written: bound operands were loaded in pre;
    # a pinned variable in scope with that register is the register's value;
    # otherwise the value carried in the virtual register file is used.
    bound_regs = {bound[i] for i in bound if i >= nout or i in init}
    reg_to_var = {}
    for name, r in pins.items():
        reg_to_var.setdefault(r, name)
    for r in first_read:
        if r in bound_regs:
            continue
        if r in reg_to_var:
            pre.append('PE_R(%d) = (unsigned int)(uintptr_t)(%s);' % (r, reg_to_var[r]))
            notes.append('reads pinned variable %s through $%d' % (reg_to_var[r], r))
        else:
            notes.append('reads $%d carried from an earlier asm statement' % r)

    for idx in range(nout):
        expr = ops[idx][1]
        c = con_of(idx)
        if 'm' in c:
            continue
        if idx in bound:
            r = bound[idx]
            if r in written or idx in init:
                post.append('(%s) = (__typeof__(%s))(uintptr_t)PE_R(%d);' % (expr, expr, r))
            else:
                raise Unsupported('output %%%d bound to $%d is never written' % (idx, r))
        elif idx in out_written or idx in init:
            post.append('(%s) = (__typeof__(%s))(uintptr_t)__pe_o%d;' % (expr, expr, idx))
        else:
            raise Unsupported('output %%%d is never set' % idx)

    return 'do { %s } while (0)' % ' '.join(pre + body + post)


OFFSETOF_RE = re.compile(
    r'\(\(\s*(u32|unsigned int|unsigned long|int|s32)\s*\)\s*&\s*\(\s*\(\s*\(\s*([A-Za-z_][\w ]*?)\s*\*\s*\)\s*0\s*\)\s*->\s*([A-Za-z_][\w.\[\]]*)\s*\)\s*\)')


OFFSETOF_BARE_RE = re.compile(
    r'\(\s*(u32|unsigned int|unsigned long|int|s32)\s*\)\s*&\s*\(\s*\(\s*([A-Za-z_][\w ]*?)\s*\*\s*\)\s*0\s*\)\s*->\s*([A-Za-z_][\w.]*)()')


def constant_offsetof(text, report):
    """((u32)&(((T *)0)->m)) is not an integer constant expression for a
    modern compiler, which turns the decomp's PE1_STATIC_ASSERT layout checks
    into silent VLAs. Rewrite it to __builtin_offsetof so they are enforced."""
    def rep(m):
        report['offsetof'] += 1
        return '((%s)__builtin_offsetof(%s, %s))' % (m.group(1), m.group(2), m.group(3))
    text = OFFSETOF_RE.sub(rep, text)
    return OFFSETOF_BARE_RE.sub(rep, text)


def transform(text, report, relpath):
    text = constant_offsetof(text, report)
    asm_funcs = []
    out = []
    last = 0
    pins = {}
    for ev in find_asm_spans(text):
        if ev[0] == 'enter':
            pins = {}
            continue
        _, start, end, inner, depth = ev
        orig = text[start:end]
        line = text.count('\n', 0, start) + 1
        parts = split_top(inner, ':')
        notes = []
        text_chunk_override = None
        try:
            if len(parts) == 1:
                s = clean(parts[0])
                m = re.fullmatch(r'"([^"\\]*)"', s)
                if m and reg_number(m.group(1)) is not None and not m.group(1).startswith('.'):
                    # register pin on a variable declaration
                    r = reg_number(m.group(1))
                    mm = re.search(r'([A-Za-z_]\w*)\s*$', text[:start])
                    if mm and depth > 0:
                        pins[mm.group(1)] = r
                    rep = ''
                    nxt = text[skip_ws(text, end):skip_ws(text, end) + 1]
                    if r == 0 and nxt in (';', ','):
                        rep = ' = 0'   # $zero always reads as 0
                    if depth == 0:
                        # File-scope register variable: becomes an ordinary
                        # (common) global on the host.
                        report['global_pins'].append({'file': relpath, 'line': line,
                                                      'name': mm.group(1) if mm else None,
                                                      'reg': r})
                        pre_text = text[last:start]
                        k = max(pre_text.rfind(';'), pre_text.rfind('}'), pre_text.rfind('\n#'))
                        head, tail = pre_text[:k + 1], pre_text[k + 1:]
                        tail = re.sub(r'\bregister\b', '', tail, count=1)
                        text_chunk_override = head + tail
                    report['pins'] += 1
                elif m and re.fullmatch(r'[A-Za-z_]\w*|0[xX][0-9A-Fa-f]+', m.group(1)) \
                        and m.group(1).lower() not in ('nop', 'syscall', 'break'):
                    rep = '__asm__(PE_ASM_LABEL("%s"))' % label_for(m.group(1))
                    report['labels'] += 1
                else:
                    tpl = template_text(parts[0])
                    if depth == 0 and re.search(r'\.ent\s+\w+', tpl):
                        # A hand-written routine (PSYQ_ASM_FUNCTION /
                        # GAME_ASM_FUNCTION): translate it to C.
                        fname = re.search(r'\.ent\s+(\w+)', tpl).group(1)
                        mb = re.search(r'^\s*%s:\s*$(.*?)^\s*\.end\s+%s\b' % (re.escape(fname), re.escape(fname)),
                                       tpl, re.S | re.M)
                        if not mb:
                            raise Unsupported('cannot find the body of ' + fname)
                        asm_funcs.append((fname, mb.group(1)))
                        rep = ''
                    elif depth == 0:
                        names = re.findall(r'\.globl\s+(\w+)', tpl)
                        report['toplevel_asm_dropped'].append({'file': relpath, 'line': line,
                                                               'defines': names})
                        rep = ''
                    else:
                        rep = translate_statement(tpl, [], [], pins, notes)
                        report['translated'] += 1
            else:
                while len(parts) < 4:
                    parts.append('')
                if len(parts) > 4:
                    raise Unsupported('asm goto is not supported')
                tpl = template_text(parts[0])
                outs = parse_operands(parts[1])
                ins = parse_operands(parts[2])
                rep = translate_statement(tpl, outs, ins, pins, notes)
                if tpl.strip() == '':
                    report['barriers'] += 1
                else:
                    report['translated'] += 1
        except Unsupported as e:
            report['unsupported'].append({'file': relpath, 'line': line, 'reason': str(e),
                                          'asm': clean(orig)[:240]})
            rep = orig
        for n in notes:
            report['notes'].append({'file': relpath, 'line': line, 'note': n})
        out.append(text[last:start] if text_chunk_override is None else text_chunk_override)
        out.append(rep)
        last = end
    out.append(text[last:])
    if asm_funcs:
        tag = re.sub(r'\W', '_', pathlib.Path(relpath).name)
        try:
            csrc, warns = mips_to_c.translate_group(tag, asm_funcs)
            out.append('\n/* Hand-written MIPS routines of this file, translated by tools/pc/mips_to_c.py */\n')
            out.append(csrc)
            for n, _ in asm_funcs:
                report['asm_functions'].append({'file': relpath, 'name': n,
                                                'warnings': [w for w in warns if w.startswith(n + ':')]})
        except mips_to_c.AsmError as e:
            report['unsupported'].append({'file': relpath, 'line': 0, 'reason': 'asm routines: %s' % e,
                                          'asm': ', '.join(n for n, _ in asm_funcs)})
    return ''.join(out)


def new_report():
    return {'pins': 0, 'asm_functions': [], 'offsetof': 0, 'global_pins': [], 'labels': 0, 'barriers': 0, 'translated': 0,
            'toplevel_asm_dropped': [], 'unsupported': [], 'notes': []}


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    src, dst = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
    report = new_report()
    text = src.read_text(errors='surrogateescape')
    new = transform(text, report, sys.argv[1])
    dst.parent.mkdir(parents=True, exist_ok=True)
    dst.write_text(new, errors='surrogateescape')
    if '--report' in sys.argv:
        pathlib.Path(sys.argv[sys.argv.index('--report') + 1]).write_text(json.dumps(report, indent=1))
    return 1 if report['unsupported'] else 0


if __name__ == '__main__':
    sys.exit(main())
