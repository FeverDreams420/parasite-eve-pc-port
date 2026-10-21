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

void Psyz_GpuWriteGP0(unsigned int word);
int Psyz_GpuExeque(void);

#define PS1_TERM 0x00FFFFFFu
#define QUEUE_ROOM 0x3F00 /* PsyZ queue holds 0x4000 words */

static int g_queued;

static void push_packet(const uint32_t *p) {
    uint32_t len = p[0] >> 24;
    uint32_t i;
    if (len == 0) {
        return;
    }
    if (g_queued + (int)len > QUEUE_ROOM) {
        Psyz_GpuExeque();
        g_queued = 0;
    }
    for (i = 1; i <= len; i++) {
        Psyz_GpuWriteGP0(p[i]);
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

void pe_psyq_DrawOTag(uint32_t *ot) { walk(ot); }

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
    to_psyz(env, &g_psyz_env);
    PutDrawEnv(&g_psyz_env);
    return env;
}

void pe_psyq_DrawOTagEnv(uint32_t *ot, GameDrawEnv *env) {
    pe_psyq_PutDrawEnv(env);
    walk(ot);
}
