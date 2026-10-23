/* PSY-Q GPU calls that involve ordering tables, primitives or DRAWENV.
 *
 * The game builds primitives in PS1 RAM with the PS1 tag: one word,
 * bits 0-23 = address of the next packet, bits 24-31 = number of data words.
 * PsyZ's own libgpu uses a two-word tag under __psyz, so these calls walk the
 * PS1 chain here and hand the data words to PsyZ's GP0 interpreter
 * (Psyz_GpuWriteGP0 / Psyz_GpuExeque).
 */
#include <psyz.h>
#include <libgpu.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void Psyz_GpuWriteGP0(unsigned int word);
int Psyz_GpuExeque(void);

#define PS1_TERM 0x00FFFFFFu
#define QUEUE_ROOM 0x3F00 /* PsyZ queue holds 0x4000 words */

static int g_queued;

/* Debug: PE_LOG_GPU=first:last logs, for those frame numbers, how many
 * packets of each GP0 command (first data word >> 24) were sent. */
unsigned int pe_host_frame_number(void);
static unsigned int g_hist[256], g_hist_frame = ~0u;
static int gpu_log_on(void) {
    static int init, first = -1, last = -1;
    unsigned int f;
    if (!init) {
        const char *e = getenv("PE_LOG_GPU");
        init = 1;
        if (e) {
            first = atoi(e);
            last = strchr(e, ':') ? atoi(strchr(e, ':') + 1) : first;
        }
    }
    if (first < 0) {
        return 0;
    }
    f = pe_host_frame_number();
    return (int)f >= first && (int)f <= last;
}
static void gpu_log_flush_frame(void) {
    unsigned int f = pe_host_frame_number(), i;
    if (f == g_hist_frame) {
        return;
    }
    if (g_hist_frame != ~0u) {
        fprintf(stderr, "pe: gpu frame %u:", g_hist_frame);
        for (i = 0; i < 256; i++) {
            if (g_hist[i]) {
                fprintf(stderr, " %02X:%u", i, g_hist[i]);
            }
        }
        fprintf(stderr, "\n");
    }
    memset(g_hist, 0, sizeof(g_hist));
    g_hist_frame = f;
}
/* F12 snapshot (hotkeys.c): every packet of one frame goes to a file. */
static FILE *g_snap_file;
static unsigned int g_snap_frame, g_snap_packets;
/* Packets recorded so far; the snapshot waits for a frame that drew some
 * (VSync can be called more than once per game frame). */
unsigned int pe_host_snapshot_gpu_packets(void) { return g_snap_packets; }
void pe_host_snapshot_gpu_begin(unsigned int frame) {
    char path[64];
    if (g_snap_file) {
        fclose(g_snap_file);
    }
    snprintf(path, sizeof(path), "snapshot-%05u-gpu.txt", frame);
    g_snap_file = fopen(path, "w");
    g_snap_frame = frame;
    g_snap_packets = 0;
}
void pe_host_snapshot_gpu_end(void) {
    if (g_snap_file) {
        fclose(g_snap_file);
        g_snap_file = NULL;
    }
}

void pe_host_gpu_log_note(const char *what, int a, int b, int c, int d) {
    if (g_snap_file) {
        fprintf(g_snap_file, "%s %d,%d %d,%d\n", what, a, b, c, d);
    }
    if (gpu_log_on()) {
        fprintf(stderr, "pe: gpu frame %u: %s %d,%d %d,%d\n", pe_host_frame_number(), what, a, b, c, d);
    }
}

int pe_host_widescreen_margin(void);

/* Widescreen (settings.c, PsyZ 0009): an untextured rectangle or polygon that
 * covers the whole 320-pixel width (fades, flashes, dimmed backdrops) is
 * widened by the margin on both sides, so it covers the 16:9 picture too.
 * Works on a copy: the game's packet in PS1 RAM is left as it was. */
static const uint32_t *widen_full_width(const uint32_t *p, uint32_t len, uint32_t *copy) {
    uint32_t op, k, n, o = 0;
    int m = pe_host_widescreen_margin(), minx = 9999, maxx = -9999, idx[4], step;
    if (m == 0 || len > 12) {
        return p;
    }
    /* the primitive can follow mode words in the same packet: the dialogue
     * box is E1 (texture page / semi-transparency), a NOP, then a 320-wide
     * semi-transparent TILE, which was left 4:3 inside the 16:9 picture */
    while (o + 1 < len && ((p[1 + o] >> 24) == 0 || ((p[1 + o] >> 24) >= 0xE1 && (p[1 + o] >> 24) <= 0xE6))) {
        o++;
    }
    op = p[1 + o] >> 24;
    if (op >= 0x60 && op <= 0x63) { /* TILE: color, xy, wh */
        int x = (int16_t)(p[2 + o] & 0xFFFF), w = (int)(p[3 + o] & 0xFFFF);
        if (len < 3 + o || x > 0 || x + w < 320) {
            return p;
        }
        memcpy(copy, p, (len + 1) * 4);
        copy[2 + o] = (copy[2 + o] & 0xFFFF0000u) | ((uint32_t)(x - m) & 0xFFFF);
        copy[3 + o] = (copy[3 + o] & 0xFFFF0000u) | ((uint32_t)(w + 2 * m) & 0xFFFF);
        return copy;
    }
    if (op < 0x20 || op > 0x3F || (op & 4)) { /* untextured polygons only */
        return p;
    }
    n = (op & 8) ? 4 : 3;
    step = (op & 0x10) ? 2 : 1; /* gouraud: a colour word before each later vertex */
    for (k = 0; k < n; k++) {
        idx[k] = 2 + (int)o + (int)k * step;
        if ((uint32_t)idx[k] > len) {
            return p;
        }
        {
            int x = (int16_t)(p[idx[k]] & 0xFFFF);
            minx = x < minx ? x : minx;
            maxx = x > maxx ? x : maxx;
        }
    }
    if (minx > 0 || maxx < 320) {
        return p;
    }
    memcpy(copy, p, (len + 1) * 4);
    for (k = 0; k < n; k++) {
        int x = (int16_t)(copy[idx[k]] & 0xFFFF);
        x = x <= 0 ? x - m : (x >= 320 ? x + m : x);
        copy[idx[k]] = (copy[idx[k]] & 0xFFFF0000u) | ((uint32_t)x & 0xFFFF);
    }
    return copy;
}

static void push_packet(const uint32_t *p) {
    uint32_t len = p[0] >> 24;
    uint32_t i;
    uint32_t copy[16];
    if (len == 0) {
        return;
    }
    p = widen_full_width(p, len, copy);
    if (g_snap_file) {
        uint32_t k;
        g_snap_packets++;
        for (k = 1; k <= len; k++) {
            fprintf(g_snap_file, "%08X%c", p[k], k == len ? '\n' : ' ');
        }
    }
    if (gpu_log_on()) {
        gpu_log_flush_frame();
        g_hist[p[1] >> 24]++;
        if (getenv("PE_DUMP_GPU")) {
            uint32_t k;
            fprintf(stderr, "pe: gp0");
            for (k = 1; k <= len; k++) {
                fprintf(stderr, " %08X", p[k]);
            }
            fprintf(stderr, "\n");
        }
    }
    if (g_queued + (int)len > QUEUE_ROOM) {
        Psyz_GpuExeque();
        g_queued = 0;
    }
    for (i = 1; i <= len; i++) {
        /* where the word is in PS1 RAM (not for a widened copy), so precise
         * geometry can find the polygon's vertices (precise_anim.c) */
        if (p == copy) {
            Psyz_GpuWriteGP0(p[i]);
        } else {
            Psyz_GpuWriteGP0At(p[i], &p[i]);
        }
    }
    g_queued += (int)len;
}

static void flush(void) {
    Psyz_GpuExeque();
    g_queued = 0;
}

/* Walk a PS1 ordering table / packet chain starting at p. */
static void walk(const uint32_t *p) {
    unsigned int guard = 0;
    for (;;) {
        uint32_t next;
        push_packet(p);
        next = p[0] & 0x00FFFFFFu;
        if (next == PS1_TERM) {
            break;
        }
        p = (const uint32_t *)(uintptr_t)(0x80000000u | next);
        if (++guard > 0x100000u) {
            fprintf(stderr, "pe: ordering table does not terminate\n");
            break;
        }
    }
    flush();
}

void pe_host_ws_commit(void);
double pe_host_now_s(void);
static double g_draw_s; /* pacing log (forward.c): time spent drawing */
double pe_host_draw_s_take(void) {
    double d = g_draw_s;
    g_draw_s = 0.0;
    return d;
}
void pe_psyq_DrawOTag(uint32_t *ot) {
    double t0 = pe_host_now_s();
    pe_host_ws_commit(); /* widescreen window for this buffer (widescreen.c) */
    walk(ot);
    g_draw_s += pe_host_now_s() - t0;
}

void pe_psyq_DrawPrim(uint32_t *p) {
    push_packet(p);
    flush();
}

/* DMA channel 6 (OTC) fills the table backwards: ot[i] links to ot[i-1],
 * ot[0] is the terminator. */
uint32_t *pe_psyq_ClearOTagR(uint32_t *ot, int n) {
    int i;
    if (n <= 0) {
        return ot;
    }
    ot[0] = PS1_TERM;
    for (i = 1; i < n; i++) {
        ot[i] = (uint32_t)(uintptr_t)&ot[i - 1] & 0x00FFFFFFu;
    }
    return ot;
}

/* The game's DRAWENV: PS1 layout, 0x1C bytes of settings then a DR_ENV. */
typedef struct {
    short clip_x, clip_y, clip_w, clip_h;
    short ofs[2];
    short tw_x, tw_y, tw_w, tw_h;
    unsigned short tpage;
    unsigned char dtd, dfe, isbg, r0, g0, b0;
} GameDrawEnv;

typedef char game_drawenv_size_check[sizeof(GameDrawEnv) == 0x1C ? 1 : -1];

static void to_psyz(const GameDrawEnv *g, DRAWENV *e) {
    e->clip.x = g->clip_x;
    e->clip.y = g->clip_y;
    e->clip.w = g->clip_w;
    e->clip.h = g->clip_h;
    e->ofs[0] = g->ofs[0];
    e->ofs[1] = g->ofs[1];
    e->tw.x = g->tw_x;
    e->tw.y = g->tw_y;
    e->tw.w = g->tw_w;
    e->tw.h = g->tw_h;
    e->tpage = g->tpage;
    e->dtd = g->dtd;
    e->dfe = g->dfe;
    e->isbg = g->isbg;
    e->r0 = g->r0;
    e->g0 = g->g0;
    e->b0 = g->b0;
}

static DRAWENV g_psyz_env;

GameDrawEnv *pe_psyq_PutDrawEnv(GameDrawEnv *env) {
    pe_host_gpu_log_note("DrawEnv", env->clip_x, env->clip_y, env->clip_w, env->clip_h);
    to_psyz(env, &g_psyz_env);
    PutDrawEnv(&g_psyz_env);
    return env;
}

void pe_psyq_DrawOTagEnv(uint32_t *ot, GameDrawEnv *env) {
    pe_psyq_PutDrawEnv(env);
    pe_host_ws_commit();
    walk(ot);
}
