/* Reads and writes through NULL pointers, as the PS1 does them.
 *
 * On the PS1, address 0x00000000-0x0000FFFF is the first 64 KiB of main RAM
 * (the kernel's area), so game code that reads `node->field` with node NULL
 * just gets a word of kernel RAM - MenuInput_DispatchQueuedEvents and
 * MenuWidget_EaseNodePosition read a field before checking the pointer, and
 * the floor code reads `floor->region` from a NULL floor. A PC process
 * cannot map that page, so such an access faults. The fault handlers in
 * pe_host_main.c pass it here: if the faulting instruction is a plain load
 * or store (the forms GCC emits for game code at -O0) in this program's own
 * code, it is carried out on a 64 KiB stand-in for that kernel RAM and the
 * program continues after it.
 *
 * Contents of the stand-in: psx-spx "Kernel (BIOS)" memory map documents
 * 0x64 = 0 and 0x68 = 0xFF, and 0x60 = RAM size in MB "(2 or 8)" - which of
 * the two a retail BIOS writes is not stated there; 2 is used. Everything
 * else starts as 0. The real values there (the garbage area at 0x00-0x0F, the
 * "unused/reserved" area at 0x10-0x3F, kernel tables further up) are NOT
 * known here, so code that uses what it reads may behave differently from
 * a console.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define LOW_SIZE 0x10000u

static uint8_t g_low[LOW_SIZE];
static int g_low_init;

static void low_init(void) {
    uint32_t v;
    g_low_init = 1;
    v = 2;
    memcpy(g_low + 0x60, &v, 4);
    v = 0;
    memcpy(g_low + 0x64, &v, 4);
    v = 0xFF;
    memcpy(g_low + 0x68, &v, 4);
}

static uint32_t rd(uint32_t a, int size) {
    uint32_t v = 0;
    memcpy(&v, g_low + a, (size_t)size);
    return v;
}

static void wr(uint32_t a, int size, uint32_t v) { memcpy(g_low + a, &v, (size_t)size); }

/* reg8 encoding: 0-3 low bytes of eax..ebx, 4-7 high bytes (ah..bh). */
static void set_reg8(uint32_t *r[8], int n, uint32_t v) {
    if (n < 4) {
        *r[n] = (*r[n] & ~0xFFu) | (v & 0xFF);
    } else {
        *r[n - 4] = (*r[n - 4] & ~0xFF00u) | ((v & 0xFF) << 8);
    }
}

static uint32_t get_reg8(uint32_t *r[8], int n) { return n < 4 ? (*r[n] & 0xFF) : ((*r[n - 4] >> 8) & 0xFF); }

/* Emulates the instruction at `ip`, which touched `addr` (< 64 KiB).
 * r: eax, ecx, edx, ebx, esp, ebp, esi, edi. Returns its length, or 0 when
 * it is not one of the handled forms (the fault is then reported). */
int pe_host_lowmem_emulate(const uint8_t *ip, uint32_t addr, uint32_t *r[8]) {
    const uint8_t *p = ip;
    int opsize16 = 0, two = 0, op, modrm, mod, reg, rm, len, size;
    uint32_t imm = 0;
    if (!g_low_init) {
        low_init();
    }
    if (*p == 0x66) {
        opsize16 = 1;
        p++;
    }
    op = *p++;
    if (op == 0x0F) {
        two = 1;
        op = *p++;
    }
    modrm = *p++;
    mod = modrm >> 6;
    reg = (modrm >> 3) & 7;
    rm = modrm & 7;
    if (mod == 3) {
        return 0;
    }
    if (rm == 4) {
        int base = *p++ & 7;
        if (mod == 0 && base == 5) {
            p += 4;
        }
    } else if (mod == 0 && rm == 5) {
        p += 4;
    }
    if (mod == 1) {
        p += 1;
    } else if (mod == 2) {
        p += 4;
    }
    if (!two) {
        switch (op) {
        case 0x8B: /* mov r, m */
            size = opsize16 ? 2 : 4;
            if (addr + size > LOW_SIZE) return 0;
            if (size == 2) {
                *r[reg] = (*r[reg] & 0xFFFF0000u) | rd(addr, 2);
            } else {
                *r[reg] = rd(addr, 4);
            }
            break;
        case 0x8A: /* mov r8, m8 */
            if (addr + 1 > LOW_SIZE) return 0;
            set_reg8(r, reg, rd(addr, 1));
            break;
        case 0x89: /* mov m, r */
            size = opsize16 ? 2 : 4;
            if (addr + size > LOW_SIZE) return 0;
            wr(addr, size, *r[reg]);
            break;
        case 0x88: /* mov m8, r8 */
            if (addr + 1 > LOW_SIZE) return 0;
            wr(addr, 1, get_reg8(r, reg));
            break;
        case 0xC7: /* mov m, imm */
            if (reg != 0) return 0;
            size = opsize16 ? 2 : 4;
            if (addr + size > LOW_SIZE) return 0;
            memcpy(&imm, p, (size_t)size);
            p += size;
            wr(addr, size, imm);
            break;
        case 0xC6: /* mov m8, imm8 */
            if (reg != 0 || addr + 1 > LOW_SIZE) return 0;
            wr(addr, 1, *p++);
            break;
        default:
            return 0;
        }
    } else {
        uint32_t v;
        switch (op) {
        case 0xB6: case 0xBE: /* movzx / movsx r, m8 */
            if (addr + 1 > LOW_SIZE) return 0;
            v = rd(addr, 1);
            if (op == 0xBE) v = (uint32_t)(int32_t)(int8_t)v;
            break;
        case 0xB7: case 0xBF: /* movzx / movsx r, m16 */
            if (addr + 2 > LOW_SIZE) return 0;
            v = rd(addr, 2);
            if (op == 0xBF) v = (uint32_t)(int32_t)(int16_t)v;
            break;
        default:
            return 0;
        }
        if (opsize16) {
            *r[reg] = (*r[reg] & 0xFFFF0000u) | (v & 0xFFFF);
        } else {
            *r[reg] = v;
        }
    }
    len = (int)(p - ip);
    {
        /* Name each place once, so a log shows where the game does this. */
        static const uint8_t *seen[64];
        static int n;
        int i;
        for (i = 0; i < n && seen[i] != ip; i++) {
        }
        if (i == n && n < 64) {
            seen[n++] = ip;
            fprintf(stderr, "pe: NULL-pointer access to PS1 address 0x%04X at eip=0x%08X "
                            "(done on the kernel-RAM stand-in)\n",
                    (unsigned)addr, (unsigned)(uintptr_t)ip);
        }
    }
    return len;
}
