/* Sharp text and smooth pictures at higher resolutions, made from the game's
 * own textures (nothing is replaced).
 *
 * Two 4-bit texture pages are redrawn at the chosen resolution and handed to
 * PsyZ (Psyz_VideoSetHiresIndices4, PsyZ patch 0008), which then reads these
 * colour indices for 4-bit polygons and sprites in those pages and looks the
 * colours up in the game's palette as before:
 *
 * - The font, VRAM (320,0), texture page 5: 12x12 cells, 21 to a row
 *   (Draw_AllocTexturedQuad in src/main/gpu/Draw_TextRenderingFlow.c picks
 *   cell column*12, row*12). Each letter is 1-pixel strokes in colour index 1
 *   with a drop shadow in index 2 one pixel down and right. Every pixel of a
 *   letter becomes a round pen dot and neighbouring pixels are joined by pen
 *   strokes of the same 1-pixel width (diagonal neighbours only when no
 *   straight path joins them), so stairs become smooth lines and the letter
 *   keeps its shape and weight. The shadow is the redrawn letter moved one
 *   original pixel, as the game's is. A cell that does not look like that
 *   (other colours, or a shadow that is not the letter moved) is enlarged
 *   like the pictures below.
 *
 * - The menu and HUD pictures, VRAM (448,0), texture page 7: icons, the
 *   yellow labels (LEVEL, OFFENSE, ...), the health bar numbers, Level / HP /
 *   Time. Drawn by Draw_AllocSprite (src/main/gpu/Draw_GlyphSprites.c) from
 *   the glyph descriptors. These are redrawn with the Scale2x / Scale3x
 *   pixel-art rules (AdvanceMAME / Andrea Mazzoleni), which only compare
 *   colour indices for equality: a pixel's corner takes a neighbour's colour
 *   where two neighbours agree across a diagonal edge, so diagonal edges and
 *   curves get smoother while straight edges, flat areas and single pixels
 *   stay exactly as they were. 4x = 2x twice, 6x = 3x then 2x, 8x = 2x three
 *   times; 5x and 7x enlarge the 4x and 6x results.
 *
 * Each page is redrawn when the game uploads, copies or clears into it and
 * when the resolution changes. Uploads redraw only the part they touch, from
 * the uploaded data; copies and clears read the page back from the GPU. Any
 * write this file does not see makes PsyZ fall back to the original pixels
 * until the next upload.
 *
 * PE_FONT_TEST=dir writes each page and its redraw as PGM pictures (index 0
 * black, 1 white, 2 grey, others by value) at every full redraw;
 * PE_FONT_TIME=1 logs how long full redraws take.
 */
#include <psyz.h>
#include <psyz/video.h>
#include <libgpu.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern unsigned long long SDL_GetTicksNS(void); /* wall-clock time, for PE_FONT_TIME */

#define PAGE_W 64  /* VRAM pixels: 256 4-bit texels */
#define PAGE_H 256
#define TEX_W (PAGE_W * 4)
#define CELL 12
#define CELLS_X 21

typedef struct SharpArea {
    const char *name;
    int slot;  /* PsyZ slot */
    int x, y;  /* VRAM pixels */
    int font;  /* 1: pen-stroke letters, 0: pixel-art pictures */
    unsigned char src[PAGE_H][TEX_W]; /* 1x indices */
    unsigned char *out;               /* scale x indices */
} SharpArea;

static SharpArea g_area[2] = {
    {"font", 0, 320, 0, 1, {{0}}, NULL},
    {"pictures", 1, 448, 0, 0, {{0}}, NULL},
};
#define AREAS ((int)(sizeof(g_area) / sizeof(g_area[0])))

static unsigned g_scale = 1;

/* ---- the font: pen strokes ------------------------------------------- */

/* Distance check from point (px,py) to segment (ax,ay)-(bx,by), squared. */
static float dist2_seg(float px, float py, float ax, float ay, float bx, float by) {
    float dx = bx - ax, dy = by - ay, t = 0.0f, l2 = dx * dx + dy * dy, ex, ey;
    if (l2 > 0.0f) {
        t = ((px - ax) * dx + (py - ay) * dy) / l2;
        t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    }
    ex = ax + t * dx - px;
    ey = ay + t * dy - py;
    return ex * ex + ey * ey;
}

/* Ink a pen stroke of radius r (in 1x pixels) from the centre of pixel
 * (x0,y0) to that of (x1,y1), both cell-local, into the cell's hi-res mask. */
static void stroke(unsigned char *mask, int s, int x0, int y0, int x1, int y1, float r) {
    float ax = x0 + 0.5f, ay = y0 + 0.5f, bx = x1 + 0.5f, by = y1 + 0.5f;
    int lx = (int)(((ax < bx ? ax : bx) - r) * s) - 1, hx = (int)(((ax > bx ? ax : bx) + r) * s) + 1;
    int ly = (int)(((ay < by ? ay : by) - r) * s) - 1, hy = (int)(((ay > by ? ay : by) + r) * s) + 1;
    int X, Y, n = CELL * s;
    for (Y = ly < 0 ? 0 : ly; Y <= hy && Y < n; Y++) {
        for (X = lx < 0 ? 0 : lx; X <= hx && X < n; X++) {
            float px = (X + 0.5f) / s, py = (Y + 0.5f) / s;
            if (dist2_seg(px, py, ax, ay, bx, by) <= r * r) {
                mask[Y * n + X] = 1;
            }
        }
    }
}

/* Redraw one cell; returns 0 when it does not look like a shadowed letter
 * (then the caller's enlarged copy stays). */
static int redraw_cell(SharpArea *a, int cx, int cy, int s) {
    static unsigned char body[CELL * PSYZ_INTERNAL_RES_MAX * CELL * PSYZ_INTERNAL_RES_MAX];
    int x, y, n = CELL * s, ink = 0;
    const float r = 0.5f - 0.5f / (float)s; /* stroke as wide as one 1x pixel, minus a hi-res pixel */
#define AT(x, y) ((x) >= 0 && (y) >= 0 && (x) < CELL && (y) < CELL ? a->src[cy + (y)][cx + (x)] : 0)
#define ON(x, y) (AT(x, y) == 1)
    for (y = 0; y < CELL; y++) {
        for (x = 0; x < CELL; x++) {
            int v = AT(x, y);
            if (v > 2) {
                return 0;
            }
            /* index 2 exactly where the letter moved by (1,1) is not ink */
            if ((v == 2) != (!ON(x, y) && ON(x - 1, y - 1))) {
                return 0;
            }
            ink |= v;
        }
    }
    if (!ink) {
        return 1; /* empty: the enlarged copy is already all 0 */
    }
    memset(body, 0, (size_t)n * n);
    for (y = 0; y < CELL; y++) {
        for (x = 0; x < CELL; x++) {
            if (!ON(x, y)) {
                continue;
            }
            stroke(body, s, x, y, x, y, r + 0.5f / s); /* the dot itself: a little rounder */
            if (ON(x + 1, y)) {
                stroke(body, s, x, y, x + 1, y, r);
            }
            if (ON(x, y + 1)) {
                stroke(body, s, x, y, x, y + 1, r);
            }
            if (ON(x + 1, y + 1) && !ON(x + 1, y) && !ON(x, y + 1)) {
                stroke(body, s, x, y, x + 1, y + 1, r);
            }
            if (ON(x - 1, y + 1) && !ON(x - 1, y) && !ON(x, y + 1)) {
                stroke(body, s, x, y, x - 1, y + 1, r);
            }
        }
    }
#undef ON
#undef AT
    for (y = 0; y < n; y++) {
        for (x = 0; x < n; x++) {
            int sx = x - s, sy = y - s;
            unsigned char v = body[y * n + x] ? 1
                              : (sx >= 0 && sy >= 0 && body[sy * n + sx]) ? 2
                                                                         : 0;
            a->out[(size_t)(cy * s + y) * TEX_W * s + cx * s + x] = v;
        }
    }
    return 1;
}

/* ---- the pictures: Scale2x / Scale3x on colour indices ------------------ */

/* in: w x h, out: 2w x 2h. Outside the picture the edge pixel repeats. */
static void scale2x(const unsigned char *in, int w, int h, unsigned char *out) {
    int x, y;
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            int xl = x > 0 ? x - 1 : x, xr = x + 1 < w ? x + 1 : x;
            int yu = y > 0 ? y - 1 : y, yd = y + 1 < h ? y + 1 : y;
            unsigned char B = in[yu * w + x], D = in[y * w + xl], E = in[y * w + x];
            unsigned char F = in[y * w + xr], H = in[yd * w + x];
            unsigned char *o = out + (size_t)(2 * y) * (2 * w) + 2 * x;
            if (B != H && D != F) {
                o[0] = D == B ? D : E;
                o[1] = B == F ? F : E;
                o[2 * w] = D == H ? D : E;
                o[2 * w + 1] = H == F ? F : E;
            } else {
                o[0] = o[1] = o[2 * w] = o[2 * w + 1] = E;
            }
        }
    }
}

/* in: w x h, out: 3w x 3h (AdvanceMAME Scale3x). */
static void scale3x(const unsigned char *in, int w, int h, unsigned char *out) {
    int x, y;
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            int xl = x > 0 ? x - 1 : x, xr = x + 1 < w ? x + 1 : x;
            int yu = y > 0 ? y - 1 : y, yd = y + 1 < h ? y + 1 : y;
            unsigned char A = in[yu * w + xl], B = in[yu * w + x], C = in[yu * w + xr];
            unsigned char D = in[y * w + xl], E = in[y * w + x], F = in[y * w + xr];
            unsigned char G = in[yd * w + xl], H = in[yd * w + x], I = in[yd * w + xr];
            unsigned char *o = out + (size_t)(3 * y) * (3 * w) + 3 * x;
            int W = 3 * w;
            if (B != H && D != F) {
                o[0] = D == B ? D : E;
                o[1] = (D == B && E != C) || (B == F && E != A) ? B : E;
                o[2] = B == F ? F : E;
                o[W] = (D == B && E != G) || (D == H && E != A) ? D : E;
                o[W + 1] = E;
                o[W + 2] = (B == F && E != I) || (H == F && E != C) ? F : E;
                o[2 * W] = D == H ? D : E;
                o[2 * W + 1] = (D == H && E != I) || (H == F && E != G) ? H : E;
                o[2 * W + 2] = H == F ? F : E;
            } else {
                o[0] = o[1] = o[2] = o[W] = o[W + 1] = o[W + 2] = o[2 * W] = o[2 * W + 1] =
                    o[2 * W + 2] = E;
            }
        }
    }
}

/* The passes for a scale: their product, and nearest enlargement for the
 * rest (5x, 7x). */
static int plan_for(int s, int *passes) {
    switch (s) {
    case 2: passes[0] = 2; return 1;
    case 3: passes[0] = 3; return 1;
    case 4: case 5: passes[0] = 2; passes[1] = 2; return 2;
    case 6: case 7: passes[0] = 3; passes[1] = 2; return 2;
    default: passes[0] = 2; passes[1] = 2; passes[2] = 2; return 3; /* 8 */
    }
}

/* Smooth texel box x0..x1, y0..y1 (exclusive ends) of a picture page. The
 * rules look one pixel around, so the box is worked out with a margin of 2
 * texels of its surroundings (or the repeated page edge) and only the box
 * itself is written. */
static void smooth_box(SharpArea *a, int x0, int y0, int x1, int y1) {
    int s = (int)g_scale, passes[3], n = plan_for(s, passes), p, f = 1, i, x, y;
    int mx0 = x0 - 2 < 0 ? 0 : x0 - 2, my0 = y0 - 2 < 0 ? 0 : y0 - 2;
    int mx1 = x1 + 2 > TEX_W ? TEX_W : x1 + 2, my1 = y1 + 2 > PAGE_H ? PAGE_H : y1 + 2;
    int w = mx1 - mx0, h = my1 - my0;
    unsigned char *buf[2];
    buf[0] = malloc((size_t)w * h * 64);
    buf[1] = malloc((size_t)w * h * 64);
    if (!buf[0] || !buf[1]) {
        free(buf[0]);
        free(buf[1]);
        return;
    }
    for (y = 0; y < h; y++) {
        memcpy(buf[0] + (size_t)y * w, &a->src[my0 + y][mx0], (size_t)w);
    }
    for (p = 0; p < n; p++) {
        if (passes[p] == 2) {
            scale2x(buf[p & 1], w * f, h * f, buf[(p + 1) & 1]);
        } else {
            scale3x(buf[p & 1], w * f, h * f, buf[(p + 1) & 1]);
        }
        f *= passes[p];
    }
    /* f is the smoothed scale; enlarge to s (equal except 5x, 7x) */
    for (y = y0 * s; y < y1 * s; y++) {
        int sy = (y - my0 * s) * f / s;
        unsigned char *dst = a->out + (size_t)y * TEX_W * s;
        const unsigned char *row = buf[n & 1] + (size_t)sy * (w * f);
        for (x = x0 * s; x < x1 * s; x++) {
            dst[x] = row[(x - mx0 * s) * f / s];
        }
    }
    (void)i;
    free(buf[0]);
    free(buf[1]);
}

/* ---- both ---------------------------------------------------------------- */

static void enlarge(SharpArea *a, int x0, int y0, int x1, int y1, int s) {
    int x, y, i, j;
    for (y = y0; y < y1; y++) {
        for (x = x0; x < x1; x++) {
            for (j = 0; j < s; j++) {
                for (i = 0; i < s; i++) {
                    a->out[(size_t)(y * s + j) * TEX_W * s + x * s + i] = a->src[y][x];
                }
            }
        }
    }
}

/* Redraw texel box x0..x1, y0..y1 (exclusive ends) of an area. For the font
 * the box must be whole cells where it meets the cell grid. */
static void redraw_box(SharpArea *a, int x0, int y0, int x1, int y1, int *cells) {
    int s = (int)g_scale, x, y;
    if (!a->font) {
        smooth_box(a, x0, y0, x1, y1);
        return;
    }
    enlarge(a, x0, y0, x1, y1, s);
    for (y = y0; y + CELL <= y1 && y + CELL <= PAGE_H; y += CELL) {
        for (x = x0; x + CELL <= x1 && x + CELL <= CELLS_X * CELL; x += CELL) {
            if (redraw_cell(a, x, y, s) && cells) {
                (*cells)++;
            }
        }
    }
}

/* Test: PE_FONT_TEST=dir writes a page as a PGM picture. */
static void write_test(const unsigned char *px, int w, int h, const char *name) {
    static const unsigned char grey[4] = {0, 255, 110, 200};
    const char *dir = getenv("PE_FONT_TEST");
    char path[512];
    FILE *f;
    int i;
    if (!dir) {
        return;
    }
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    if ((f = fopen(path, "wb")) == NULL) {
        return;
    }
    fprintf(f, "P5\n%d %d\n255\n", w, h);
    for (i = 0; i < w * h; i++) {
        fputc(px[i] < 4 ? grey[px[i]] : px[i] * 16, f);
    }
    fclose(f);
}

static void unpack(SharpArea *a, const unsigned short *words) {
    int x, y;
    for (y = 0; y < PAGE_H; y++) {
        for (x = 0; x < TEX_W; x++) {
            a->src[y][x] = (unsigned char)((words[y * PAGE_W + (x >> 2)] >> ((x & 3) * 4)) & 0xF);
        }
    }
}

/* whole: the full page as 16-bit VRAM words, or NULL to read it back */
static void rebuild_from(SharpArea *a, const unsigned short *whole) {
    static unsigned short words[PAGE_H * PAGE_W];
    RECT r;
    int s = (int)g_scale, cells = 0;
    unsigned long long t0 = SDL_GetTicksNS(), t_read;
    char name[64];
    if (s < 2) {
        Psyz_VideoSetHiresIndices4(a->slot, 0, 0, 0, 0, 0, NULL);
        return;
    }
    if (whole) {
        memcpy(words, whole, sizeof(words));
    } else {
        r.x = (short)a->x;
        r.y = (short)a->y;
        r.w = PAGE_W;
        r.h = PAGE_H;
        DrawSync(0);
        StoreImage(&r, (u_long *)words);
        DrawSync(0);
    }
    t_read = SDL_GetTicksNS();
    unpack(a, words);
    free(a->out);
    a->out = malloc((size_t)TEX_W * s * PAGE_H * s);
    if (!a->out) {
        return;
    }
    redraw_box(a, 0, 0, TEX_W, PAGE_H, &cells);
    snprintf(name, sizeof(name), "%s-1x.pgm", a->name);
    write_test(&a->src[0][0], TEX_W, PAGE_H, name);
    snprintf(name, sizeof(name), "%s-%dx.pgm", a->name, s);
    write_test(a->out, TEX_W * s, PAGE_H * s, name);
    if (getenv("PE_FONT_TIME")) {
        fprintf(stderr, "pe: %s redraw took %.2f ms (%.2f ms of it reading VRAM)\n", a->name,
                (double)(SDL_GetTicksNS() - t0) / 1e6, (double)(t_read - t0) / 1e6);
    }
    if (Psyz_VideoSetHiresIndices4(a->slot, a->x, a->y, PAGE_W, PAGE_H, (unsigned)s, a->out) == 0) {
        if (a->font) {
            fprintf(stderr, "pe: sharp text at %dx (%d font cells redrawn)\n", s, cells);
        } else {
            fprintf(stderr, "pe: smooth pictures at %dx\n", s);
        }
    }
}

/* The picture page is smoothed only when settings.ini asks for it
 * (smooth_pictures=1): on the small yellow labels it looked rough. */
int pe_host_smooth_pictures(void);
static int area_enabled(const SharpArea *a) { return a->font || pe_host_smooth_pictures(); }

static int overlaps(const SharpArea *a, int x, int y, int w, int h) {
    return area_enabled(a) && x < a->x + PAGE_W && x + w > a->x && y < a->y + PAGE_H && y + h > a->y;
}

/* Before the game writes VRAM: which areas were in use (and so matched
 * VRAM) just before the write, one bit per area. */
int pe_host_font_before_write(int x, int y, int w, int h) {
    int i, bits = 0;
    for (i = 0; i < AREAS; i++) {
        if (g_scale >= 2 && g_area[i].out && overlaps(&g_area[i], x, y, w, h) &&
            Psyz_VideoHiresIndices4Active(g_area[i].slot)) {
            bits |= 1 << i;
        }
    }
    return bits;
}

/* After LoadImage(x, y, w, h, data). Where an area matched VRAM before the
 * upload, only the uploaded part (and, for pictures, the 2 texels around it
 * that its rules look at) is redrawn, from the data itself; otherwise the
 * whole area is read back and redrawn. */
void pe_host_font_after_load(int was_on, int x, int y, int w, int h, const unsigned short *data) {
    int i, row, col;
    for (i = 0; i < AREAS; i++) {
        SharpArea *a = &g_area[i];
        int cx0, cy0, cx1, cy1, tx0, tx1;
        if (g_scale < 2 || !overlaps(a, x, y, w, h)) {
            continue;
        }
        if (data && x == a->x && y == a->y && w == PAGE_W && h >= PAGE_H - 2) {
            /* the whole page (the pictures page is loaded 254 rows tall) */
            static unsigned short whole[PAGE_H * PAGE_W];
            memset(whole, 0, sizeof(whole));
            memcpy(whole, data, (size_t)PAGE_W * (h < PAGE_H ? h : PAGE_H) * 2);
            if (h < PAGE_H) { /* keep the rows it did not load */
                for (row = h; row < PAGE_H; row++) {
                    for (col = 0; col < PAGE_W; col++) {
                        int t = col * 4;
                        whole[row * PAGE_W + col] = (unsigned short)(a->src[row][t] | (a->src[row][t + 1] << 4) |
                                                                     (a->src[row][t + 2] << 8) |
                                                                     (a->src[row][t + 3] << 12));
                    }
                }
                if (!(was_on & (1 << i))) {
                    rebuild_from(a, NULL); /* rows outside the upload are unknown */
                    continue;
                }
            }
            rebuild_from(a, whole);
            continue;
        }
        if (!(was_on & (1 << i)) || !data) {
            rebuild_from(a, NULL);
            continue;
        }
        cx0 = x > a->x ? x : a->x; /* clipped, VRAM pixels */
        cy0 = y > a->y ? y : a->y;
        cx1 = x + w < a->x + PAGE_W ? x + w : a->x + PAGE_W;
        cy1 = y + h < a->y + PAGE_H ? y + h : a->y + PAGE_H;
        for (row = cy0; row < cy1; row++) {
            for (col = cx0; col < cx1; col++) {
                unsigned short v = data[(row - y) * w + (col - x)];
                int t = (col - a->x) * 4;
                a->src[row - a->y][t + 0] = v & 0xF;
                a->src[row - a->y][t + 1] = (v >> 4) & 0xF;
                a->src[row - a->y][t + 2] = (v >> 8) & 0xF;
                a->src[row - a->y][t + 3] = (v >> 12) & 0xF;
            }
        }
        tx0 = (cx0 - a->x) * 4; /* texels */
        tx1 = (cx1 - a->x) * 4;
        cy0 -= a->y;
        cy1 -= a->y;
        if (a->font) { /* whole cells inside the grid */
            if (tx0 < CELLS_X * CELL) {
                tx0 = tx0 / CELL * CELL;
                if (tx1 < CELLS_X * CELL) {
                    tx1 = (tx1 + CELL - 1) / CELL * CELL;
                }
            }
            if (cy0 < PAGE_H / CELL * CELL) {
                cy0 = cy0 / CELL * CELL;
                if (cy1 < PAGE_H / CELL * CELL) {
                    cy1 = (cy1 + CELL - 1) / CELL * CELL;
                }
            }
        } else { /* the pixels around it change too */
            tx0 = tx0 - 2 < 0 ? 0 : tx0 - 2;
            tx1 = tx1 + 2 > TEX_W ? TEX_W : tx1 + 2;
            cy0 = cy0 - 2 < 0 ? 0 : cy0 - 2;
            cy1 = cy1 + 2 > PAGE_H ? PAGE_H : cy1 + 2;
        }
        tx0 = tx0 / 4 * 4; /* whole VRAM pixels */
        tx1 = (tx1 + 3) / 4 * 4;
        redraw_box(a, tx0, cy0, tx1, cy1, NULL);
        if (getenv("PE_FONT_TIME")) {
            fprintf(stderr, "pe: %s part redrawn %d,%d %dx%d\n", a->name, tx0, cy0, tx1 - tx0, cy1 - cy0);
        }
        if (Psyz_VideoUpdateHiresIndices4(a->slot, a->x + tx0 / 4, a->y + cy0, (tx1 - tx0) / 4, cy1 - cy0,
                                          a->out) != 0) {
            rebuild_from(a, NULL);
        }
    }
}

/* After a VRAM copy or clear that reached an area: read it again. */
void pe_host_font_vram_written(int x, int y, int w, int h) {
    int i;
    for (i = 0; i < AREAS; i++) {
        if (g_scale >= 2 && overlaps(&g_area[i], x, y, w, h)) {
            rebuild_from(&g_area[i], NULL);
        }
    }
}

/* The resolution setting changed (settings.c). */
void pe_host_font_set_scale(unsigned scale) {
    int i;
    g_scale = scale;
    for (i = 0; i < AREAS; i++) {
        if (area_enabled(&g_area[i])) {
            rebuild_from(&g_area[i], NULL);
        } else {
            Psyz_VideoSetHiresIndices4(g_area[i].slot, 0, 0, 0, 0, 0, NULL);
        }
    }
}
