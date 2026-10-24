/* F12 capture: everything for one moment goes into its own folder,
 * f12-NNNNN (NNNNN = the game frame F12 was pressed on). Besides the
 * snapshot (forward.c), the GPU packets (gpu.c), the recording (recorder.c)
 * and the VRAM check (vram_check.c), this file writes:
 *   snapshot-NNNNN-info.txt     frame, time, resolution, widescreen slide and
 *                               picture ends, settings.ini, loaded overlays,
 *                               last CD reads
 *   snapshot-NNNNN-scratch.bin  the 1 KiB scratchpad
 *   snapshot-NNNNN-spu.bin      512 KiB of sound RAM, then the 512 bytes of
 *                               SPU registers (0x1F801C00..)
 *   snapshot-NNNNN-ref1x.ppm    the same frame drawn at 1x the way the PS1
 *                               draws it (PsyZ reference picture)
 *   snapshot-NNNNN-hd.ppm       the frame as drawn at the chosen resolution
 *   snapshot-NNNNN-hd-vs-1x.ppm heat map: the 1x picture in grey, yellow to
 *                               red where the HD frame, scaled down, differs
 *   snapshot-NNNNN-hd-vs-1x.txt how much differs, and the areas where most of
 *                               the pixels differ (8x8 blocks joined)
 * HD always differs a little: polygon edges, exact positions (up to 2
 * pixels), textures with perspective on 3D models, sharp text, smooth
 * shadows. Those are thin lines or known places; a solid area is worth a
 * look. Game data in these files stays on your computer. */
#include <psyz.h>
#include <psyz/video.h>
#include <psyz/spu.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#define make_dir(p) _mkdir(p)
#else
#include <sys/stat.h>
#define make_dir(p) mkdir(p, 0777)
#endif

double pe_host_now_s(void);
float pe_host_ws_last_shift(void);
void pe_host_ws_last_picture(float *left, float *right);
void pe_host_overlays_report_to(FILE *f);
void pe_host_cd_report_to(FILE *f);

static char g_dir[32]; /* "f12-NNNNN", or empty (current folder) */

/* A new F12: its folder. */
void pe_host_f12_start(unsigned frame) {
    snprintf(g_dir, sizeof(g_dir), "f12-%05u", frame);
    make_dir(g_dir); /* fails harmlessly if it is there */
    {
        /* check it can be written; else use the current folder */
        char probe[64];
        FILE *f;
        snprintf(probe, sizeof(probe), "%s/.probe", g_dir);
        if ((f = fopen(probe, "w")) == NULL) {
            fprintf(stderr, "pe: F12: cannot create folder %s, writing here\n", g_dir);
            g_dir[0] = 0;
        } else {
            fclose(f);
            remove(probe);
        }
    }
}

/* Path of an F12 file: the name (printf-style) inside the current F12 folder. */
const char *pe_host_f12_path(char *buf, size_t size, const char *fmt, ...) {
    char name[96];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(name, sizeof(name), fmt, ap);
    va_end(ap);
    if (g_dir[0]) {
        snprintf(buf, size, "%s/%s", g_dir, name);
    } else {
        snprintf(buf, size, "%s", name);
    }
    return buf;
}

static void write_ppm(const char *path, const unsigned char *rgb, int w, int h) {
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "pe: F12: cannot create %s\n", path);
        return;
    }
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    fwrite(rgb, 3, (size_t)w * h, f);
    fclose(f);
}

static void write_bin(const char *path, const void *a, size_t na, const void *b, size_t nb) {
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "pe: F12: cannot create %s\n", path);
        return;
    }
    fwrite(a, 1, na, f);
    if (b) {
        fwrite(b, 1, nb, f);
    }
    fclose(f);
}

/* Game state that is not a picture (called with the snapshot). */
void pe_host_f12_data(unsigned frame, int refreshes) {
    char path[128];
    FILE *f;
    write_bin(pe_host_f12_path(path, sizeof(path), "snapshot-%05u-scratch.bin", frame),
              (const void *)(uintptr_t)0x1F800000u, 1024, NULL, 0);
    {
        static unsigned char spu_ram[512 * 1024];
        unsigned short regs[256];
        int i;
        Psyz_SpuMemRead(0, spu_ram, sizeof(spu_ram));
        for (i = 0; i < 256; i++) {
            regs[i] = Psyz_SpuRead((unsigned)i * 2);
        }
        write_bin(pe_host_f12_path(path, sizeof(path), "snapshot-%05u-spu.bin", frame), spu_ram,
                  sizeof(spu_ram), regs, sizeof(regs));
    }
    if ((f = fopen(pe_host_f12_path(path, sizeof(path), "snapshot-%05u-info.txt", frame), "w")) !=
        NULL) {
        float left = 0.0f, right = 0.0f;
        FILE *ini;
        pe_host_ws_last_picture(&left, &right);
        fprintf(f, "game frame %u, %.2f s since start, last frame took %d refreshes\n", frame,
                pe_host_now_s(), refreshes);
        fprintf(f, "resolution %ux, widescreen %s, slide %.1f, picture ends %.1f .. %.1f\n",
                Psyz_VideoGetInternalResolution(), Psyz_VideoGetWidescreen() ? "on" : "off",
                pe_host_ws_last_shift(), left, right);
        fprintf(f, "\nsettings.ini:\n");
        if ((ini = fopen("settings.ini", "r")) != NULL) {
            char line[256];
            while (fgets(line, sizeof(line), ini)) {
                if (line[0] != '#') {
                    fprintf(f, "  %s", line);
                }
            }
            fclose(ini);
        }
        fprintf(f, "\noverlays:\n");
        pe_host_overlays_report_to(f);
        fprintf(f, "\nlast CD reads:\n");
        pe_host_cd_report_to(f);
        fclose(f);
    }
}

/* HD vs 1x ------------------------------------------------------------- */

#define CELL 8
#define STRONG 40 /* a channel off by more than this counts as differing */

typedef struct {
    int x0, y0, x1, y1, cells;
    unsigned strong;
} DiffArea;

static int cmp_area(const void *a, const void *b) {
    const DiffArea *p = a, *q = b;
    return (int)q->strong - (int)p->strong;
}

static void compare(unsigned frame, const unsigned char *ref, int w, int h, const unsigned char *hd,
                    int sw, int sh, float coverage) {
    const int n = sw / w;
    char path[128];
    unsigned char *diff = malloc((size_t)w * h);
    unsigned char *heat = malloc((size_t)w * h * 3);
    const int cw = (w + CELL - 1) / CELL, ch = (h + CELL - 1) / CELL;
    unsigned short *cell = calloc((size_t)cw * ch, sizeof(unsigned short));
    unsigned char *seen = calloc((size_t)cw * ch, 1);
    int *stack = malloc(sizeof(int) * (size_t)cw * ch);
    DiffArea *areas = malloc(sizeof(DiffArea) * (size_t)cw * ch);
    unsigned mild = 0, strong = 0;
    int n_areas = 0, x, y;
    FILE *f;
    if (!diff || !heat || !cell || !seen || !stack || !areas || n < 1 || sw != w * n ||
        sh != h * n) {
        fprintf(stderr, "pe: F12: HD vs 1x: sizes do not match (%dx%d vs %dx%d)\n", w, h, sw, sh);
        goto out;
    }
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            unsigned sum[3] = {0, 0, 0};
            int i, j, c, d = 0;
            const unsigned char *r = ref + ((size_t)y * w + x) * 3;
            unsigned char *o = heat + ((size_t)y * w + x) * 3;
            for (j = 0; j < n; j++) {
                const unsigned char *p = hd + (((size_t)y * n + j) * sw + (size_t)x * n) * 3;
                for (i = 0; i < n; i++, p += 3) {
                    sum[0] += p[0], sum[1] += p[1], sum[2] += p[2];
                }
            }
            for (c = 0; c < 3; c++) {
                int a = (int)((sum[c] + (unsigned)(n * n) / 2) / (unsigned)(n * n));
                int e = abs(a - r[c]);
                d = e > d ? e : d;
            }
            diff[y * w + x] = (unsigned char)d;
            {
                const int grey = (r[0] * 77 + r[1] * 150 + r[2] * 29) >> 8;
                o[0] = o[1] = o[2] = (unsigned char)(grey * 2 / 5);
            }
            if (d > STRONG) {
                strong++;
                cell[(y / CELL) * cw + x / CELL]++;
                o[0] = 255, o[1] = (unsigned char)(d > 120 ? 0 : 120 - d), o[2] = 0;
            } else if (d > 12) {
                mild++;
                o[0] = 200, o[1] = 200, o[2] = 0;
            }
        }
    }
    /* areas: cells where at least 3/8 of the pixels differ strongly, joined
     * with their 8 neighbours */
    for (y = 0; y < ch; y++) {
        for (x = 0; x < cw; x++) {
            int sp = 0;
            DiffArea a;
            if (seen[y * cw + x] || cell[y * cw + x] < CELL * CELL * 3 / 8) {
                continue;
            }
            a.x0 = w, a.y0 = h, a.x1 = -1, a.y1 = -1, a.cells = 0, a.strong = 0;
            seen[y * cw + x] = 1;
            stack[sp++] = y * cw + x;
            while (sp) {
                const int t = stack[--sp], cy = t / cw, cx = t % cw;
                int dx, dy;
                a.cells++;
                a.strong += cell[t];
                a.x0 = cx * CELL < a.x0 ? cx * CELL : a.x0;
                a.y0 = cy * CELL < a.y0 ? cy * CELL : a.y0;
                a.x1 = cx * CELL + CELL - 1 > a.x1 ? cx * CELL + CELL - 1 : a.x1;
                a.y1 = cy * CELL + CELL - 1 > a.y1 ? cy * CELL + CELL - 1 : a.y1;
                for (dy = -1; dy <= 1; dy++) {
                    for (dx = -1; dx <= 1; dx++) {
                        const int ny = cy + dy, nx = cx + dx;
                        if (ny >= 0 && ny < ch && nx >= 0 && nx < cw && !seen[ny * cw + nx] &&
                            cell[ny * cw + nx] >= CELL * CELL * 3 / 8) {
                            seen[ny * cw + nx] = 1;
                            stack[sp++] = ny * cw + nx;
                        }
                    }
                }
            }
            a.x1 = a.x1 >= w ? w - 1 : a.x1;
            a.y1 = a.y1 >= h ? h - 1 : a.y1;
            areas[n_areas++] = a;
        }
    }
    qsort(areas, (size_t)n_areas, sizeof(DiffArea), cmp_area);
    write_ppm(pe_host_f12_path(path, sizeof(path), "snapshot-%05u-ref1x.ppm", frame), ref, w, h);
    write_ppm(pe_host_f12_path(path, sizeof(path), "snapshot-%05u-hd.ppm", frame), hd, sw, sh);
    write_ppm(pe_host_f12_path(path, sizeof(path), "snapshot-%05u-hd-vs-1x.ppm", frame), heat, w, h);
    if ((f = fopen(pe_host_f12_path(path, sizeof(path), "snapshot-%05u-hd-vs-1x.txt", frame), "w")) !=
        NULL) {
        int i;
        fprintf(f, "HD (%dx, %dx%d, averaged down) vs the 1x reference (%dx%d), game frame %u\n", n,
                sw, sh, w, h, frame);
        if (coverage < 0.9f) {
            fprintf(f, "NOTE: the reference drew only %.0f%% of the picture since it started; "
                       "the rest is the HD picture copied down, so it cannot differ\n",
                    coverage * 100.0f);
        }
        fprintf(f, "pixels: %u differ strongly (a channel by more than %d), %u slightly, of %d\n",
                strong, STRONG, mild, w * h);
        fprintf(f, "areas (8x8 blocks with 3/8 or more differing strongly, joined), largest first: %d\n",
                n_areas);
        for (i = 0; i < n_areas && i < 40; i++) {
            fprintf(f, "  x %3d..%3d  y %3d..%3d  (%dx%d), %u pixels differ\n", areas[i].x0,
                    areas[i].x1, areas[i].y0, areas[i].y1, areas[i].x1 - areas[i].x0 + 1,
                    areas[i].y1 - areas[i].y0 + 1, areas[i].strong);
        }
        fprintf(f, "\nCoordinates are 1x picture pixels (the -ref1x picture); HD pixel = x * %d.\n"
                   "Expected differences: polygon edges (thin lines), exact positions (up to 2 px),\n"
                   "perspective textures on 3D models, sharp text, smooth shadows.\n",
                n);
        fclose(f);
    }
    fprintf(stderr,
            "pe: F12 frame %u: HD vs 1x: %u pixels differ strongly, %u slightly, %d areas%s -> "
            "snapshot-%05u-hd-vs-1x.txt\n",
            frame, strong, mild, n_areas, coverage < 0.9f ? " (reference incomplete)" : "", frame);
out:
    free(diff);
    free(heat);
    free(cell);
    free(seen);
    free(stack);
    free(areas);
}

/* The reference picture runs from the snapshot frame on; the comparison is
 * made at the first VSync where it has drawn the picture shown (it may be the
 * buffer drawn one frame later), at most 3 frames on. */
static int g_ref; /* 0 off, else frames since it began + 1 */
static unsigned g_ref_snap;

void pe_host_f12_ref_begin(unsigned snap_frame) {
    static int on = -1;
    if (on < 0) {
        on = !(getenv("PE_F12_REFERENCE") && atoi(getenv("PE_F12_REFERENCE")) == 0);
    }
    if (on && Psyz_VideoRefBegin() == 0) {
        g_ref = 1;
        g_ref_snap = snap_frame;
    }
}

void pe_host_f12_ref_step(unsigned frame) {
    unsigned char *ref, *hd;
    int w, h, sw, sh;
    float coverage = 0.0f;
    if (g_ref == 0) {
        return;
    }
    if (g_ref++ == 1) {
        return; /* began at this VSync: nothing drawn yet */
    }
    ref = Psyz_VideoRefCapture(&w, &h, &coverage);
    if (!ref) {
        fprintf(stderr, "pe: F12: no 1x reference picture (movie or read failed)\n");
        Psyz_VideoRefEnd();
        g_ref = 0;
        return;
    }
    if (coverage < 0.9f && g_ref <= 4) {
        free(ref);
        return; /* the picture shown is not the one drawn yet */
    }
    hd = Psyz_VideoAllocCapturedFrameScaled(&sw, &sh);
    Psyz_VideoRefEnd();
    g_ref = 0;
    if (hd) {
        compare(g_ref_snap, ref, w, h, hd, sw, sh, coverage);
        if (frame != g_ref_snap) {
            fprintf(stderr, "pe: F12: HD vs 1x compared at game frame %u\n", frame);
        }
    }
    free(ref);
    free(hd);
}
