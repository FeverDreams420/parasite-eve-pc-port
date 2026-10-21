#!/usr/bin/env python3
"""Translate a hand-written MIPS routine (PSYQ_ASM_FUNCTION / GAME_ASM_FUNCTION
body) into an equivalent C function, instruction by instruction.

The decomp keeps 31 routines as the original assembler text (assembled with
.set noreorder / noat, so every delay slot is written out). This turns each
into C that does exactly the same register and memory operations:

* the 32 general registers, HI and LO are local variables (r[n], hi, lo);
* a branch evaluates its condition, runs its delay-slot instruction, then
  jumps; jr $ra runs the delay slot and returns $v0;
* jal SYM / jalr calls the host function at that address with $a0-$a3 and
  puts the result in $v0 (the PS1 address of a game function holds a jump to
  its host code, so calling an address works);
* %hi/%lo(SYM) use the symbol's address, which on the host is its retail
  address;
* trapping add/addi/sub wrap (the host has no overflow exception);
* COP2 transfers and commands go to PsyZ's GTE (Psyz_Gte*);
* COP0 Status ($12) reads/writes a variable (interrupt masking has no
  meaning on the host: handlers are serialised by the kernel lock).

Anything outside the supported set raises an error naming the line, so a
routine is either translated completely or not at all.

Used by tools/pc/prep_host_sources.py; can also be run on a file:
  mips_to_c.py NAME < body.s
"""
import re
import sys

ABI = {'zero': 0, 'at': 1, 'v0': 2, 'v1': 3, 'a0': 4, 'a1': 5, 'a2': 6, 'a3': 7,
       't0': 8, 't1': 9, 't2': 10, 't3': 11, 't4': 12, 't5': 13, 't6': 14, 't7': 15,
       's0': 16, 's1': 17, 's2': 18, 's3': 19, 's4': 20, 's5': 21, 's6': 22, 's7': 23,
       't8': 24, 't9': 25, 'k0': 26, 'k1': 27, 'gp': 28, 'sp': 29, 'fp': 30, 's8': 30, 'ra': 31}

# include/gte_macros.inc: cop2op fake_op, op, gbg=0, sf=1, mx=0, v=0, cv=0, lm=0
GTE_FIXED = {
    'rtps': (0x01, 0x01, {}), 'rtpt': (0x02, 0x30, {}), 'dpcl': (0x06, 0x29, {}),
    'dpcs': (0x07, 0x10, {}), 'dpct': (0x0F, 0x2A, {}), 'intpl': (0x09, 0x11, {}),
    'ncs': (0x0C, 0x1E, {'lm': 1}), 'nct': (0x0D, 0x20, {'lm': 1}), 'ncds': (0x0E, 0x13, {'lm': 1}),
    'ncdt': (0x0F, 0x16, {'lm': 1}), 'nccs': (0x10, 0x1B, {'lm': 1}), 'ncct': (0x11, 0x3F, {'lm': 1}),
    'cdp': (0x12, 0x14, {'lm': 1}), 'cc': (0x13, 0x1C, {'lm': 1}), 'nclip': (0x14, 0x06, {'sf': 0}),
    'avsz3': (0x15, 0x2D, {}), 'avsz4': (0x16, 0x2E, {}),
}
GTE_PARAM = {  # mnemonic: (fake, op, parameter names, fixed)
    'mvmva': (0x04, 0x12, ['sf', 'mx', 'v', 'cv', 'lm'], {}),
    'sqr': (0x0A, 0x28, ['sf'], {'lm': 1}),
    'op': (0x17, 0x0C, ['sf'], {}),
    'gpf': (0x19, 0x3D, ['sf'], {}),
    'gpl': (0x1A, 0x3E, ['sf'], {}),
}
GTE_WORDS = {'rtv0': 0x4A486012, 'rtv1': 0x4A48E012, 'rtv2': 0x4A496012,
             'rtir12': 0x4A49E012, 'rtir0': 0x4A41E012, 'rtv0tr': 0x4A480012}


def cop2op(fake, op, gbg=0, sf=1, mx=0, v=0, cv=0, lm=0):
    imm = fake << 20 | gbg << 20 | sf << 19 | mx << 17 | v << 15 | cv << 13 | lm << 10 | op
    return 0x4A000000 | (imm & 0x1FFFFFF)


class AsmError(Exception):
    pass


def reg(tok):
    t = tok.strip()
    if not t.startswith('$'):
        raise AsmError('expected register, got ' + tok)
    t = t[1:]
    if t.isdigit() and int(t) < 32:
        return int(t)
    if t in ABI:
        return ABI[t]
    raise AsmError('bad register ' + tok)


WRITES_FIRST = {'addu', 'add', 'subu', 'sub', 'addiu', 'addi', 'and', 'or', 'xor', 'nor', 'andi', 'ori',
                'xori', 'lui', 'sll', 'srl', 'sra', 'sllv', 'srlv', 'srav', 'slt', 'sltu', 'slti', 'sltiu',
                'negu', 'move', 'lw', 'lh', 'lhu', 'lb', 'lbu', 'mflo', 'mfhi', 'mfc2', 'cfc2', 'mfc0'}


def reads_reg(text, r):
    m = re.match(r'([a-z0-9.]+)\s*(.*)$', text)
    if not m or r == 0:
        return False
    mn, rest = m.group(1), m.group(2)
    if mn in ('mtc2', 'ctc2', 'mtc0'):
        toks = re.findall(r'\$\w+', rest)[:1]
    elif mn in ('mfc2', 'cfc2', 'mfc0'):
        toks = []
    elif mn in ('lwc2', 'swc2'):
        toks = re.findall(r'\(\s*(\$\w+)\s*\)', rest)
    else:
        toks = re.findall(r'\$\w+', rest)
        if mn in WRITES_FIRST:
            toks = toks[1:]
    return any(reg(t) == r for t in toks)


def translate_body(name, body, syms, group):
    """C statements for one routine operating on the register file r[34]
    (r[32] = HI, r[33] = LO). `group` maps routine names translated together
    (same source file) to entry numbers: calls between them share registers,
    as the original code relies on (values passed in $t0-$t2, ...)."""
    lines = []
    for raw in body.split('\n'):
        line = raw.split('#')[0].strip()
        if not line:
            continue
        if line.startswith('.') and not re.match(r'\.L\w*:', line) and not line.startswith('.word'):
            continue  # directives
        lines.append(line)

    insns = []  # (label or None, text)
    pending_labels = []
    for line in lines:
        m = re.match(r'^([.\w$]+):\s*(.*)$', line)
        if m:
            pending_labels.append(m.group(1))
            line = m.group(2).strip()
            if not line:
                continue
        insns.append((pending_labels, line))
        pending_labels = []
    if pending_labels:
        insns.append((pending_labels, 'nop'))

    out = []
    used_labels = set()
    uses_cop0 = [False]

    def label(n):
        return 'L_' + re.sub(r'\W', '_', n)

    def R(t):
        n = reg(t)
        return '0u' if n == 0 else 'r[%d]' % n

    def W(t, expr):
        n = reg(t)
        return '' if n == 0 else 'r[%d] = (uint32_t)(%s);' % (n, expr)

    def imm(t):
        t = t.strip()
        m = re.fullmatch(r'%(hi|lo)\((\w+)\)', t)
        if m:
            syms.add(m.group(2))
            a = '((uint32_t)(uintptr_t)pe_asmref_%s)' % m.group(2)
            if m.group(1) == 'hi':
                return '((%s + 0x8000u) >> 16)' % a
            return '((uint32_t)(int32_t)(int16_t)(%s & 0xFFFFu))' % a
        return '(uint32_t)(%d)' % int(t, 0)

    def mem(t):
        m = re.fullmatch(r'\s*(.*?)\(\s*(\$\w+)\s*\)\s*', t)
        if not m:
            raise AsmError('bad memory operand ' + t)
        off = m.group(1).strip() or '0'
        return '(uintptr_t)(%s + %s)' % (R(m.group(2)), imm(off))

    def simple(text):
        """C for one non-branch instruction."""
        m = re.match(r'([a-z0-9.]+)\s*(.*)$', text)
        mn, rest = m.group(1), m.group(2)
        a = [x.strip() for x in rest.split(',')] if rest else []
        if mn == 'nop':
            return ''
        if mn in ('addu', 'add'):
            return W(a[0], '%s + %s' % (R(a[1]), R(a[2])))
        if mn in ('subu', 'sub'):
            return W(a[0], '%s - %s' % (R(a[1]), R(a[2])))
        if mn in ('addiu', 'addi'):
            return W(a[0], '%s + %s' % (R(a[1]), imm(a[2])))
        if mn == 'and':
            return W(a[0], '%s & %s' % (R(a[1]), R(a[2])))
        if mn == 'or':
            return W(a[0], '%s | %s' % (R(a[1]), R(a[2])))
        if mn == 'xor':
            return W(a[0], '%s ^ %s' % (R(a[1]), R(a[2])))
        if mn == 'nor':
            return W(a[0], '~(%s | %s)' % (R(a[1]), R(a[2])))
        if mn in ('andi', 'ori', 'xori'):
            op = {'andi': '&', 'ori': '|', 'xori': '^'}[mn]
            return W(a[0], '%s %s (%s & 0xFFFFu)' % (R(a[1]), op, imm(a[2])))
        if mn == 'lui':
            return W(a[0], '(%s & 0xFFFFu) << 16' % imm(a[1]))
        if mn == 'sll':
            return W(a[0], '%s << %d' % (R(a[1]), int(a[2], 0) & 31))
        if mn == 'srl':
            return W(a[0], '%s >> %d' % (R(a[1]), int(a[2], 0) & 31))
        if mn == 'sra':
            return W(a[0], '(uint32_t)((int32_t)%s >> %d)' % (R(a[1]), int(a[2], 0) & 31))
        if mn == 'sllv':
            return W(a[0], '%s << (%s & 31)' % (R(a[1]), R(a[2])))
        if mn == 'srlv':
            return W(a[0], '%s >> (%s & 31)' % (R(a[1]), R(a[2])))
        if mn == 'srav':
            return W(a[0], '(uint32_t)((int32_t)%s >> (%s & 31))' % (R(a[1]), R(a[2])))
        if mn == 'slt':
            return W(a[0], '(int32_t)%s < (int32_t)%s' % (R(a[1]), R(a[2])))
        if mn == 'sltu':
            return W(a[0], '%s < %s' % (R(a[1]), R(a[2])))
        if mn == 'slti':
            return W(a[0], '(int32_t)%s < (int32_t)%s' % (R(a[1]), imm(a[2])))
        if mn == 'sltiu':
            return W(a[0], '%s < %s' % (R(a[1]), imm(a[2])))
        if mn == 'negu':
            return W(a[0], '0u - %s' % R(a[1]))
        if mn == 'move':
            return W(a[0], R(a[1]))
        if mn == 'lw':
            return W(a[0], '*(uint32_t *)%s' % mem(a[1]))
        if mn == 'lh':
            return W(a[0], '(uint32_t)(int32_t)*(int16_t *)%s' % mem(a[1]))
        if mn == 'lhu':
            return W(a[0], '*(uint16_t *)%s' % mem(a[1]))
        if mn == 'lb':
            return W(a[0], '(uint32_t)(int32_t)*(int8_t *)%s' % mem(a[1]))
        if mn == 'lbu':
            return W(a[0], '*(uint8_t *)%s' % mem(a[1]))
        if mn == 'sw':
            return '*(uint32_t *)%s = %s;' % (mem(a[1]), R(a[0]))
        if mn == 'sh':
            return '*(uint16_t *)%s = (uint16_t)%s;' % (mem(a[1]), R(a[0]))
        if mn == 'sb':
            return '*(uint8_t *)%s = (uint8_t)%s;' % (mem(a[1]), R(a[0]))
        if mn == 'mult':
            return '{ int64_t p = (int64_t)(int32_t)%s * (int32_t)%s; r[33] = (uint32_t)p; r[32] = (uint32_t)((uint64_t)p >> 32); }' % (R(a[0]), R(a[1]))
        if mn == 'multu':
            return '{ uint64_t p = (uint64_t)%s * %s; r[33] = (uint32_t)p; r[32] = (uint32_t)(p >> 32); }' % (R(a[0]), R(a[1]))
        if mn == 'div':
            x, y = R(a[0]), R(a[1])
            return ('{ int32_t n = (int32_t)%s, d = (int32_t)%s; if (d == 0) { r[33] = n >= 0 ? 0xFFFFFFFFu : 1u; r[32] = (uint32_t)n; } '
                    'else if (n == INT32_MIN && d == -1) { r[33] = (uint32_t)n; r[32] = 0; } else { r[33] = (uint32_t)(n / d); r[32] = (uint32_t)(n %% d); } }' % (x, y))
        if mn == 'divu':
            x, y = R(a[0]), R(a[1])
            return ('{ uint32_t n = %s, d = %s; if (d == 0) { r[33] = 0xFFFFFFFFu; r[32] = n; } else { r[33] = n / d; r[32] = n %% d; } }' % (x, y))
        if mn == 'mflo':
            return W(a[0], 'r[33]')
        if mn == 'mfhi':
            return W(a[0], 'r[32]')
        if mn == 'mtlo':
            return 'r[33] = %s;' % R(a[0])
        if mn == 'mthi':
            return 'r[32] = %s;' % R(a[0])
        if mn in ('mtc2', 'ctc2'):
            fn = 'pe_gte_data_write' if mn == 'mtc2' else 'pe_gte_ctrl_write'
            return '%s(%d, %s);' % (fn, reg(a[1]), R(a[0]))
        if mn in ('mfc2', 'cfc2'):
            fn = 'pe_gte_data_read' if mn == 'mfc2' else 'pe_gte_ctrl_read'
            return W(a[0], '%s(%d)' % (fn, reg(a[1])))
        if mn == 'lwc2':
            return 'pe_gte_data_write(%d, *(uint32_t *)%s);' % (reg(a[0]), mem(a[1]))
        if mn == 'swc2':
            return '*(uint32_t *)%s = pe_gte_data_read(%d);' % (mem(a[1]), reg(a[0]))
        if mn == 'mfc0':
            uses_cop0[0] = True
            if reg(a[1]) != 12:
                raise AsmError('mfc0 of COP0 register other than Status: ' + text)
            return W(a[0], 'pe_cop0_status')
        if mn == 'mtc0':
            uses_cop0[0] = True
            if reg(a[1]) != 12:
                raise AsmError('mtc0 of COP0 register other than Status: ' + text)
            return 'pe_cop0_status = %s;' % R(a[0])
        if mn == '.word':
            w = int(a[0], 0)
            if (w >> 25) != 0x25:
                raise AsmError('data word in code: ' + text)
            return 'Psyz_GteCommand(0x%08Xu);' % w
        if mn in GTE_FIXED:
            fake, op, fixed = GTE_FIXED[mn]
            return 'Psyz_GteCommand(0x%08Xu);' % cop2op(fake, op, **fixed)
        if mn in GTE_PARAM:
            fake, op, names, fixed = GTE_PARAM[mn]
            kw = dict(fixed)
            for nm, v in zip(names, a):
                kw[nm] = int(v, 0)
            return 'Psyz_GteCommand(0x%08Xu);' % cop2op(fake, op, **kw)
        if mn in GTE_WORDS:
            return 'Psyz_GteCommand(0x%08Xu);' % GTE_WORDS[mn]
        if mn == 'break':
            return 'pe_asm_break("%s");' % name
        raise AsmError('unsupported instruction: ' + text)

    def call(target_expr):
        return 'r[2] = ((pe_asm_fn)(uintptr_t)(%s))(r[4], r[5], r[6], r[7]); r[3] = 0;' % target_expr

    loads = ('lw', 'lh', 'lhu', 'lb', 'lbu', 'mfc2', 'cfc2', 'mfc0')
    warnings = []
    i = 0
    n = len(insns)
    while i < n:
        labels, text = insns[i]
        for lb in labels:
            out.append('%s:;' % label(lb))
        m = re.match(r'([a-z0-9.]+)\s*(.*)$', text)
        mn, rest = m.group(1), m.group(2)
        a = [x.strip() for x in rest.split(',')] if rest else []
        branch = mn in ('b', 'j', 'beq', 'bne', 'beqz', 'bnez', 'blez', 'bgtz', 'bltz', 'bgez', 'jr', 'jal', 'jalr')
        if not branch:
            out.append(simple(text))
            # Load delay check: on the R3000 the instruction right after a
            # load still sees the old value of the loaded register.
            if mn in loads and i + 1 < n and reads_reg(insns[i + 1][1], reg(a[0])):
                warnings.append('load delay: "%s" then "%s"' % (text, insns[i + 1][1]))
            i += 1
            continue
        if i + 1 >= n:
            raise AsmError('branch without delay slot: ' + text)
        dlabels, dtext = insns[i + 1]
        if re.match(r'(b|j|beq|bne|beqz|bnez|blez|bgtz|bltz|bgez|jr|jal|jalr)\b', dtext):
            raise AsmError('branch in a delay slot: ' + dtext)
        delay = simple(dtext)
        if mn in ('b', 'j'):
            out.append('%s goto %s;' % (delay, label(a[0])))
            used_labels.add(a[0])
        elif mn == 'jr':
            if reg(a[0]) != 31:
                out.append('{ uint32_t t = %s; %s r[2] = ((pe_asm_fn)(uintptr_t)t)(r[4], r[5], r[6], r[7]); return; }' % (R(a[0]), delay))
            else:
                out.append('%s return;' % delay)
        elif mn == 'jal':
            if a[0] in group:
                out.append('%s r[31] = 0; pe_asmgroup(%d, r);' % (delay, group[a[0]]))
            else:
                syms.add(a[0])
                out.append('%s %s' % (delay, call('pe_asmref_%s' % a[0])))
        elif mn == 'jalr':
            rs = a[-1]
            out.append('{ uint32_t t = %s; %s %s }' % (R(rs), delay, call('t')))
        else:
            if mn in ('beq', 'bne'):
                cond = '%s %s %s' % (R(a[0]), '==' if mn == 'beq' else '!=', R(a[1]))
                tgt = a[2]
            else:
                op = {'beqz': '== 0', 'bnez': '!= 0', 'blez': '<= 0', 'bgtz': '> 0', 'bltz': '< 0', 'bgez': '>= 0'}[mn]
                cond = '(int32_t)%s %s' % (R(a[0]), op) if mn not in ('beqz', 'bnez') else '%s %s' % (R(a[0]), op)
                tgt = a[1]
            out.append('{ int c = (%s); %s if (c) goto %s; }' % (cond, delay, label(tgt)))
            used_labels.add(tgt)
        if dlabels:
            # Another branch targets this delay slot: entering there runs the
            # delay-slot instruction and continues after the branch.
            k = len(out)
            out.append('goto after_%d;' % k)
            for lb in dlabels:
                out.append('%s:;' % label(lb))
            out.append(delay)
            out.append('after_%d:;' % k)
        i += 2

    out.append('return;')
    return out, warnings


def translate_group(tag, funcs, stack_args=0):
    """Translate the routines of one source file together.

    funcs: list of (name, body). Returns (c_source, warnings). Each routine
    gets a public C function NAME(a0..a3) returning $v0; their bodies live in
    one static function so branches between routines (VectorNormalS jumps
    into VectorNormalSS) and register-passing calls work as in the original.
    """
    syms = set()
    group = {n: k for k, (n, _) in enumerate(funcs)}
    parts, warns = [], []
    for k, (n, b) in enumerate(funcs):
        st, w = translate_body(n, b, syms, group)
        parts.append('E_%d:;\n    ' % k + '\n    '.join(x for x in st if x))
        warns += ['%s: %s' % (n, x) for x in w]
    names = set(group)
    decl = ''.join('extern char pe_asmref_%s[] __asm__(PE_ASM_LABEL("%s"));\n' % (x, x)
                   for x in sorted(syms - names))
    for x in sorted(syms & names):
        decl += '#define pe_asmref_%s ((char *)%s)\n' % (x, x)
    # stack_args > 0: the wrappers also take that many further arguments and
    # store them where the MIPS calling convention puts them (16 bytes above
    # the caller's $sp, after the four argument-register home slots).
    extra = ''.join(', uint32_t a%d' % (4 + k) for k in range(stack_args))
    store = ''.join(' pe_stack[%d] = a%d;' % (244 + k, 4 + k) for k in range(stack_args))
    proto = ''.join('uint32_t %s(uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3%s);\n' % (n, extra) for n, _ in funcs)
    sw = ' '.join('case %d: goto E_%d;' % (k, k) for k in range(len(funcs)))
    grp = ('static void pe_asmgroup(int entry, uint32_t *r) {\n'
           '    switch (entry) { %s default: return; }\n    %s\n}\n') % (sw, '\n    '.join(parts))
    wrap = ''.join(
        'uint32_t %s(uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3%s) {\n'
        '    uint32_t r[34] = {0};\n'
        '    uint32_t pe_stack[%d];\n'
        '    r[4] = a0; r[5] = a1; r[6] = a2; r[7] = a3;%s\n'
        '    r[29] = (uint32_t)(uintptr_t)&pe_stack[240];\n'
        '    pe_asmgroup(%d, r);\n'
        '    return r[2];\n'
        '}\n' % (n, extra, 256 + stack_args, store, k) for k, (n, _) in enumerate(funcs))
    src = proto + decl + grp.replace('pe_asmgroup', 'pe_asmgroup_' + tag) + wrap.replace('pe_asmgroup', 'pe_asmgroup_' + tag)
    return src, warns


def translate(name, body):
    return translate_group(re.sub(r'\W', '_', name), [(name, body)])


def prelude():
    return ('#include <stdint.h>\n'
            'typedef uint32_t (*pe_asm_fn)(uint32_t, uint32_t, uint32_t, uint32_t);\n'
            'extern uint32_t pe_cop0_status;\n'
            'void pe_asm_break(const char *name);\n')


if __name__ == '__main__':
    c, w = translate(sys.argv[1], sys.stdin.read())
    sys.stdout.write(prelude() + c)
    for x in w:
        sys.stderr.write('warning: ' + x + '\n')
