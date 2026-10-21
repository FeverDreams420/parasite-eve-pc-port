#ifndef PE_HOST_MEM_H
#define PE_HOST_MEM_H

#include <stdint.h>
#include "pe_host_func_table.h"

typedef struct {
    unsigned int pc, gp, t_addr, t_size, sp;
} PeExeInfo;

int pe_host_map_memory(void);
int pe_host_load_exe(const char *path, PeExeInfo *info);
int pe_host_load_exe_image(const unsigned char *img, unsigned int len, PeExeInfo *info);
void pe_host_write_jump(unsigned int mips_addr, void *host);
int pe_host_install_jumps(const PeHostFunc *table, unsigned int count);

extern unsigned int pe_mips_gpr[32];

#endif
