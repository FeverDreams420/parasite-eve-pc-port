/* PSY-Q functions whose PsyZ implementation can take the game's arguments
 * unchanged on a 32-bit host: the structures involved (RECT, DISPENV,
 * MATRIX, VECTOR, SVECTOR, SpuCommonAttr, ...) have no primitive tag, so
 * their layout is the PS1 layout. Functions that take primitives, ordering
 * tables or DRAWENV (which embeds a tagged DR_ENV) are in gpu.c instead.
 */
#include <psyz.h>
#include <kernel.h>
#include <libapi.h>
#include <libetc.h>
#include <libgpu.h>
#include <libgte.h>
#include <libspu.h>
#include <libsnd.h>
#include <string.h>

void pe_host_spu_sync(void);

/* kernel: critical sections, events, root counters, callbacks */
void pe_psyq_EnterCriticalSection(void) { PS1_EnterCriticalSection(); }
void pe_psyq_ExitCriticalSection(void) { PS1_ExitCriticalSection(); }
void pe_psyq_FlushCache(void) {}
/* Handlers the game installs run through a wrapper that syncs the SPU
 * register shadow afterwards (see spu_regs.c). */
#define EVENT_WRAPS 16
static long (*g_event_func[EVENT_WRAPS])();
#define WRAP(n) static long event_wrap_##n(void) { long r = g_event_func[n](); pe_host_spu_sync(); return r; }
WRAP(0) WRAP(1) WRAP(2) WRAP(3) WRAP(4) WRAP(5) WRAP(6) WRAP(7)
WRAP(8) WRAP(9) WRAP(10) WRAP(11) WRAP(12) WRAP(13) WRAP(14) WRAP(15)
static long (*const g_event_wrap[EVENT_WRAPS])() = {
    event_wrap_0, event_wrap_1, event_wrap_2, event_wrap_3, event_wrap_4, event_wrap_5,
    event_wrap_6, event_wrap_7, event_wrap_8, event_wrap_9, event_wrap_10, event_wrap_11,
    event_wrap_12, event_wrap_13, event_wrap_14, event_wrap_15};

long pe_psyq_OpenEvent(unsigned long desc, long spec, long mode, long (*func)()) {
    int i;
    if (func) {
        for (i = 0; i < EVENT_WRAPS; i++) {
            if (!g_event_func[i] || g_event_func[i] == func) {
                g_event_func[i] = func;
                return OpenEvent(desc, spec, mode, g_event_wrap[i]);
            }
        }
    }
    return OpenEvent(desc, spec, mode, func);
}
long pe_psyq_CloseEvent(long e) { return CloseEvent(e); }
long pe_psyq_EnableEvent(long e) { return EnableEvent(e); }
long pe_psyq_DisableEvent(long e) { return DisableEvent(e); }
long pe_psyq_TestEvent(long e) { return TestEvent(e); }
void pe_psyq_UnDeliverEvent(unsigned a, unsigned b) { UnDeliverEvent(a, b); }
long pe_psyq_SetRCnt(long spec, unsigned short target, long mode) { return SetRCnt(spec, target, mode); }
long pe_psyq_GetRCnt(long spec) { return GetRCnt(spec); }
long pe_psyq_StartRCnt(long spec) { return StartRCnt(spec); }
long pe_psyq_StopRCnt(long spec) { return StopRCnt(spec); }
/* Debugging aids, all off unless set in the environment:
 *   PE_CAPTURE_DIR=dir PE_CAPTURE_EVERY=n  write frame_NNNNN.ppm every n frames
 *   PE_CAPTURE_VRAM=1                      write the whole VRAM as well
 *   PE_EXIT_AFTER=n                        quit after n frames */
#include <stdio.h>
#include <stdlib.h>
unsigned char *Psyz_VideoAllocCapturedFrame(int *w, int *h);
unsigned char *Psyz_VideoAllocVramDump(int *w, int *h);
static unsigned int g_frames;
unsigned int pe_host_frame_number(void) { return g_frames; }
void pe_host_gpu_log_note(const char *what, int a, int b, int c, int d);

static void write_ppm(const char *path, const unsigned char *rgb, int w, int h) {
    static int reported;
    FILE *f = fopen(path, "wb");
    if (!f) {
        if (!reported++) {
            fprintf(stderr, "pe: capture: cannot create %s\n", path);
        }
        return;
    }
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    fwrite(rgb, 3, (size_t)w * h, f);
    fclose(f);
}

static void debug_frame(void) {
    static int every = -1, exit_after = -1;
    static const char *dir;
    if (every < 0) {
        dir = getenv("PE_CAPTURE_DIR");
        every = getenv("PE_CAPTURE_EVERY") ? atoi(getenv("PE_CAPTURE_EVERY")) : 0;
        exit_after = getenv("PE_EXIT_AFTER") ? atoi(getenv("PE_EXIT_AFTER")) : 0;
    }
    g_frames++;
    if (dir && every > 0 && g_frames % (unsigned)every == 0) {
        char path[512];
        int w, h;
        unsigned char *rgb = Psyz_VideoAllocCapturedFrame(&w, &h);
        static int reported;
        if (!rgb && !reported++) {
            fprintf(stderr, "pe: capture: reading the screen back from the GPU failed (frame %u)\n", g_frames);
        }
        if (rgb) {
            snprintf(path, sizeof(path), "%s/frame_%05u.ppm", dir, g_frames);
            write_ppm(path, rgb, w, h);
            free(rgb);
        }
        if (getenv("PE_CAPTURE_VRAM") && (rgb = Psyz_VideoAllocVramDump(&w, &h))) {
            snprintf(path, sizeof(path), "%s/vram_%05u.ppm", dir, g_frames);
            write_ppm(path, rgb, w, h);
            free(rgb);
        }
    }
    if (exit_after > 0 && g_frames >= (unsigned)exit_after) {
        fprintf(stderr, "pe: PE_EXIT_AFTER reached (%u frames)\n", g_frames);
        fflush(stderr);
        exit(0);
    }
}

int pe_psyq_VSync(int mode) {
    int r;
    pe_host_spu_sync();
    r = VSync(mode);
    if (mode == 0 || mode > 1) {
        debug_frame();
    }
    return r;
}
static void (*g_vsync_func)(void);
static void vsync_wrap(void) {
    if (g_vsync_func) {
        g_vsync_func();
    }
    pe_host_spu_sync();
}
int pe_psyq_VSyncCallback(void (*f)()) {
    g_vsync_func = f;
    return VSyncCallback(f ? vsync_wrap : NULL);
}
int pe_psyq_ResetCallback(void) { return ResetCallback(); }

/* GPU calls without tagged structures */
int pe_psyq_ResetGraph(int mode) { return ResetGraph(mode); }
int pe_psyq_SetGraphDebug(int level) { return SetGraphDebug(level); }
void pe_psyq_SetDispMask(int mask) { SetDispMask(mask); }
int pe_psyq_ClearImage(RECT *r, u_char cr, u_char cg, u_char cb) {
    pe_host_gpu_log_note("ClearImage", r->x, r->y, r->w, r->h);
    return ClearImage(r, cr, cg, cb);
}
int pe_psyq_LoadImage(RECT *r, u_long *p) {
    pe_host_gpu_log_note("LoadImage", r->x, r->y, r->w, r->h);
    return LoadImage(r, p);
}
int pe_psyq_StoreImage(RECT *r, u_long *p) {
    pe_host_gpu_log_note("StoreImage", r->x, r->y, r->w, r->h);
    return StoreImage(r, p);
}
int pe_psyq_MoveImage(RECT *r, int x, int y) {
    pe_host_gpu_log_note("MoveImage", r->x, r->y, r->w, r->h);
    pe_host_gpu_log_note("MoveImage to", x, y, 0, 0);
    return MoveImage(r, x, y);
}
int pe_psyq_DrawSync(int mode) { return DrawSync(mode); }
DISPENV *pe_psyq_PutDispEnv(DISPENV *env) { return PutDispEnv(env); }

/* SPU library (PsyZ implements it on its SPU register model) */
void pe_psyq_SpuInit(void) { pe_host_spu_sync(); SpuInit(); pe_host_spu_sync(); }
void pe_psyq_SpuStart(void) { pe_host_spu_sync(); SpuStart(); pe_host_spu_sync(); }
void pe_psyq_SpuQuit(void) { SpuQuit(); }
long pe_psyq_SpuInitMalloc(long num, char *top) { return SpuInitMalloc(num, top); }
/* On hardware a DMA transfer ends with the DMA interrupt, whose handler
 * (_spu_FiDMA) calls the transfer callback. PsyZ copies at once and only
 * raises the completion event when no callback is set, so the callback is
 * called here after a DMA-mode transfer. */
static SpuTransferCallbackProc g_spu_transfer_cb;
static long g_spu_transfer_mode; /* 0 = DMA (SPU_TRANSFER_BY_DMA), 1 = I/O */

static void spu_transfer_done(void) {
    if (g_spu_transfer_mode == 0 && g_spu_transfer_cb) {
        g_spu_transfer_cb();
    }
}
u_long pe_psyq_SpuRead(u_char *addr, u_long size) {
    u_long r = SpuRead(addr, size);
    spu_transfer_done();
    return r;
}
u_long pe_psyq_SpuWrite(u_char *addr, u_long size) {
    u_long r = SpuWrite(addr, size);
    spu_transfer_done();
    return r;
}
void pe_psyq_SpuSetCommonAttr(SpuCommonAttr *a) { pe_host_spu_sync(); SpuSetCommonAttr(a); pe_host_spu_sync(); }
long pe_psyq_SpuSetIRQ(long on) { return SpuSetIRQ(on); }
SpuIRQCallbackProc pe_psyq_SpuSetIRQCallback(SpuIRQCallbackProc f) { return SpuSetIRQCallback(f); }
long pe_psyq_SpuSetNoiseClock(long n) { long r; pe_host_spu_sync(); r = SpuSetNoiseClock(n); pe_host_spu_sync(); return r; }
long pe_psyq_SpuSetReverb(long on) { long r; pe_host_spu_sync(); r = SpuSetReverb(on); pe_host_spu_sync(); return r; }
SpuTransferCallbackProc pe_psyq_SpuSetTransferCallback(SpuTransferCallbackProc f) {
    g_spu_transfer_cb = f;
    return SpuSetTransferCallback(f);
}
long pe_psyq_SpuSetTransferMode(long mode) {
    g_spu_transfer_mode = mode;
    return SpuSetTransferMode(mode);
}
unsigned long pe_psyq_SpuSetTransferStartAddr(unsigned long a) { return SpuSetTransferStartAddr(a); }
void pe_psyq_SsInit(void) { SsInit(); }


/* SpuSetReverbModeType (retail s_srmt.c sets the preset registers for the
 * type, zeroes the reverb depth, and for ECHO (7) / DELAY (8) sets delay and
 * feedback; bit 0x100 also clears the work area). Expressed through PsyZ's
 * SpuSetReverbModeParam with the same values. */
long pe_psyq_SpuSetReverbModeType(long mode) {
    SpuReverbAttr a;
    long type = mode & ~0x100;
    if (type < 0 || type >= 10) {
        return -1;
    }
    memset(&a, 0, sizeof(a));
    a.mask = SPU_REV_MODE | SPU_REV_DEPTHL | SPU_REV_DEPTHR | SPU_REV_DELAYTIME | SPU_REV_FEEDBACK;
    a.mode = (int)mode;
    a.depth.left = 0;
    a.depth.right = 0;
    a.delay = (type == 7 || type == 8) ? 0x7F : 0;
    a.feedback = type == 7 ? 0x7F : 0;
    pe_host_spu_sync();
    SpuSetReverbModeParam(&a);
    pe_host_spu_sync();
    return 0;
}

