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
#include <stdint.h>
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
 *   PE_CAPTURE_FROM=n                      ...only from frame n on
 *   PE_CAPTURE_VRAM=1                      write the whole VRAM as well
 *   PE_EXIT_AFTER=n                        quit after n frames
 *   PE_VRAM_CHECK=1, PE_VRAM_CHECK_EVERY=n VRAM check (vram_check.c) */
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

/* F12 snapshot: requested -> record the next frame's packets -> at its
 * VSync save the picture and video memory. */
void pe_host_snapshot_gpu_begin(unsigned int frame);
void pe_host_snapshot_gpu_end(void);
unsigned int pe_host_snapshot_gpu_packets(void);
static int g_snap_state; /* 0 idle, 1 requested, 2 recording */
static int g_burst;      /* frames still to capture after a snapshot */
static FILE *g_pgxp_dump; /* polygon corners (Psyz_PgxpDebugFile) for the snapshot / burst */
static int g_frame_refreshes; /* refreshes the last game frame took */
float pe_host_ws_last_shift(void);
void pe_host_record_start(unsigned frame);
void pe_host_record_frame(unsigned frame, int refreshes, int mode, double game_s, double wait_s, int ff);
void pe_host_vram_check_snapshot(unsigned frame);
void pe_host_vram_check_frame(unsigned frame);
void pe_host_f12_start(unsigned frame);
const char *pe_host_f12_path(char *buf, size_t size, const char *fmt, ...);
void pe_host_f12_data(unsigned frame, int refreshes);
void pe_host_f12_ref_begin(unsigned snap_frame);
void pe_host_f12_ref_step(unsigned frame);
/* F12 (f12.c lists every file): all of it goes into the folder f12-NNNNN. */
void pe_host_snapshot_request(void) {
    char path[128];
    if (g_snap_state != 0) {
        return; /* the last one is still being taken */
    }
    g_snap_state = 1;
    pe_host_f12_start(g_frames);
    /* the screen as shown (window, soft borders), full size */
    Psyz_VideoPresentTestNext(
        pe_host_f12_path(path, sizeof(path), "snapshot-%05u-screen.ppm", g_frames + 1));
    pe_host_record_start(g_frames); /* and the next seconds (recorder.c) */
    fprintf(stderr, "pe: F12: saving into f12-%05u\n", g_frames);
}
static void snapshot_step(void) {
    if (g_snap_state == 2 && pe_host_snapshot_gpu_packets() != 0) {
        char path[128];
        int w, h;
        unsigned char *rgb;
        pe_host_snapshot_gpu_end();
        /* first: the reads below copy the HD picture down into the 1x VRAM */
        pe_host_vram_check_snapshot(g_frames);
        if ((rgb = Psyz_VideoAllocCapturedFrame(&w, &h)) != NULL) {
            pe_host_f12_path(path, sizeof(path), "snapshot-%05u-frame.ppm", g_frames);
            write_ppm(path, rgb, w, h);
            free(rgb);
        }
        if ((rgb = Psyz_VideoAllocVramDump(&w, &h)) != NULL) {
            pe_host_f12_path(path, sizeof(path), "snapshot-%05u-vram.ppm", g_frames);
            write_ppm(path, rgb, w, h);
            free(rgb);
        }
        pe_host_f12_data(g_frames, g_frame_refreshes);
        if (!getenv("PE_SNAPSHOT_RAM") || atoi(getenv("PE_SNAPSHOT_RAM")) != 0) {
            /* the 2 MiB of PS1 RAM (PE_SNAPSHOT_RAM=0: not) */
            FILE *rf;
            pe_host_f12_path(path, sizeof(path), "snapshot-%05u-ram.bin", g_frames);
            if ((rf = fopen(path, "wb")) != NULL) {
                fwrite((const void *)(uintptr_t)0x80000000u, 1, 0x200000, rf);
                fclose(rf);
            }
            {
                /* and the raw 16-bit video memory (1 MiB), for texture work */
                static unsigned short vram[1024 * 512];
                RECT all = {0, 0, 1024, 512};
                StoreImage(&all, (u_long *)vram);
                DrawSync(0);
                pe_host_f12_path(path, sizeof(path), "snapshot-%05u-vram.bin", g_frames);
                if ((rf = fopen(path, "wb")) != NULL) {
                    fwrite(vram, 1, sizeof(vram), rf);
                    fclose(rf);
                }
            }
        }
        fprintf(stderr, "pe: snapshot of frame %u saved\n", g_frames);
        g_snap_state = 0;
        Psyz_PgxpDebugFile(NULL);
        if (g_pgxp_dump) {
            fclose(g_pgxp_dump);
            g_pgxp_dump = NULL;
        }
        /* then the next PE_CAPTURE_BURST frames in a row (default 4, 0 =
         * none, up to 60), at the chosen resolution, for motion that shows
         * only while playing (vibration) */
        g_burst = getenv("PE_CAPTURE_BURST") ? atoi(getenv("PE_CAPTURE_BURST")) : 4;
        g_burst = g_burst > 60 ? 60 : (g_burst < 0 ? 0 : g_burst);
        if (g_burst > 0) {
            /* and every polygon's corners in those frames */
            g_pgxp_dump = fopen(pe_host_f12_path(path, sizeof(path), "burst-pgxp.txt"), "w");
            Psyz_PgxpDebugFile(g_pgxp_dump);
        }
    }
    if (g_snap_state == 1) {
        char path[128];
        pe_host_snapshot_gpu_begin(g_frames + 1);
        g_snap_state = 2;
        /* the same frame drawn at 1x as the PS1 draws it (f12.c) */
        pe_host_f12_ref_begin(g_frames + 1);
        /* every polygon's corners in the snapshot frame */
        pe_host_f12_path(path, sizeof(path), "snapshot-%05u-pgxp.txt", g_frames + 1);
        g_pgxp_dump = fopen(path, "w");
        Psyz_PgxpDebugFile(g_pgxp_dump);
    }
}

unsigned int pe_host_frame_number(void);
double pe_host_now_s(void);
static void debug_frame(void) {
    static int every = -1, exit_after = -1, from = 0;
    static const char *dir;
    if (every < 0) {
        dir = getenv("PE_CAPTURE_DIR");
        every = getenv("PE_CAPTURE_EVERY") ? atoi(getenv("PE_CAPTURE_EVERY")) : 0;
        exit_after = getenv("PE_EXIT_AFTER") ? atoi(getenv("PE_EXIT_AFTER")) : 0;
        from = getenv("PE_CAPTURE_FROM") ? atoi(getenv("PE_CAPTURE_FROM")) : 0;
    }
    g_frames++;
    snapshot_step();
    pe_host_f12_ref_step(g_frames);
    pe_host_vram_check_frame(g_frames);
    {
        /* debug PE_PRESENT_AT=n,m,...: the screen as presented (window, soft
         * borders) after game frame n is saved as present-<n>.ppm */
        static const char *at = (const char *)-1;
        if (at == (const char *)-1) {
            at = getenv("PE_PRESENT_AT");
        }
        if (at) {
            const char *p = at;
            while (*p) {
                if ((unsigned)atoi(p) == g_frames) {
                    char path[64];
                    snprintf(path, sizeof(path), "present-%05u.ppm", g_frames);
                    Psyz_VideoPresentTestNext(path);
                    break;
                }
                while (*p && *p != ',') {
                    p++;
                }
                if (*p == ',') {
                    p++;
                }
            }
        }
    }
    if (g_burst > 0) {
        /* Kept in memory and handled when the burst ends, so the frames being
         * recorded are not slowed down. The log gets, for each frame, how
         * many refreshes it took, the widescreen slide, and how many pixels
         * changed since the frame before in each cell of an 8 x 6 grid over
         * the picture (what moves, and where). PE_BURST_IMAGES=1 also writes
         * the pictures. */
        static unsigned char *keep[60];
        static int kw, kh, kn, kref[60];
        static unsigned kframe[60];
        static float kshift[60];
        int w, h, i;
        unsigned char *rgb = Psyz_VideoAllocCapturedFrameScaled(&w, &h);
        if (g_pgxp_dump) {
            fprintf(g_pgxp_dump, "frame %u (above)\n", g_frames);
        }
        if (rgb && kn < 60) {
            keep[kn] = rgb;
            kw = w, kh = h;
            kframe[kn] = g_frames;
            kref[kn] = g_frame_refreshes;
            kshift[kn] = pe_host_ws_last_shift();
            kn++;
        } else {
            free(rgb);
        }
        if (--g_burst == 0) {
            for (i = 0; i < kn; i++) {
                char line[512];
                int len = snprintf(line, sizeof(line), "pe: burst frame %u: %d refreshes, slide %.1f",
                                   kframe[i], kref[i], kshift[i]);
                if (i > 0) {
                    unsigned cells[6][8] = {{0}}, total = 0;
                    int x, y, cx, cy;
                    for (y = 0; y < kh; y++) {
                        for (x = 0; x < kw; x++) {
                            const unsigned char *a = keep[i] + ((size_t)y * kw + x) * 3;
                            const unsigned char *b = keep[i - 1] + ((size_t)y * kw + x) * 3;
                            int d = abs(a[0] - b[0]) + abs(a[1] - b[1]) + abs(a[2] - b[2]);
                            if (d > 48) {
                                cells[y * 6 / kh][x * 8 / kw]++;
                                total++;
                            }
                        }
                    }
                    len += snprintf(line + len, sizeof(line) - len, ", %u pixels changed:", total);
                    for (cy = 0; cy < 6; cy++) {
                        len += snprintf(line + len, sizeof(line) - len, " |");
                        for (cx = 0; cx < 8; cx++) {
                            len += snprintf(line + len, sizeof(line) - len, " %u", cells[cy][cx]);
                        }
                    }
                }
                fprintf(stderr, "%s\n", line);
            }
            for (i = 0; i < kn; i++) {
                if (!getenv("PE_BURST_IMAGES") || atoi(getenv("PE_BURST_IMAGES")) != 0) {
                    char path[128];
                    pe_host_f12_path(path, sizeof(path), "burst-%05u.ppm", kframe[i]);
                    write_ppm(path, keep[i], kw, kh);
                }
                free(keep[i]);
            }
            fprintf(stderr, "pe: burst of %d frames at %dx%d done\n", kn, kw, kh);
            Psyz_PgxpDebugFile(NULL);
            if (g_pgxp_dump) {
                fclose(g_pgxp_dump);
                g_pgxp_dump = NULL;
            }
            kn = 0;
        }
    }
    if (dir && every > 0 && g_frames >= (unsigned)from && g_frames % (unsigned)every == 0) {
        char path[512];
        int w, h;
        unsigned char *rgb = Psyz_VideoAllocCapturedFrameScaled(&w, &h); /* at the chosen resolution */
        static int reported;
        if (!rgb && !reported++) {
            fprintf(stderr, "pe: capture: reading the screen back from the GPU failed (frame %u)\n", g_frames);
        }
        if (rgb) {
            snprintf(path, sizeof(path), "%s/frame_%05u.ppm", dir, g_frames);
            write_ppm(path, rgb, w, h);
            free(rgb);
            if (getenv("PE_CAPTURE_LOG")) { /* debug: the game frame (PE_PAD_SCRIPT numbering) */
                fprintf(stderr, "pe: capture %u = game frame %u\n", g_frames, pe_host_frame_number());
            }
        }
        if (getenv("PE_CAPTURE_VRAM") && (rgb = Psyz_VideoAllocVramDump(&w, &h))) {
            snprintf(path, sizeof(path), "%s/vram_%05u.ppm", dir, g_frames);
            write_ppm(path, rgb, w, h);
            free(rgb);
        }
    }
    {
        /* PE_TEST_CRASH=n: jump to a bad address at frame n (tests the
         * crash report and crash-dump.bin). */
        static int crash_at = -2;
        if (crash_at == -2) {
            crash_at = getenv("PE_TEST_CRASH") ? atoi(getenv("PE_TEST_CRASH")) : -1;
        }
        if (crash_at > 0 && g_frames == (unsigned)crash_at) {
            ((void (*)(void))(uintptr_t)0x80210001u)();
        }
    }
    if (exit_after > 0 && g_frames >= (unsigned)exit_after) {
        fprintf(stderr, "pe: PE_EXIT_AFTER reached (%u frames)\n", g_frames);
        fflush(stderr);
        exit(0);
    }
}

/* VSync as the retail libetc does it (src/main/psyq/libetc/vsync.c):
 * VSync(n), n >= 2, returns once n refreshes have passed since the previous
 * VSync returned (at least the next refresh), so VSync(2) holds the game at
 * 30 frames per second and VSync(4) - the movie player - at 15. PsyZ waits
 * for one refresh only and runs the VBlank interrupt n times, which made
 * everything paced by VSync(2), and everything driven by the interrupt
 * (sound driver, timers), run at double speed.
 * Here PsyZ's VSync(0) - show the frame, wait one refresh (driver vsync or
 * PsyZ's frame timer), one VBlank interrupt - is repeated until n refreshes
 * have passed by the clock (PsyZ's VSync(-1)). Refreshes the game itself used
 * up beyond those get their VBlank interrupt too, as on hardware (at most 8
 * at a time, so a long synchronous CD read does not cause a burst). */
void Psyz_KernelVBlank(void);
void pe_host_hotkeys_frame(void);
int pe_host_fast_forward(void);
#ifdef _WIN32
/* Only Sleep is needed; <windows.h> clashes with the PS1 headers. */
__declspec(dllimport) void __stdcall Sleep(unsigned long milliseconds);
static void sleep_1ms(void) { Sleep(1); }
#else
#include <time.h>
static void sleep_1ms(void) {
    struct timespec ts = {0, 1000000};
    nanosleep(&ts, NULL);
}
#endif
void SDL_DelayPrecise(unsigned long long ns); /* SDL3 */
static int g_last_vblank = -1;

static unsigned g_vsync_cb_calls;
unsigned pe_host_vsync_cb_calls(void);
void pe_host_vsync_cb_reset(void);
double pe_host_now_s(void);
double pe_host_draw_s_take(void);
unsigned Psyz_KernelTimerTicks(int i);
unsigned Psyz_KernelVBlankCount(void);
static double g_vsync_leave, g_game_s, g_wait_s; /* pacing log */
int pe_psyq_VSync(int mode) {
    int r, now, steps = 0, extra;
    double t_enter;
    if (mode < 0 || mode == 1) {
        return VSync(mode);
    }
    t_enter = pe_host_now_s();
    if (g_vsync_leave > 0) {
        g_game_s += t_enter - g_vsync_leave;
    }
    if (g_last_vblank < 0) {
        g_last_vblank = VSync(-1);
    }
    pe_host_hotkeys_frame();
    {
        void pe_host_movie_check(void);
        pe_host_movie_check(); /* Original while a movie plays (settings.c) */
    }
    pe_host_spu_sync();
    /* exact positions (precise geometry) age by game frames */
    Psyz_PgxpNewFrame();
    {
        /* debug PE_LOG_RCNT: the sound driver's timer (root counter 2) ticks
         * and the blanks per VSync call, and calls per second */
        static int log_rcnt = -1;
        if (log_rcnt < 0) {
            log_rcnt = getenv("PE_LOG_RCNT") != NULL;
        }
        if (log_rcnt) {
            static unsigned frames, last, lastvb, modes[4];
            static double t0;
            modes[mode < 3 ? mode : 3]++;
            if (++frames % 120 == 0) {
                double t = pe_host_now_s();
                fprintf(stderr,
                        "pe: timer 2: %.1f ticks and %.2f blanks per VSync call (modes 0:%u 2:%u >2:%u), "
                        "%.0f calls/s%s\n",
                        (Psyz_KernelTimerTicks(2) - last) / 120.0, (Psyz_KernelVBlankCount() - lastvb) / 120.0,
                        modes[0], modes[2], modes[3], t0 > 0 ? 120.0 / (t - t0) : 0.0,
                        pe_host_fast_forward() ? " (fast forward)" : "");
                last = Psyz_KernelTimerTicks(2);
                lastvb = Psyz_KernelVBlankCount();
                t0 = t;
                modes[0] = modes[2] = modes[3] = 0;
            }
        }
    }
    do {
        double t0 = pe_host_now_s();
        const int clock_paced = Psyz_VideoClockPacing();
        /* Fullscreen (clock pacing, PsyZ 0030): the frame is shown on the
         * first refresh step only; the other steps just wait their refresh. */
        if (clock_paced && steps > 0) {
            Psyz_VideoSkipNextPresent();
        }
        r = VSync(0);
        steps++;
        if (!pe_host_fast_forward() && !clock_paced) {
            /* Floor: a refresh step never ends sooner than a screen refresh
             * after the last one. In exclusive fullscreen on the test PC
             * (D3D12) VSync(0) returned early often enough to run the game
             * 20-40% fast (frames waited 23-28 ms instead of 33), but not so
             * often that the switch below noticed. A schedule advancing
             * 1/60.3 s per step: with working driver VSync the real refreshes
             * (1/60 s) stay behind it and nothing waits; when steps come
             * early they wait for it. Late by over 50 ms: start again. */
            static double due;
            const double step_s = 1.0 / 60.3;
            double t = pe_host_now_s();
            if (due <= 0.0 || t - due > 0.05) {
                due = t;
            } else {
                due += step_s;
                if (t < due) { /* SDL's precise wait: high-resolution timer, then a short spin */
                    SDL_DelayPrecise((unsigned long long)((due - t) * 1e9));
                }
            }
        }
        now = VSync(-1);
        if (!pe_host_fast_forward() && !clock_paced) {
            /* Safety net: VSync(0) should wait for a screen refresh (driver
             * VSync). Where it returns at once again and again (seen with
             * exclusive fullscreen on one PC: the game ran in fast forward),
             * PsyZ's own frame limiter takes over. */
            static unsigned checked, quick;
            static int switched = -1;
            double took = pe_host_now_s() - t0;
            if (switched < 0) { /* not without a display (offscreen tests) */
                const char *drv = getenv("SDL_VIDEO_DRIVER");
                switched = drv && !strcmp(drv, "offscreen");
            }
            if (!switched && Psyz_VideoGetVsyncMode() != PSYZ_VSYNC_OFF) {
                checked++;
                quick += took < 0.004;
                if (checked == 60) {
                    if (quick > 50) {
                        Psyz_VideoSetVsyncMode(PSYZ_VSYNC_OFF);
                        switched = 1;
                        fprintf(stderr, "pe: the screen refreshes did not make the game wait (%u of 60 "
                                        "returned at once); using the frame limiter\n",
                                quick);
                    }
                    checked = quick = 0;
                }
            }
        }
    } while (!pe_host_fast_forward() && mode >= 2 && now - g_last_vblank < mode && steps < mode);
    if (pe_host_fast_forward()) {
        /* Fast forward (Tab, hotkeys.c): one step per frame, no waiting; the
         * VBlank interrupt runs as often as the game would have waited. */
        for (extra = (mode >= 2 ? mode : 1) - steps; extra > 0; extra--) {
            Psyz_KernelVBlank();
        }
        pe_host_record_frame(g_frames + 1, now - g_last_vblank, mode,
                             g_vsync_leave > 0 ? t_enter - g_vsync_leave : 0.0, pe_host_now_s() - t_enter, 1);
        g_last_vblank = now;
        debug_frame();
        return r;
    }
    /* Safety net where PsyZ's step does not really wait (no display, e.g.
     * the offscreen test driver): never return more than one refresh early. */
    while (mode >= 2 && now - g_last_vblank < mode - 1) {
        sleep_1ms();
        now = VSync(-1);
    }
    g_frame_refreshes = now - g_last_vblank; /* for the burst log */
    extra = now - g_last_vblank - steps;
    if (extra > 8) {
        extra = 8;
    }
    for (; extra > 0; extra--) {
        Psyz_KernelVBlank();
    }
    if (getenv("PE_LOG_VSYNC")) {
        /* every 300 calls: how often each mode was used and refreshes per call */
        static unsigned calls, by_mode[4], vblanks;
        static int start = -1;
        if (start < 0) {
            start = g_last_vblank;
        }
        calls++;
        by_mode[mode < 3 ? mode : 3]++;
        if (calls % 300 == 0) {
            vblanks = (unsigned)(now - start);
            {
                static double t0;
                double t = pe_host_now_s();
                fprintf(stderr, "pe: vsync: %u calls (mode 0: %u, 2: %u, >2: %u), %.2f refreshes per call, "
                                "%.1f VSync callbacks/s\n",
                        calls, by_mode[0], by_mode[2], by_mode[3], (double)vblanks / 300.0,
                        t0 > 0 ? (double)pe_host_vsync_cb_calls() / (t - t0) : 0.0);
                pe_host_vsync_cb_reset();
                t0 = t;
            }
            start = now;
            memset(by_mode, 0, sizeof(by_mode));
        }
    }
    if (mode >= 2) {
        /* every 300 game frames: how many took longer than the refreshes the
         * game paces itself at (late frames make its motion uneven) */
        static unsigned frames, late, worst;
        unsigned took = (unsigned)(now - g_last_vblank);
        frames++;
        if (took > (unsigned)mode) {
            late++;
            worst = took > worst ? took : worst;
        }
        if (frames == 300) {
            unsigned dl;
            double dl_ms, draw_s = pe_host_draw_s_take();
            Psyz_VideoDownloadStats(&dl, &dl_ms);
            double pm[8], mdec_ms;
            unsigned pn[8], mdec_n;
            void pe_mdec_take(double *ms, unsigned *n);
            Psyz_VideoProfileTake(pm, pn);
            pe_mdec_take(&mdec_ms, &mdec_n);
            if (late || getenv("PE_LOG_PACING")) {
                /* per frame, on average: the game's own work (includes
                 * drawing), drawing, waiting in VSync (the refresh, and the
                 * graphics card finishing), and the VRAM read-backs */
                fprintf(stderr,
                        "pe: pacing: %u of the last 300 frames were late (longest %u refreshes); "
                        "per frame: game %.1f ms (drawing %.1f), waiting %.1f ms; %u VRAM read-backs, "
                        "%.1f ms\n",
                        late, worst, g_game_s * 1000.0 / 300, draw_s * 1000.0 / 300,
                        g_wait_s * 1000.0 / 300, dl, dl_ms);
                /* where the time went (all 300 frames together) */
                fprintf(stderr,
                        "pe: timing: movie decode %u x %.0f ms, uploads %u x %.0f ms (copies to HD %u x %.0f ms), "
                        "24-bit picture %u x %.0f ms, presents %u x %.0f ms, MoveImage %u x %.0f ms, "
                        "ClearImage %u x %.0f ms, HD copies down %u x %.0f ms, drawing %u x %.0f ms\n",
                        mdec_n, mdec_ms, pn[0], pm[0], pn[1], pm[1], pn[2], pm[2], pn[3], pm[3], pn[4], pm[4],
                        pn[5], pm[5], pn[6], pm[6], pn[7], pm[7]);
            }
            frames = late = worst = 0;
            g_game_s = g_wait_s = 0.0;
        }
    }
    pe_host_record_frame(g_frames + 1, now - g_last_vblank, mode,
                         g_vsync_leave > 0 ? t_enter - g_vsync_leave : 0.0, pe_host_now_s() - t_enter, 0);
    g_last_vblank = now;
    debug_frame();
    g_vsync_leave = pe_host_now_s();
    g_wait_s += g_vsync_leave - t_enter;
    return r;
}
static void (*g_vsync_func)(void);
unsigned pe_host_vsync_cb_calls(void) { return g_vsync_cb_calls; }
void pe_host_vsync_cb_reset(void) { g_vsync_cb_calls = 0; }
static void vsync_wrap(void) {
    g_vsync_cb_calls++;
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
/* The retail ResetGraph (src/main/psyq/libgpu/sys.c) also sets up libgpu's
 * own state at 0x8009574C (GpuSystemState, 0x80 bytes): cleared, queue = 1,
 * VRAM width/height from the tables at 0x800957CC/0x800957D8 indexed by
 * the GPU type, and the DRAWENV/DISPENV caches filled with 0xFF. The game's
 * C reads that state - SetDrawArea clamps every coordinate to width-1 /
 * height-1, so with it left at 0 every clip area became (1023,511) and the
 * name-entry text was clipped away. The type is detected from the GPU on
 * hardware; the tables give 1024x512 for both types 0 and 1 (a retail
 * console's VRAM size), so type 0 is used. */
int pe_psyq_ResetGraph(int mode) {
    int r = ResetGraph(mode);
    switch (mode & 7) {
    case 0:
    case 3:
    case 5: {
        unsigned char *state = (unsigned char *)0x8009574Cu;
        const unsigned short *widths = (const unsigned short *)0x800957CCu;
        const unsigned short *heights = (const unsigned short *)0x800957D8u;
        const int type = 0;
        memset(state, 0, 0x80);
        state[0] = (unsigned char)type;
        state[1] = 1;                                     /* queue */
        *(short *)(state + 4) = (short)widths[type * 2];  /* width */
        *(short *)(state + 6) = (short)heights[type * 2]; /* height */
        memset(state + 0x10, 0xFF, 0x6C - 0x10);          /* DRAWENV cache */
        memset(state + 0x6C, 0xFF, 0x80 - 0x6C);          /* DISPENV cache */
        break;
    }
    default:
        break;
    }
    return r;
}
int pe_psyq_SetGraphDebug(int level) { return SetGraphDebug(level); }
void pe_psyq_SetDispMask(int mask) { SetDispMask(mask); }
void pe_host_font_vram_written(int x, int y, int w, int h);
int pe_psyq_ClearImage(RECT *r, u_char cr, u_char cg, u_char cb) {
    int ret;
    pe_host_gpu_log_note("ClearImage", r->x, r->y, r->w, r->h);
    ret = ClearImage(r, cr, cg, cb);
    pe_host_font_vram_written(r->x, r->y, r->w, r->h);
    return ret;
}
/* sharp text (font_hires.c) follows what the game writes to its font */
int pe_host_font_before_write(int x, int y, int w, int h);
void pe_host_font_after_load(int was_on, int x, int y, int w, int h, const unsigned short *data);
int pe_psyq_LoadImage(RECT *r, u_long *p) {
    int ret, was_on = pe_host_font_before_write(r->x, r->y, r->w, r->h);
    pe_host_gpu_log_note("LoadImage", r->x, r->y, r->w, r->h);
    ret = LoadImage(r, p);
    pe_host_font_after_load(was_on, r->x, r->y, r->w, r->h, (const unsigned short *)p);
    return ret;
}
int pe_psyq_StoreImage(RECT *r, u_long *p) {
    pe_host_gpu_log_note("StoreImage", r->x, r->y, r->w, r->h);
    return StoreImage(r, p);
}
int pe_psyq_MoveImage(RECT *r, int x, int y) {
    int ret;
    pe_host_gpu_log_note("MoveImage", r->x, r->y, r->w, r->h);
    pe_host_gpu_log_note("MoveImage to", x, y, 0, 0);
    ret = MoveImage(r, x, y);
    pe_host_font_vram_written(x, y, r->w, r->h);
    return ret;
}
int pe_psyq_DrawSync(int mode) { return DrawSync(mode); }
/* DISPENV.screen places the picture on the TV's 240 lines (Parasite Eve:
 * 224 lines starting 8 down); PsyZ is told so it shows the rest black
 * instead of the VRAM rows below the display area (the other frame
 * buffer, which showed as a strip at the bottom). */
void Psyz_VideoSetScreenLines(int top, int lines);
DISPENV *pe_psyq_PutDispEnv(DISPENV *env) {
    Psyz_VideoSetScreenLines(env->screen.y, env->screen.h);
    return PutDispEnv(env);
}

/* SPU library (PsyZ implements it on its SPU register model) */
/* PsyZ opens the audio device (Psyz_AudioInit) only from its own libcd,
 * when CD audio is switched on; this port replaces libcd/libds with its own
 * disc reader, so without this the device was never opened and nothing was
 * heard. It is opened before the SPU is initialised (Psyz_AudioInit also
 * resets PsyZ's SPU). */
int Psyz_AudioInit(void);
void pe_psyq_SpuInit(void) {
    if (Psyz_AudioInit() != 0) {
        fprintf(stderr, "pe: could not open the audio device; no sound\n");
    }
    pe_host_spu_sync();
    SpuInit();
    pe_host_spu_sync();
}
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
void pe_psyq_SpuSetCommonAttr(SpuCommonAttr *a) {
    pe_host_spu_sync();
    SpuSetCommonAttr(a);
    pe_host_spu_sync();
    if (getenv("PE_LOG_SPU")) {
        volatile unsigned short *r = (volatile unsigned short *)(uintptr_t)0x1F801C00u;
        fprintf(stderr, "pe: SpuSetCommonAttr mask %04lX cd vol %04X/%04X mix %ld -> SPUCNT %04X CD vol %04X/%04X\n",
                (unsigned long)a->mask, (unsigned)(unsigned short)a->cd.volume.left,
                (unsigned)(unsigned short)a->cd.volume.right, (long)a->cd.mix, r[0x1AA / 2], r[0x1B0 / 2],
                r[0x1B2 / 2]);
    }
}
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
/* SsInit: in this game the function at the PSY-Q SsInit address
 * (src/main/psyq/libsnd/ssinit_c.c) only starts the three game-time counters
 * (game clock, play time); it does not reset the SPU. PsyZ's SsInit runs
 * SpuInit, which cleared the CD audio enable and CD volume that
 * Spu_InitHardware had just set (so movie audio was silent) and left the
 * play-time clock stopped. Do what the retail code does. */
void GameTime_InitCounter0(void);
void GameTime_InitCounter1(void);
void GameTime_InitCounter2(void);
void pe_psyq_SsInit(void) {
    GameTime_InitCounter0();
    GameTime_InitCounter2();
    GameTime_InitCounter1();
}


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

