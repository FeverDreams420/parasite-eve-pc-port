/* GTE register access for translated game code.
 *
 * PsyZ's register bridge (Psyz_GteDataRead/Write) covers data registers 0-27.
 * The hardware registers it does not model are handled here, following
 * psx-spx ("GTE Data Register Summary"):
 *   15 SXYP  read = SXY2; write pushes the screen-XY FIFO
 *            (SXY0 <- SXY1, SXY1 <- SXY2, SXY2 <- value)
 *   28 IRGB  write: IR1..IR3 = (5-bit fields) * 0x80; read returns ORGB
 *   29 ORGB  read: IR1..IR3 / 0x80, each saturated to 0..0x1F
 *   30 LZCS  read/write
 *   31 LZCR  read: number of leading bits equal to LZCS bit 31 (1..32)
 * Without these, routines such as SquareRoot0 and VectorNormal (which use
 * LZCS/LZCR) return wrong results.
 */
#include <stdint.h>

unsigned int Psyz_GteDataRead(unsigned reg);
void Psyz_GteDataWrite(unsigned reg, unsigned int value);
unsigned int Psyz_GteCtrlRead(unsigned reg);
void Psyz_GteCtrlWrite(unsigned reg, unsigned int value);

static uint32_t g_lzcs;

static uint32_t lzcr(uint32_t v) {
    uint32_t n = 0;
    uint32_t bit = v >> 31;
    while (n < 32 && ((v >> (31 - n)) & 1) == bit) {
        n++;
    }
    return n;
}

static uint32_t sat5(int32_t ir) {
    int32_t c = ir / 0x80;
    if (c < 0) {
        c = 0;
    }
    if (c > 0x1F) {
        c = 0x1F;
    }
    return (uint32_t)c;
}

static uint32_t orgb(void) {
    return sat5((int32_t)Psyz_GteDataRead(9)) | sat5((int32_t)Psyz_GteDataRead(10)) << 5 |
           sat5((int32_t)Psyz_GteDataRead(11)) << 10;
}

unsigned int pe_gte_data_read(unsigned reg) {
    switch (reg) {
    case 15:
        return Psyz_GteDataRead(14);
    case 28:
    case 29:
        return orgb();
    case 30:
        return g_lzcs;
    case 31:
        return lzcr(g_lzcs);
    default:
        return Psyz_GteDataRead(reg);
    }
}

void pe_gte_data_write(unsigned reg, unsigned int v) {
    switch (reg) {
    case 15:
        Psyz_GteDataWrite(12, Psyz_GteDataRead(13));
        Psyz_GteDataWrite(13, Psyz_GteDataRead(14));
        Psyz_GteDataWrite(14, v);
        Psyz_GteDataWrite(15, v);
        break;
    case 28:
        Psyz_GteDataWrite(9, (v & 0x1F) * 0x80);
        Psyz_GteDataWrite(10, ((v >> 5) & 0x1F) * 0x80);
        Psyz_GteDataWrite(11, ((v >> 10) & 0x1F) * 0x80);
        break;
    case 29:
    case 31:
        break; /* read-only */
    case 30:
        g_lzcs = v;
        break;
    default:
        Psyz_GteDataWrite(reg, v);
        break;
    }
}

unsigned int pe_gte_ctrl_read(unsigned reg) { return Psyz_GteCtrlRead(reg); }
void pe_gte_ctrl_write(unsigned reg, unsigned int v) { Psyz_GteCtrlWrite(reg, v); }

/* COP0 Status as seen by translated assembler (interrupt masks). */
uint32_t pe_cop0_status = 0x40000401u;
