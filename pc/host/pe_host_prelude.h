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

/* Virtual MIPS register file for registers named in rewritten asm. */
extern unsigned int pe_mips_gpr[32];
#define PE_R(n) (pe_mips_gpr[(n)])

/* Asm labels name raw linker symbols; add the target's C symbol prefix
 * ("_" on 32-bit Windows, nothing on Linux). */
#define PE_STR2_(x) #x
#define PE_STR_(x) PE_STR2_(x)
#define PE_ASM_LABEL(name) PE_STR_(__USER_LABEL_PREFIX__) name

#endif
