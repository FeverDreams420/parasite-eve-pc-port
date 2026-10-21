#define _GNU_SOURCE
/* PS1 address space on a 32-bit host.
 *
 * The game's data lives at its retail addresses, so the program reserves
 * those ranges at the same addresses:
 *   0x80000000  2 MiB main RAM (read/write/execute: holds the jump stubs)
 *   0x1F800000  scratchpad (1 KiB) and the I/O window after it
 * then copies the text/data image of your own SLUS_006.62 there and writes
 * a 5-byte x86 `jmp` at every decompiled function's retail address, so a
 * MIPS function pointer read from game data calls the compiled C.
 */
#include "pe_host_mem.h"
#include "pe_host_func_table.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

unsigned int pe_mips_gpr[32];

#define PE_RAM_BASE 0x80000000u
#define PE_RAM_SIZE 0x00200000u
#define PE_SCRATCH_BASE 0x1F800000u
#define PE_SCRATCH_SIZE 0x00010000u /* scratchpad + I/O window, 64 KiB */

static void *map_fixed(unsigned int addr, unsigned int size, int exec) {
#ifdef _WIN32
    void *p = VirtualAlloc((void *)(uintptr_t)addr, size, MEM_RESERVE | MEM_COMMIT,
                           exec ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE);
#else
    void *p = mmap((void *)(uintptr_t)addr, size,
                   PROT_READ | PROT_WRITE | (exec ? PROT_EXEC : 0),
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (p == MAP_FAILED) {
        p = NULL;
    }
#endif
    if (p != (void *)(uintptr_t)addr) {
        return NULL;
    }
    return p;
}

int pe_host_map_memory(void) {
    if (sizeof(void *) != 4) {
        fprintf(stderr, "pe: this build must be 32-bit\n");
        return -1;
    }
    if (!map_fixed(PE_RAM_BASE, PE_RAM_SIZE, 1)) {
        fprintf(stderr, "pe: cannot reserve PS1 RAM at 0x%08X "
                        "(32-bit Windows builds must be large-address-aware)\n", PE_RAM_BASE);
        return -1;
    }
    if (!map_fixed(PE_SCRATCH_BASE, PE_SCRATCH_SIZE, 0)) {
        fprintf(stderr, "pe: cannot reserve scratchpad at 0x%08X\n", PE_SCRATCH_BASE);
        return -1;
    }
    return 0;
}

static unsigned int rd32(const unsigned char *p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((unsigned int)p[3] << 24);
}

int pe_host_load_exe(const char *path, PeExeInfo *info) {
    unsigned char hdr[0x800];
    unsigned int t_addr, t_size;
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "pe: cannot open %s\n", path);
        return -1;
    }
    if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr) || memcmp(hdr, "PS-X EXE", 8) != 0) {
        fprintf(stderr, "pe: %s is not a PS-X EXE\n", path);
        fclose(f);
        return -1;
    }
    t_addr = rd32(hdr + 0x18);
    t_size = rd32(hdr + 0x1C);
    if (t_addr < PE_RAM_BASE + 0x10000 || t_addr + t_size > PE_RAM_BASE + PE_RAM_SIZE) {
        fprintf(stderr, "pe: unexpected load range 0x%08X+0x%X\n", t_addr, t_size);
        fclose(f);
        return -1;
    }
    if (fread((void *)(uintptr_t)t_addr, 1, t_size, f) != t_size) {
        fprintf(stderr, "pe: short read of %s\n", path);
        fclose(f);
        return -1;
    }
    fclose(f);
    if (info) {
        info->pc = rd32(hdr + 0x10);
        info->gp = rd32(hdr + 0x14);
        info->t_addr = t_addr;
        info->t_size = t_size;
        info->sp = rd32(hdr + 0x30);
    }
    return 0;
}

void pe_host_write_jump(unsigned int mips_addr, void *host) {
    unsigned char *p = (unsigned char *)(uintptr_t)mips_addr;
    int rel = (int)((uintptr_t)host - ((uintptr_t)p + 5));
    p[0] = 0xE9;
    memcpy(p + 1, &rel, 4);
}

int pe_host_install_jumps(const PeHostFunc *table, unsigned int count) {
    unsigned int i;
    for (i = 0; i < count; i++) {
        if (i + 1 < count && table[i + 1].mips_addr - table[i].mips_addr < 5) {
            fprintf(stderr, "pe: functions at 0x%08X and 0x%08X are too close for a jump\n",
                    table[i].mips_addr, table[i + 1].mips_addr);
            return -1;
        }
        pe_host_write_jump(table[i].mips_addr, table[i].host);
    }
#ifdef _WIN32
    FlushInstructionCache(GetCurrentProcess(), (void *)(uintptr_t)PE_RAM_BASE, PE_RAM_SIZE);
#endif
    return 0;
}
