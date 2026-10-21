/* Force-included into every game translation unit after the asm transform
 * (tools/pc/prep_host_sources.py). Declares what the rewritten asm uses. */
#ifndef PE_HOST_PRELUDE_H
#define PE_HOST_PRELUDE_H

#include <stdint.h>

/* PsyZ GTE register interface (third_party/psyz/psyz/include/psyz/gte.h). */
unsigned int Psyz_GteDataRead(unsigned reg);
void Psyz_GteDataWrite(unsigned reg, unsigned int value);
unsigned int Psyz_GteCtrlRead(unsigned reg);
void Psyz_GteCtrlWrite(unsigned reg, unsigned int value);
void Psyz_GteCommand(unsigned int cmd);
/* Full GTE register file access (pc/host/pe_host_gte.c). */
unsigned int pe_gte_data_read(unsigned reg);
void pe_gte_data_write(unsigned reg, unsigned int value);
unsigned int pe_gte_ctrl_read(unsigned reg);
void pe_gte_ctrl_write(unsigned reg, unsigned int value);

/* Virtual MIPS register file for registers named in rewritten asm. */
extern unsigned int pe_mips_gpr[32];
#define PE_R(n) (pe_mips_gpr[(n)])

/* Asm labels name raw linker symbols; add the target's C symbol prefix
 * ("_" on 32-bit Windows, nothing on Linux). */
#define PE_STR2_(x) #x
#define PE_STR_(x) PE_STR2_(x)
#define PE_ASM_LABEL(name) PE_STR_(__USER_LABEL_PREFIX__) name

/* Hand-written MIPS routines translated to C (tools/pc/mips_to_c.py). */
typedef uint32_t (*pe_asm_fn)(uint32_t, uint32_t, uint32_t, uint32_t);
extern uint32_t pe_cop0_status;
void pe_asm_break(const char *name);

#endif
