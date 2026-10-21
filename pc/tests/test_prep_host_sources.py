"""Checks for tools/pc/prep_host_sources.py. Run: python3 pc/tests/test_prep_host_sources.py"""
import sys, pathlib
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / 'tools' / 'pc'))
import prep_host_sources as p

def run(src):
    r = p.new_report()
    return p.transform(src, r, 't.c'), r

def norm(s):
    return ' '.join(s.split())

cases = 0
def check(cond, msg):
    global cases
    cases += 1
    if not cond:
        print('FAIL:', msg); sys.exit(1)

# register pin removed, $zero pin becomes 0
out, r = run('void f(void){ register int a asm("$4"); register unsigned z asm("$0"); a = z; }')
check('asm' not in out and 'z = 0' in norm(out).replace('z  = 0', 'z = 0'), out)

# symbol alias keeps the label through the host prefix macro; scratch alias renamed
out, r = run('extern int x asm("g_Foo"); extern int y asm("0x1F800008");')
check('PE_ASM_LABEL("g_Foo")' in out and 'PE_ASM_LABEL("pe_abs_1F800008")' in out, out)

# barrier with matched different input is an assignment, identity is nothing
out, r = run('void f(int a,int b){ asm volatile("" : "=r"(a) : "0"(b)); asm volatile("" : "=r"(a) : "0"(a)); }')
check('(a) = (__typeof__(a))(b);' in out and out.count('do {') == 2, out)

# mfc2 into a register-pinned output (macro written for a caller's pins)
out, r = run('void f(void){ register int gx asm("$8"); asm volatile("mfc2 $8,$25" : "=r"(gx)); }')
check('PE_R(8) = pe_gte_data_read(25);' in out and '(gx) = (__typeof__(gx))(uintptr_t)PE_R(8);' in out, out)

# input bound to a pinned variable feeds the register the template names
out, r = run('void f(int *p){ register int *o asm("$8") = p; asm volatile("swc2 $20,0($8)" : : "r"(o) : "memory"); }')
check('PE_R(8) = (unsigned int)(uintptr_t)(o);' in out and '*(unsigned int *)((uintptr_t)PE_R(8) + (0)) = pe_gte_data_read(20);' in out, out)

# value carried between statements through the virtual register file
out, r = run('void f(void){ asm volatile("cfc2 $13,$0" : : : "$13"); asm volatile("ctc2 $13,$0" : : : "$13"); }')
check('PE_R(13) = pe_gte_ctrl_read(0);' in out and 'pe_gte_ctrl_write(0, PE_R(13));' in out
      and any('carried' in n['note'] for n in r['notes']), out)

# COP2 command word and lwc2 through an operand
out, r = run('void f(void*q){ asm volatile("lwc2 $0,4(%0)\\n\\t.word 0x4A180001" : : "r"(q)); }')
check('pe_gte_data_write(0, *(unsigned int *)((uintptr_t)__pe_o0 + (4)));' in out
      and 'Psyz_GteCommand(0x4A180001u);' in out, out)

# anything else is reported and left untouched
out, r = run('void f(void){ asm volatile("syscall"); }')
check(r['unsupported'] and 'syscall' in out, out)

# offsetof becomes a constant expression
out, r = run('typedef char t[((u32)&(((S *)0)->m)) == 4 ? 1 : -1];')
check('__builtin_offsetof(S, m)' in out, out)

print('ok', cases, 'checks')
