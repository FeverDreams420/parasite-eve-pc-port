/* Code the decomp keeps as data words, run from your own executable.
 *
 * A few routines of the main executable sit in its data section, and the
 * decomp lists them only as words (asm/USA/main/data/main/dtail_0D0860.data.s:
 * dlabel func_800E0060, func_800E00CC, func_800E01BC and the routines they
 * call, up to the data at D_800E0808); the game's C calls them by address.
 * The program holds no copy of them: at start-up the words are taken from
 * the SLUS_006.62 just loaded from the disc (before the jump stubs are
 * written over the entry points) and a small MIPS interpreter runs them.
 *
 * The interpreter follows tools/pc/mips_to_c.py instruction for instruction:
 * the same register file, the same private stack (four further arguments
 * stored 16 bytes above $sp), branches that evaluate their condition before
 * the delay slot, no load delay, trapping add/addi/sub that wrap, COP2
 * through PsyZ's GTE and COP0 Status as a variable. Every word of the range
 * is decoded at start-up, so an instruction outside that set stops the
 * program at once instead of in the middle of a game.
 */
#include "pe_host_func_table.h"
#include "pe_host_prelude.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

double pe_host_now_s(void);

#define DC_START 0x800E0060u
#define DC_END 0x800E0808u
#define DC_WORDS ((DC_END - DC_START) / 4)

static uint32_t g_code[DC_WORDS];
static int g_depth;
static unsigned g_calls;
static unsigned long long g_insns;
static double g_total_s, g_worst_s;

static void dc_fail(uint32_t pc, uint32_t w, const char *what) {
    fprintf(stderr, "pe: data code at 0x%08X (word 0x%08X): %s\n", pc, w, what);
    abort();
}

static uint32_t fetch(uint32_t pc) {
    if (pc < DC_START || pc >= DC_END || (pc & 3)) {
        dc_fail(pc, 0, "outside the routines kept as data");
    }
    return g_code[(pc - DC_START) / 4];
}

static int in_range(uint32_t a) { return a >= DC_START && a < DC_END; }

static uint32_t call_host(uint32_t target, uint32_t *r) {
    return ((pe_asm_fn)(uintptr_t)target)(r[4], r[5], r[6], r[7]);
}

enum { K_BAD, K_SIMPLE, K_BRANCH };

/* What a word is: an instruction the interpreter runs inline, a branch or
 * jump (followed by its delay slot), or one it does not support. */
static int kind(uint32_t w) {
    uint32_t op = w >> 26, fn = w & 63, rs = (w >> 21) & 31, rt = (w >> 16) & 31;
    switch (op) {
    case 0:
        switch (fn) {
        case 0: case 2: case 3: case 4: case 6: case 7: case 13: case 16: case 17: case 18: case 19:
        case 24: case 25: case 26: case 27: case 32: case 33: case 34: case 35: case 36: case 37:
        case 38: case 39: case 42: case 43:
            return K_SIMPLE;
        case 8: case 9:
            return K_BRANCH;
        }
        return K_BAD;
    case 1:
        return rt == 0 || rt == 1 ? K_BRANCH : K_BAD;
    case 2: case 3: case 4: case 5: case 6: case 7:
        return K_BRANCH;
    case 8: case 9: case 10: case 11: case 12: case 13: case 14: case 15:
    case 32: case 33: case 35: case 36: case 37: case 40: case 41: case 43: case 50: case 58:
        return K_SIMPLE;
    case 16:
        return (rs == 0 || rs == 4) && ((w >> 11) & 31) == 12 ? K_SIMPLE : K_BAD;
    case 18:
        if (w & (1u << 25)) {
            return K_SIMPLE;
        }
        return rs == 0 || rs == 2 || rs == 4 || rs == 6 ? K_SIMPLE : K_BAD;
    }
    return K_BAD;
}

#define RS r[(w >> 21) & 31]
#define RT r[(w >> 16) & 31]
#define SET(n, v) do { uint32_t v_ = (v); if (n) r[n] = v_; } while (0)
#define IMM ((uint32_t)(int32_t)(int16_t)(w & 0xFFFFu))
#define ADDR ((uintptr_t)(RS + IMM))

/* One instruction that is not a branch (r[32] = HI, r[33] = LO). */
static void simple(uint32_t pc, uint32_t w, uint32_t *r) {
    uint32_t op = w >> 26, rt = (w >> 16) & 31, rd = (w >> 11) & 31, sa = (w >> 6) & 31;
    switch (op) {
    case 0:
        switch (w & 63) {
        case 0: SET(rd, RT << sa); return;
        case 2: SET(rd, RT >> sa); return;
        case 3: SET(rd, (uint32_t)((int32_t)RT >> sa)); return;
        case 4: SET(rd, RT << (RS & 31)); return;
        case 6: SET(rd, RT >> (RS & 31)); return;
        case 7: SET(rd, (uint32_t)((int32_t)RT >> (RS & 31))); return;
        case 13: pe_asm_break("the routines kept as data"); return;
        case 16: SET(rd, r[32]); return;
        case 17: r[32] = RS; return;
        case 18: SET(rd, r[33]); return;
        case 19: r[33] = RS; return;
        case 24: {
            int64_t p = (int64_t)(int32_t)RS * (int32_t)RT;
            r[33] = (uint32_t)p;
            r[32] = (uint32_t)((uint64_t)p >> 32);
            return;
        }
        case 25: {
            uint64_t p = (uint64_t)RS * RT;
            r[33] = (uint32_t)p;
            r[32] = (uint32_t)(p >> 32);
            return;
        }
        case 26: {
            int32_t n = (int32_t)RS, d = (int32_t)RT;
            if (d == 0) {
                r[33] = n >= 0 ? 0xFFFFFFFFu : 1u;
                r[32] = (uint32_t)n;
            } else if (n == INT32_MIN && d == -1) {
                r[33] = (uint32_t)n;
                r[32] = 0;
            } else {
                r[33] = (uint32_t)(n / d);
                r[32] = (uint32_t)(n % d);
            }
            return;
        }
        case 27: {
            uint32_t n = RS, d = RT;
            if (d == 0) {
                r[33] = 0xFFFFFFFFu;
                r[32] = n;
            } else {
                r[33] = n / d;
                r[32] = n % d;
            }
            return;
        }
        case 32: case 33: SET(rd, RS + RT); return;
        case 34: case 35: SET(rd, RS - RT); return;
        case 36: SET(rd, RS & RT); return;
        case 37: SET(rd, RS | RT); return;
        case 38: SET(rd, RS ^ RT); return;
        case 39: SET(rd, ~(RS | RT)); return;
        case 42: SET(rd, (int32_t)RS < (int32_t)RT); return;
        case 43: SET(rd, RS < RT); return;
        }
        break;
    case 8: case 9: SET(rt, RS + IMM); return;
    case 10: SET(rt, (int32_t)RS < (int32_t)IMM); return;
    case 11: SET(rt, RS < IMM); return;
    case 12: SET(rt, RS & (w & 0xFFFFu)); return;
    case 13: SET(rt, RS | (w & 0xFFFFu)); return;
    case 14: SET(rt, RS ^ (w & 0xFFFFu)); return;
    case 15: SET(rt, (w & 0xFFFFu) << 16); return;
    case 16:
        if (((w >> 21) & 31) == 0) {
            SET(rt, pe_cop0_status);
        } else {
            pe_cop0_status = RT;
        }
        return;
    case 18:
        if (w & (1u << 25)) {
            Psyz_GteCommand(w);
            return;
        }
        switch ((w >> 21) & 31) {
        case 0: SET(rt, pe_gte_data_read(rd)); return;
        case 2: SET(rt, pe_gte_ctrl_read(rd)); return;
        case 4: pe_gte_data_write(rd, RT); return;
        case 6: pe_gte_ctrl_write(rd, RT); return;
        }
        break;
    case 32: SET(rt, (uint32_t)(int32_t)*(int8_t *)ADDR); return;
    case 33: SET(rt, (uint32_t)(int32_t)*(int16_t *)ADDR); return;
    case 35: SET(rt, *(uint32_t *)ADDR); return;
    case 36: SET(rt, *(uint8_t *)ADDR); return;
    case 37: SET(rt, *(uint16_t *)ADDR); return;
    case 40: *(uint8_t *)ADDR = (uint8_t)RT; return;
    case 41: *(uint16_t *)ADDR = (uint16_t)RT; return;
    case 43: *(uint32_t *)ADDR = RT; return;
    case 50: pe_gte_data_write(rt, *(uint32_t *)ADDR); return;
    case 58: *(uint32_t *)ADDR = pe_gte_data_read(rt); return;
    }
    dc_fail(pc, w, "unsupported instruction");
}

/* Run from `entry` until it returns to its caller ($ra = 0 on entry, as in
 * the translated wrappers). Calls inside the range share the registers. */
static void run(uint32_t pc, uint32_t *r) {
    for (;;) {
        uint32_t w = fetch(pc), op = w >> 26, target = 0;
        int taken = 0, link = 0;
        if (kind(w) != K_BRANCH) {
            simple(pc, w, r);
            r[0] = 0;
            g_insns++;
            pc += 4;
            continue;
        }
        switch (op) {
        case 0: /* jr, jalr */
            target = RS;
            taken = 1;
            link = (w & 63) == 9;
            break;
        case 1: /* bltz, bgez */
            taken = ((w >> 16) & 1) ? (int32_t)RS >= 0 : (int32_t)RS < 0;
            target = pc + 4 + (IMM << 2);
            break;
        case 2: case 3: /* j, jal */
            target = ((pc + 4) & 0xF0000000u) | ((w & 0x03FFFFFFu) << 2);
            taken = 1;
            link = op == 3;
            break;
        case 4: taken = RS == RT; target = pc + 4 + (IMM << 2); break;
        case 5: taken = RS != RT; target = pc + 4 + (IMM << 2); break;
        case 6: taken = (int32_t)RS <= 0; target = pc + 4 + (IMM << 2); break;
        case 7: taken = (int32_t)RS > 0; target = pc + 4 + (IMM << 2); break;
        }
        {
            uint32_t d = fetch(pc + 4);
            if (kind(d) == K_BRANCH) {
                dc_fail(pc + 4, d, "branch in a delay slot");
            }
            simple(pc + 4, d, r);
            r[0] = 0;
        }
        g_insns += 2;
        if (!taken) {
            pc += 8;
        } else if (link && !in_range(target)) {
            /* a call out of the range: the host function at that address */
            r[2] = call_host(target, r);
            r[3] = 0;
            pc += 8;
        } else if (link) {
            if (op == 0 && ((w >> 11) & 31) != 31) {
                dc_fail(pc, w, "jalr linking a register other than $ra");
            }
            r[31] = pc + 8;
            pc = target;
        } else if (target == 0) {
            return; /* back to the host caller */
        } else if (!in_range(target)) {
            /* jump to another function: it returns to our caller */
            r[2] = call_host(target, r);
            r[3] = 0;
            if (r[31] == 0) {
                return;
            }
            pc = r[31];
        } else {
            pc = target;
        }
    }
}

static uint32_t pe_datacode_run(uint32_t entry, const uint32_t *args) {
    uint32_t r[34] = {0};
    uint32_t stack[260];
    double t0 = g_depth == 0 ? pe_host_now_s() : 0.0;
    memcpy(&r[4], args, 4 * sizeof(uint32_t));
    memcpy(&stack[244], args + 4, 4 * sizeof(uint32_t));
    r[29] = (uint32_t)(uintptr_t)&stack[240];
    g_depth++;
    run(entry, r);
    g_depth--;
    if (g_depth == 0) {
        double s = pe_host_now_s() - t0;
        g_calls++;
        g_total_s += s;
        g_worst_s = s > g_worst_s ? s : g_worst_s;
    }
    return r[2];
}

#define ENTRY(addr) \
    static uint32_t pe_datacode_##addr(uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, \
                                       uint32_t a5, uint32_t a6, uint32_t a7) { \
        uint32_t a[8] = {a0, a1, a2, a3, a4, a5, a6, a7}; \
        return pe_datacode_run(0x##addr##u, a); \
    }
/* the dlabels of the range, and the jal targets inside it */
ENTRY(800E0060)
ENTRY(800E00CC)
ENTRY(800E01BC)
ENTRY(800E026C)
ENTRY(800E03A0)
ENTRY(800E051C)

const PeHostFunc pe_data_code_table[] = {
    {0x800E0060u, (void *)pe_datacode_800E0060},
    {0x800E00CCu, (void *)pe_datacode_800E00CC},
    {0x800E01BCu, (void *)pe_datacode_800E01BC},
    {0x800E026Cu, (void *)pe_datacode_800E026C},
    {0x800E03A0u, (void *)pe_datacode_800E03A0},
    {0x800E051Cu, (void *)pe_datacode_800E051C},
};
const unsigned int pe_data_code_count = sizeof(pe_data_code_table) / sizeof(pe_data_code_table[0]);

/* Keep the routines' words from the loaded executable; call before the jump
 * stubs are installed. Returns -1 if a word is not a supported instruction. */
int pe_host_datacode_init(void) {
    unsigned i;
    memcpy(g_code, (const void *)(uintptr_t)DC_START, sizeof(g_code));
    for (i = 0; i < DC_WORDS; i++) {
        if (kind(g_code[i]) == K_BAD) {
            fprintf(stderr, "pe: data code at 0x%08X: unsupported instruction 0x%08X\n",
                    DC_START + 4 * i, g_code[i]);
            return -1;
        }
    }
    return 0;
}

/* Calls, instructions and time since the last take (pc/host/psyq/forward.c
 * logs them with the pacing lines). */
void pe_host_datacode_take(unsigned *calls, unsigned long long *insns, double *total_ms, double *worst_ms) {
    *calls = g_calls;
    *insns = g_insns;
    *total_ms = g_total_s * 1000.0;
    *worst_ms = g_worst_s * 1000.0;
    g_calls = 0;
    g_insns = 0;
    g_total_s = g_worst_s = 0.0;
}
