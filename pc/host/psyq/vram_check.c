/* VRAM check (debug): is what the textures are read from still what the game
 * put there? PsyZ keeps a CPU copy of the 1x VRAM from every upload, fill and
 * copy (Psyz_VramShadow); Psyz_VideoVramCheck compares the GPU's VRAM with it
 * wherever nothing has been drawn since: the 1x VRAM, the 1x copy that
 * polygons sample, and the HD VRAM (which a drawn area is copied down from).
 * A difference there is a stale or wrong texture, CLUT or picture.
 *
 *   F12                      checks every HD texel and writes
 *                            vram-check-NNNNN.txt / .ppm in the F12 folder
 *                            (PE_VRAM_CHECK=0: not)
 *   PE_VRAM_CHECK_EVERY=n    a quick check every n seconds (one HD texel
 *                            per pixel); a log line when the result changes,
 *                            files when something differs (at most 20 sets)
 *
 * vram-check-NNNNN.txt: the counts and each area that differs (16x16 blocks
 * joined), with its texture pages and a few pixels (expected / got).
 * vram-check-NNNNN.ppm (1024x512): VRAM as the game wrote it, dimmed; dark
 * blue = drawn since (not checked); red = HD differs; yellow = 1x differs;
 * magenta = only the sampled 1x copy differs; cyan = HD copy still pending.
 * With differences also -expected.ppm, -native.ppm and -hd.ppm (full colour). */
#include <psyz.h>
#include <psyz/video.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

double pe_host_now_s(void);
const char *pe_host_f12_path(char *buf, size_t size, const char *fmt, ...);

#define VW 1024
#define VH 512
#define TILE 16
#define TW (VW / TILE)
#define TH (VH / TILE)
#define BAD (PSYZ_VCHK_NATIVE | PSYZ_VCHK_SAMPLE | PSYZ_VCHK_HD)

static void rgb_of(unsigned short c, unsigned char *p) {
    p[0] = (unsigned char)((c & 31) * 255 / 31);
    p[1] = (unsigned char)(((c >> 5) & 31) * 255 / 31);
    p[2] = (unsigned char)(((c >> 10) & 31) * 255 / 31);
}

static void write_vram_ppm(const char *path, const unsigned short *v) {
    FILE *f = fopen(path, "wb");
    unsigned char *rgb = malloc((size_t)VW * VH * 3);
    if (f && rgb) {
        for (int i = 0; i < VW * VH; i++) {
            rgb_of(v[i], rgb + (size_t)i * 3);
        }
        fprintf(f, "P6\n%d %d\n255\n", VW, VH);
        fwrite(rgb, 3, (size_t)VW * VH, f);
    }
    free(rgb);
    if (f) {
        fclose(f);
    }
}

static void write_heat_map(const char *path, const Psyz_VramCheck *c, const unsigned short *want) {
    FILE *f = fopen(path, "wb");
    unsigned char *rgb = malloc((size_t)VW * VH * 3);
    if (f && rgb) {
        for (int i = 0; i < VW * VH; i++) {
            unsigned char *p = rgb + (size_t)i * 3;
            const unsigned char st = c->status[i];
            rgb_of(want[i], p);
            p[0] = (unsigned char)(p[0] * 2 / 5);
            p[1] = (unsigned char)(p[1] * 2 / 5);
            p[2] = (unsigned char)(p[2] * 2 / 5);
            if (!(st & PSYZ_VCHK_KNOWN)) {
                p[0] = (unsigned char)(p[0] / 3);
                p[1] = (unsigned char)(p[1] / 3);
                p[2] = (unsigned char)(p[2] / 3 + 70);
            } else if (st & PSYZ_VCHK_NATIVE) {
                p[0] = 255, p[1] = 255, p[2] = 0;
            } else if (st & PSYZ_VCHK_HD) {
                p[0] = 255, p[1] = 0, p[2] = 0;
            } else if (st & PSYZ_VCHK_SAMPLE) {
                p[0] = 255, p[1] = 0, p[2] = 255;
            } else if (st & PSYZ_VCHK_SKIP_HD) {
                p[0] = 0, p[1] = 160, p[2] = 160;
            }
        }
        fprintf(f, "P6\n%d %d\n255\n", VW, VH);
        fwrite(rgb, 3, (size_t)VW * VH, f);
    }
    free(rgb);
    if (f) {
        fclose(f);
    }
}

/* Areas: 16x16 blocks with a difference, joined with their 8 neighbours. */
typedef struct {
    int x0, y0, x1, y1; /* pixels that differ: bounding box, inclusive */
    unsigned native, sample, hd;
} Area;

static int find_areas(const Psyz_VramCheck *c, Area *areas, int max) {
    static unsigned char tile[TH][TW], seen[TH][TW];
    static int stack[TW * TH];
    int n = 0;
    memset(tile, 0, sizeof(tile));
    memset(seen, 0, sizeof(seen));
    for (int y = 0; y < VH; y++) {
        for (int x = 0; x < VW; x++) {
            if (c->status[y * VW + x] & BAD) {
                tile[y / TILE][x / TILE] = 1;
            }
        }
    }
    for (int ty = 0; ty < TH; ty++) {
        for (int tx = 0; tx < TW; tx++) {
            if (!tile[ty][tx] || seen[ty][tx]) {
                continue;
            }
            Area a = {VW, VH, -1, -1, 0, 0, 0};
            int sp = 0;
            stack[sp++] = ty * TW + tx;
            seen[ty][tx] = 1;
            while (sp) {
                const int t = stack[--sp], cy = t / TW, cx = t % TW;
                for (int y = cy * TILE; y < cy * TILE + TILE; y++) {
                    for (int x = cx * TILE; x < cx * TILE + TILE; x++) {
                        const unsigned char st = c->status[y * VW + x];
                        if (!(st & BAD)) {
                            continue;
                        }
                        a.x0 = x < a.x0 ? x : a.x0;
                        a.y0 = y < a.y0 ? y : a.y0;
                        a.x1 = x > a.x1 ? x : a.x1;
                        a.y1 = y > a.y1 ? y : a.y1;
                        a.native += (st & PSYZ_VCHK_NATIVE) != 0;
                        a.sample += (st & PSYZ_VCHK_SAMPLE) != 0;
                        a.hd += (st & PSYZ_VCHK_HD) != 0;
                    }
                }
                for (int dy = -1; dy <= 1; dy++) {
                    for (int dx = -1; dx <= 1; dx++) {
                        const int ny = cy + dy, nx = cx + dx;
                        if (ny >= 0 && ny < TH && nx >= 0 && nx < TW && tile[ny][nx] &&
                            !seen[ny][nx]) {
                            seen[ny][nx] = 1;
                            stack[sp++] = ny * TW + nx;
                        }
                    }
                }
            }
            if (n < max) {
                areas[n] = a;
            }
            n++;
        }
    }
    return n;
}

static void write_report(const char *path, unsigned frame, const char *why, const Psyz_VramCheck *c,
                         const unsigned short *want, const Area *areas, int n_areas, int max) {
    FILE *f = fopen(path, "w");
    if (!f) {
        return;
    }
    fprintf(f, "VRAM check, game frame %u (%s), %dx, %s\n", frame, why, c->scale,
            c->scale <= 1 ? "1x only" : (c->full ? "every HD texel" : "one HD texel per pixel"));
    fprintf(f, "checked %u of %d pixels (the rest was drawn into since it was written)\n",
            c->n_known, VW * VH);
    fprintf(f, "differ: 1x VRAM %u, sampled 1x copy %u, HD %u", c->n_native, c->n_sample, c->n_hd);
    if (c->full && c->n_hd) {
        fprintf(f, " (%u HD texels)", c->n_hd_texels);
    }
    fprintf(f, "\n");
    if (c->sample_pending[2] > 0) {
        fprintf(f, "sampled copy pending (not compared): %d,%d %dx%d\n", c->sample_pending[0],
                c->sample_pending[1], c->sample_pending[2], c->sample_pending[3]);
    }
    if (c->hd_pending[2] > 0) {
        fprintf(f, "HD copy pending (not compared): %d,%d %dx%d, %u pixels\n", c->hd_pending[0],
                c->hd_pending[1], c->hd_pending[2], c->hd_pending[3], c->n_skip_hd);
    }
    fprintf(f, "areas that differ: %d%s\n", n_areas, n_areas > max ? " (first ones listed)" : "");
    for (int i = 0; i < n_areas && i < max; i++) {
        const Area *a = &areas[i];
        int shown = 0;
        fprintf(f, "\narea %d: x %d..%d, y %d..%d (%dx%d); pixels: 1x %u, sampled %u, HD %u\n",
                i + 1, a->x0, a->x1, a->y0, a->y1, a->x1 - a->x0 + 1, a->y1 - a->y0 + 1, a->native,
                a->sample, a->hd);
        fprintf(f, "  texture pages (x/64, y/256):");
        for (int py = a->y0 / 256; py <= a->y1 / 256; py++) {
            for (int px = a->x0 / 64; px <= a->x1 / 64; px++) {
                fprintf(f, " %d,%d", px, py);
            }
        }
        fprintf(f, "\n  pixel      expected 1x    sampled  HD\n");
        for (int y = a->y0; y <= a->y1 && shown < 8; y++) {
            for (int x = a->x0; x <= a->x1 && shown < 8; x++) {
                const int j = y * VW + x;
                if (!(c->status[j] & BAD)) {
                    continue;
                }
                fprintf(f, "  %4d,%3d   %04X     %04X  %04X     ", x, y, want[j], c->native[j],
                        c->sample[j]);
                if (c->hd) {
                    fprintf(f, "%04X", c->hd[j]);
                }
                fprintf(f, "\n");
                shown++;
            }
        }
    }
    fclose(f);
}

/* Runs the check; logs one line (always when forced, else when the result
 * changed) and writes the files when forced or when something differs. */
static void vram_check_run(unsigned frame, int full, int forced) {
    static unsigned last_native = ~0u, last_sample = ~0u, last_hd = ~0u;
    static int file_sets;
    enum { MAX_AREAS = 64 };
    static Area areas[MAX_AREAS];
    Psyz_VramCheck c;
    const double t0 = pe_host_now_s();
    if (Psyz_VideoVramCheck(&c, full) != 0) {
        fprintf(stderr, "pe: vram check: could not read the VRAM back\n");
        return;
    }
    const double ms = (pe_host_now_s() - t0) * 1000.0;
    const unsigned short *want = Psyz_VramShadow();
    const int n_areas = find_areas(&c, areas, MAX_AREAS);
    const int bad = c.n_native || c.n_sample || c.n_hd;
    const int changed = c.n_native != last_native || c.n_sample != last_sample || c.n_hd != last_hd;
    last_native = c.n_native, last_sample = c.n_sample, last_hd = c.n_hd;
    const int files = forced || (bad && changed && file_sets < 20);
    if (forced || changed) {
        fprintf(stderr,
                "pe: vram check frame %u (%dx%s): %u pixels checked, differ: 1x %u, sampled %u, HD %u"
                " in %d areas (%.0f ms)%s",
                frame, c.scale, c.full ? ", every texel" : "", c.n_known, c.n_native, c.n_sample,
                c.n_hd, n_areas, ms, files ? "" : "\n");
    }
    if (files) {
        char path[128];
        if (!forced) {
            file_sets++;
        }
#define VCHK_PATH(...) \
    (forced ? (void)pe_host_f12_path(path, sizeof(path), __VA_ARGS__) \
            : (void)snprintf(path, sizeof(path), __VA_ARGS__))
        VCHK_PATH("vram-check-%05u.txt", frame);
        write_report(path, frame, forced ? "F12" : "every few seconds", &c, want, areas, n_areas,
                     MAX_AREAS);
        VCHK_PATH("vram-check-%05u.ppm", frame);
        write_heat_map(path, &c, want);
        if (bad) {
            VCHK_PATH("vram-check-%05u-expected.ppm", frame);
            write_vram_ppm(path, want);
            VCHK_PATH("vram-check-%05u-native.ppm", frame);
            write_vram_ppm(path, c.native);
            if (c.hd) {
                VCHK_PATH("vram-check-%05u-hd.ppm", frame);
                write_vram_ppm(path, c.hd);
            }
        }
        fprintf(stderr, " -> vram-check-%05u.txt\n", frame);
    }
    Psyz_VideoVramCheckFree(&c);
}

/* F12 (forward.c snapshot), before the snapshot reads the VRAM back. */
void pe_host_vram_check_snapshot(unsigned frame) {
    static int on = -1;
    if (on < 0) {
        on = !getenv("PE_VRAM_CHECK") || atoi(getenv("PE_VRAM_CHECK")) != 0;
    }
    if (on) {
        vram_check_run(frame, 1, 1);
    }
}

/* Every game frame (forward.c). */
void pe_host_vram_check_frame(unsigned frame) {
    static double every = -1.0, next;
    if (every < 0.0) {
        every = getenv("PE_VRAM_CHECK_EVERY") ? atof(getenv("PE_VRAM_CHECK_EVERY")) : 0.0;
        if (every > 0.0) {
            fprintf(stderr, "pe: vram check every %.1f s\n", every);
            next = pe_host_now_s() + every;
        }
    }
    if (every > 0.0 && pe_host_now_s() >= next) {
        vram_check_run(frame, 0, 0);
        next = pe_host_now_s() + every;
    }
}
