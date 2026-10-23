/* Software MDEC (motion decoder) for the movie player.
 *
 * The game's movie code (sys_reset overlay, PSY-Q libpress) drives the MDEC
 * chip through its registers and DMA channels 0 (in) and 1 (out). Those
 * register routines are replaced (pc/host/overlay_overrides.txt) by:
 *   pe_mdec_reset(mode)       func_8010C0FC   reset, mode 0 also reloads tables
 *   pe_mdec_in(data, size)    func_8010C1EC   send a command + parameters
 *   pe_mdec_out(buf, size)    func_8010C27C   receive `size` words of pixels
 *   pe_mdec_in_sync()         func_8010C308   wait for input  (always done)
 *   pe_mdec_out_sync()        func_8010C39C   wait for output (always done)
 *   pe_mdec_status()          func_8010C430   status register (idle)
 * Output is produced at once; the DMA channel 1 completion callback the game
 * installed with DMACallback(1, f) is then called, as the DMA interrupt would.
 *
 * Decoding follows psx-spx "Macroblock Decoder (MDEC)":
 *  - commands: bits 31-29 = 1 decode (28-27 depth 0=4,1=8,2=24,3=15 bit;
 *    26 signed; 25 bit-15 set; 15-0 parameter words), 2 set quant tables
 *    (bit 0: also the colour table), 3 set scale table (64 halfwords);
 *  - rl_decode_block: skip FE00h padding; q_scale = (n >> 10) & 3Fh;
 *    DC val = signed10bit(n & 3FFh) * qt[k]; AC
 *    val = (signed10bit(n & 3FFh) * qt[k] * q_scale + 4) / 8;
 *    if q_scale = 0: val = signed10bit(n & 3FFh) * 2; val = minmax(-400h, 3FFh);
 *    stored at blk[zagzig[k]] (q_scale > 0) or blk[k] (q_scale = 0);
 *    k += ((n >> 10) & 3Fh) + 1; block ends when k > 63;
 *  - real_idct_core: two passes of
 *    dst[x+y*8] = (sum(src[y+z*8] * (scale[x+z*8] / 8)) + 0FFFh) / 2000h,
 *    swapping src/dst after each pass;
 *  - colour: blocks Cr, Cb, Y1(0,0), Y2(8,0), Y3(0,8), Y4(8,8); Cr/Cb use the
 *    colour quant table, Y the luminance table; per pixel
 *    R = Cr[(x+xx)/2 + (y+yy)/2*8], B = Cb[...], G = -0.3437*B - 0.7143*R,
 *    R = 1.402*R, B = 1.772*B, each channel = minmax(-128, 127, Y + c),
 *    unsigned output = value xor 80h; written to dst[(x+xx) + (y+yy)*16].
 * Not stated on psx-spx and chosen here: the divisions round like the
 * hardware's shifts (arithmetic shift), and 15-bit output keeps the top five
 * bits of each 8-bit channel.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

typedef void (*DmaCallback)(void);
DmaCallback pe_host_dma_callback(int channel);

static const uint8_t k_zigzag[64] = {
    0, 1, 5, 6, 14, 15, 27, 28, 2, 4, 7, 13, 16, 26, 29, 42,
    3, 8, 12, 17, 25, 30, 41, 43, 9, 11, 18, 24, 31, 40, 44, 53,
    10, 19, 23, 32, 39, 45, 52, 54, 20, 22, 33, 38, 46, 51, 55, 60,
    21, 34, 37, 47, 50, 56, 59, 61, 35, 36, 48, 49, 57, 58, 62, 63};

static uint8_t g_zagzig[64];
static uint8_t g_qt_y[64], g_qt_uv[64];
static int16_t g_scale[64];

/* Pending decode command. */
static uint32_t g_cmd;
static const uint16_t *g_src;
static const uint16_t *g_src_end;

static int signed10(int v) { return (v & 0x200) ? v - 0x400 : v; }

static void init_tables(void) {
    static int done;
    int i;
    if (done) {
        return;
    }
    done = 1;
    for (i = 0; i < 64; i++) {
        g_zagzig[k_zigzag[i]] = (uint8_t)i;
    }
}

static void idct(int32_t *blk) {
    int32_t tmp[64];
    int32_t *src = blk, *dst = tmp, *t;
    int pass, x, y, z;
    for (pass = 0; pass < 2; pass++) {
        for (x = 0; x < 8; x++) {
            for (y = 0; y < 8; y++) {
                int64_t sum = 0;
                for (z = 0; z < 8; z++) {
                    sum += (int64_t)src[y + z * 8] * (g_scale[x + z * 8] >> 3);
                }
                dst[x + y * 8] = (int32_t)((sum + 0xFFF) >> 13);
            }
        }
        t = src;
        src = dst;
        dst = t;
    }
    /* After two passes src is blk again. */
}

/* Returns 0 at the end of the input. */
static int rl_decode_block(int32_t *blk, const uint8_t *qt) {
    int k, q_scale, val;
    uint16_t n;
    memset(blk, 0, 64 * sizeof(int32_t));
    do {
        if (g_src >= g_src_end) {
            return 0;
        }
        n = *g_src++;
    } while (n == 0xFE00);
    k = 0;
    q_scale = (n >> 10) & 0x3F;
    val = signed10(n & 0x3FF) * qt[k];
    for (;;) {
        if (q_scale == 0) {
            val = signed10(n & 0x3FF) * 2;
        }
        if (val < -0x400) {
            val = -0x400;
        }
        if (val > 0x3FF) {
            val = 0x3FF;
        }
        if (q_scale > 0) {
            blk[g_zagzig[k]] = val;
        } else {
            blk[k] = val;
        }
        if (g_src >= g_src_end) {
            break;
        }
        n = *g_src++;
        k = k + ((n >> 10) & 0x3F) + 1;
        if (k > 63) {
            break;
        }
        val = (signed10(n & 0x3FF) * qt[k] * q_scale + 4) >> 3;
    }
    idct(blk);
    return 1;
}

static int clamp8(int v) { return v < -128 ? -128 : (v > 127 ? 127 : v); }

/* Decode one colour macroblock into 16x16 pixels of 0x00BBGGRR. */
static int decode_macroblock(uint32_t *out) {
    int32_t cr[64], cb[64], yb[64];
    static const int qx[4] = {0, 8, 0, 8}, qy[4] = {0, 0, 8, 8};
    int q, x, y, sgn = (g_cmd >> 26) & 1;
    if (!rl_decode_block(cr, g_qt_uv) || !rl_decode_block(cb, g_qt_uv)) {
        return 0;
    }
    for (q = 0; q < 4; q++) {
        if (!rl_decode_block(yb, g_qt_y)) {
            return 0;
        }
        for (y = 0; y < 8; y++) {
            for (x = 0; x < 8; x++) {
                int xx = qx[q], yy = qy[q];
                double R = cr[(x + xx) / 2 + (y + yy) / 2 * 8];
                double B = cb[(x + xx) / 2 + (y + yy) / 2 * 8];
                double G = (-0.3437 * B) + (-0.7143 * R);
                int Y = yb[x + y * 8];
                int r, g, b;
                R = 1.402 * R;
                B = 1.772 * B;
                r = clamp8(Y + (int)lrint(R));
                g = clamp8(Y + (int)lrint(G));
                b = clamp8(Y + (int)lrint(B));
                if (!sgn) {
                    r ^= 0x80;
                    g ^= 0x80;
                    b ^= 0x80;
                }
                out[(x + xx) + (y + yy) * 16] =
                    (uint32_t)(r & 0xFF) | (uint32_t)(g & 0xFF) << 8 | (uint32_t)(b & 0xFF) << 16;
            }
        }
    }
    return 1;
}

void pe_mdec_in(uint32_t *data, uint32_t size);

/* func_8010C0FC: mode 0 also resends the default tables the retail code sends
 * (the quant table command at 0x8010DA0C and the scale table command at
 * 0x8010DA90 in the sys_reset overlay, each 0x20 words). */
void pe_mdec_reset(int mode) {
    init_tables();
    g_src = g_src_end = NULL;
    if (mode == 0) {
        pe_mdec_in((uint32_t *)(uintptr_t)0x8010DA0Cu, 0x20);
        pe_mdec_in((uint32_t *)(uintptr_t)0x8010DA90u, 0x20);
    }
}

void pe_mdec_in(uint32_t *data, uint32_t size) {
    uint32_t cmd = data[0];
    static int logged;
    init_tables();
    if (getenv("PE_LOG_MDEC") && logged < 200) {
        logged++;
        fprintf(stderr, "pe: mdec_in cmd 0x%08X size 0x%X data %08X %08X %08X %08X\n", cmd, size,
                data[1], data[2], data[3], data[4]);
        if ((cmd >> 29) == 1 && getenv("PE_MDEC_DUMP") && logged == atoi(getenv("PE_MDEC_DUMP_N") ? getenv("PE_MDEC_DUMP_N") : "60")) {
            FILE *f = fopen(getenv("PE_MDEC_DUMP"), "wb");
            if (f) {
                fwrite(data, 4, 1 + (cmd & 0xFFFF), f);
                fwrite(g_qt_y, 1, 64, f);
                fwrite(g_qt_uv, 1, 64, f);
                fwrite(g_scale, 2, 64, f);
                fclose(f);
            }
        }
    }
    switch (cmd >> 29) {
    case 1:
        g_cmd = cmd;
        g_src = (const uint16_t *)(data + 1);
        g_src_end = g_src + 2 * (cmd & 0xFFFF);
        break;
    case 2:
        memcpy(g_qt_y, data + 1, 64);
        if (cmd & 1) {
            memcpy(g_qt_uv, (uint8_t *)(data + 1) + 64, 64);
        }
        break;
    case 3:
        memcpy(g_scale, data + 1, sizeof(g_scale));
        break;
    default:
        break;
    }
    (void)size;
    {
        DmaCallback cb = pe_host_dma_callback(0);
        if (cb) {
            cb();
        }
    }
}

double pe_host_now_s(void);
static double g_mdec_s; /* decoding time, for the pacing log (forward.c) */
static unsigned g_mdec_n;
static unsigned g_mdec_total; /* decodes since start, for the movie switch (settings.c) */
unsigned pe_mdec_decodes(void) { return g_mdec_total; }
void pe_mdec_take(double *ms, unsigned *n) {
    *ms = g_mdec_s * 1000.0;
    *n = g_mdec_n;
    g_mdec_s = 0.0;
    g_mdec_n = 0;
}
void pe_mdec_out(uint32_t *buf, uint32_t size) {
    int depth = (g_cmd >> 27) & 3;
    static int logged_out;
    if (getenv("PE_LOG_MDEC") && logged_out < 60) {
        logged_out++;
        fprintf(stderr, "pe: mdec_out buf 0x%08X size 0x%X (input left %d halfwords)\n",
                (unsigned)(uintptr_t)buf, size, g_src ? (int)(g_src_end - g_src) : -1);
    }
    uint32_t px[256];
    uint8_t *dst = (uint8_t *)buf;
    uint32_t bytes = size * 4, done = 0;
    int i;
    double t_dec = pe_host_now_s();
    while (done < bytes) {
        if (!g_src || !decode_macroblock(px)) {
            memset(dst + done, 0, bytes - done);
            break;
        }
        if (depth == 2) { /* 24-bit: R, G, B bytes */
            for (i = 0; i < 256 && done + 3 <= bytes; i++) {
                dst[done++] = (uint8_t)px[i];
                dst[done++] = (uint8_t)(px[i] >> 8);
                dst[done++] = (uint8_t)(px[i] >> 16);
            }
        } else { /* 15-bit */
            uint16_t stp = (g_cmd & 0x02000000) ? 0x8000 : 0;
            for (i = 0; i < 256 && done + 2 <= bytes; i++) {
                uint16_t v = (uint16_t)(((px[i] & 0xFF) >> 3) | (((px[i] >> 8) & 0xFF) >> 3) << 5 |
                                        (((px[i] >> 16) & 0xFF) >> 3) << 10 | stp);
                memcpy(dst + done, &v, 2);
                done += 2;
            }
        }
    }
    g_mdec_s += pe_host_now_s() - t_dec;
    g_mdec_n++;
    g_mdec_total++;
    /* On hardware the DMA channel 1 interrupt fires after the transfer, so
     * the callback that started this transfer finishes first (it uploads the
     * previous slice after starting the next). The output is produced at
     * once here, so completions are queued and run one after another rather
     * than nested inside the callback that started them. */
    {
        static int in_callback, pending;
        pending++;
        if (in_callback) {
            return;
        }
        while (pending > 0) {
            DmaCallback cb = pe_host_dma_callback(1);
            pending--;
            if (!cb) {
                continue;
            }
            in_callback = 1;
            cb();
            in_callback = 0;
        }
    }
}

int pe_mdec_in_sync(void) { return 0; }
int pe_mdec_out_sync(void) { return 0; }
uint32_t pe_mdec_status(void) { return 0x80040000u; /* output FIFO empty, idle */ }
