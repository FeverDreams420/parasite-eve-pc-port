/* Widescreen: keep the 16:9 window on the picture at a room's ends.
 *
 * With HD the game draws 54 pixels further on each side than its 320-pixel
 * screen (settings.c, PsyZ 0009), and the screen shows a 16:9 window of that:
 * 224 lines x 16/9 for the game's pixel aspect (PsyZ reports it: about 372
 * pixels in the field, so 26 pixels of extra picture on each side when
 * centred). At the end of a scrolling room the stored background
 * stops, which showed as black boxes at that side. The background tiles the
 * game links each frame (host patch of Render_DrawSpriteEntry,
 * Render_MeshListAndFade.c) say where the picture is: for every 8th screen
 * line, the left and right ends of the tiles crossing it (all layers); the
 * picture's edge is the innermost of those. The
 * window then slides towards the side that has picture, never so far that
 * the game's own 320-pixel screen leaves it (menus and text may sit off
 * centre there, up to the window's edge). A room whose stored
 * picture is narrower than the window (some rooms are just the 4:3 screen)
 * gets equal bars on both sides, centred on its picture.
 */
#include <psyz.h>
#include <psyz/video.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define LINES 28         /* screen lines sampled: 0, 8, ... 216 of the 224 */

/* Per sampled screen line: the left and right ends of the background tiles
 * (all layers together) that cross it. A room's picture can be made of
 * several layers side by side, and pieces of one layer overlap rows of
 * another, so lines, not tile rows, are what is covered. */
static float g_last_shift; /* for the burst log (forward.c) */
float pe_host_ws_last_shift(void) { return g_last_shift; }
static float g_last_left, g_last_right; /* for the recorder (recorder.c) */
void pe_host_ws_last_picture(float *left, float *right) {
    *left = g_last_left;
    *right = g_last_right;
}

static int g_line_min[LINES], g_line_max[LINES], g_lines_used, g_tiles, g_tiles_frame;

/* Every screen line's picture ends, for the soft borders only (the window
 * still follows the sampled lines above): where one layer stops short of the
 * others on some lines (the precinct basement's floor runs 11 pixels below
 * the walls and is narrower), the bar starts there instead of leaving a thin
 * hard black corner between the picture and the bar. left > right: none. */
#define ROWS 256
static short g_row_l[ROWS], g_row_r[ROWS];
static int g_rows_reset;

/* Debug: PE_LOG_WS=1 logs the picture's ends and the slide each frame, 2 also
 * each sampled line's ends, 3 also every tile. Read once (getenv is slow on
 * Windows). */
static int ws_log_level(void) {
    static int level = -1;
    if (level < 0) {
        const char *e = getenv("PE_LOG_WS");
        level = e ? (e[0] >= '1' && e[0] <= '9' ? e[0] - '0' : 1) : 0;
    }
    return level;
}

/* Half the band around each sampled line that a tile must cross to count.
 * Layers of one picture can start a pixel or two apart in height: in the
 * precinct basement the right-hand layer begins 1 pixel below the left one,
 * so while the camera scrolled up or down the top sampled line lost the
 * right-hand layer every few pixels of scroll, the picture seemed to end at
 * 288 instead of 331 and the window jumped 22 pixels back and forth. A gap
 * that thin is a row of the screen, not a narrower picture. */
#define LINE_BAND 4

/* Which columns of a 16x16 background tile hold picture, per row: lo[j]..hi[j]
 * (hi exclusive; lo = 16 for an empty row). A tile is drawn whole, but the
 * stored picture can stop inside it: in the precinct basement the right wall's
 * last tiles (x 315) hold 5 columns of picture and 11 empty ones, so the
 * picture ended at 320 while the tiles said 331, and the soft border started
 * 11 pixels out, past a hard black strip that Aya's leg was drawn over. A
 * texel counts as empty when it is transparent (0x0000), and, outside the
 * game's own 320-pixel screen, also when it is opaque black (0x8000): inside
 * the screen black is part of the picture (the NYPD upstairs hallway's
 * ceiling is opaque black tiles; counted as empty, the picture seemed a
 * narrow wedge at the top lines and the window jumped), outside it black is
 * only padding. Read from PsyZ's copy of the VRAM; returns 0 when the whole
 * tile is empty. */
struct RenderTilePacketView { /* RenderTilePacket, include/pe1/render_prim.h */
    unsigned char address[3], length;
    unsigned char r, g, b, code;
    unsigned short x, y;
    unsigned char u, v;
    unsigned short clut;
};
static unsigned g_dbg_texels[3]; /* debug: transparent, opaque black, other */
unsigned pe_host_ws_dbg(int i) { return g_dbg_texels[i]; }
/* Opaque black this close to the 320-pixel screen's sides is padding the TV's
 * overscan hid (the police car scene's street: the last column, 319, black on
 * the upper lines); for the soft borders it counts as empty (blo/bhi), so the
 * bar starts before it instead of after a black line. The window keeps lo/hi. */
#define EDGE_PAD 4

static int tile_columns(const void *sprite, const void *page, int x, signed char lo[16],
                        signed char hi[16], signed char blo[16], signed char bhi[16]) {
    const struct RenderTilePacketView *t = sprite;
    const unsigned short *vram = Psyz_VramShadow();
    unsigned int tp = ((const unsigned int *)page)[1]; /* E1 command */
    int bx = (tp & 0xF) * 64, by = ((tp >> 4) & 1) * 256, depth = (tp >> 7) & 3;
    int cx = (t->clut & 0x3F) * 16, cy = (t->clut >> 6) & 0x1FF;
    int i, j, any = 0;
    if (!vram || depth == 3) {
        return -1; /* unknown: count the whole tile */
    }
    for (j = 0; j < 16; j++) {
        int tv = (t->v + j) & 0xFF;
        lo[j] = blo[j] = 16;
        hi[j] = bhi[j] = 0;
        for (i = 0; i < 16; i++) {
            int tu = (t->u + i) & 0xFF;
            unsigned short c;
            if (depth == 0) {
                unsigned short w = vram[(by + tv) * 1024 + ((bx + tu / 4) & 1023)];
                c = vram[cy * 1024 + cx + ((w >> ((tu & 3) * 4)) & 0xF)];
            } else if (depth == 1) {
                unsigned short w = vram[(by + tv) * 1024 + ((bx + tu / 2) & 1023)];
                c = vram[cy * 1024 + ((cx + ((w >> ((tu & 1) * 8)) & 0xFF)) & 1023)];
            } else {
                c = vram[(by + tv) * 1024 + ((bx + tu) & 1023)];
            }
            g_dbg_texels[c == 0 ? 0 : (c == 0x8000 ? 1 : 2)]++;
            if ((c & 0x7FFF) || (c && x + i >= 0 && x + i < 320)) {
                if (lo[j] == 16) {
                    lo[j] = (signed char)i;
                }
                hi[j] = (signed char)(i + 1);
                any = 1;
                if ((c & 0x7FFF) || (x + i >= EDGE_PAD && x + i < 320 - EDGE_PAD)) {
                    if (blo[j] == 16) {
                        blo[j] = (signed char)i;
                    }
                    bhi[j] = (signed char)(i + 1);
                }
            }
        }
    }
    return any;
}

/* Whether any room background has been drawn since start (the boot logos
 * and the title draw none), for the movie switch (settings.c). */
static int g_room_seen;
int pe_host_room_seen(void) { return g_room_seen; }

/* A background tile was linked at (x, y), 16x16, screen pixels. */
void pe_host_ws_tile(void *layer, int x, int y, const void *sprite, const void *page) {
    int l, tlo = 0, thi = 16;
    signed char lo[16], hi[16], blo[16], bhi[16];
    int cols;
    (void)layer;
    x = (short)x; /* the game keeps these as 16-bit screen values */
    y = (short)y;
    g_tiles_frame++;
    g_tiles++;
    g_room_seen = 1;
    unsigned d0 = g_dbg_texels[0], d1 = g_dbg_texels[1];
    cols = tile_columns(sprite, page, x, lo, hi, blo, bhi);
    if (cols == 0) {
        if (ws_log_level() >= 3) {
            fprintf(stderr, "pe: ws tile %p %d,%d empty transparent %u black %u\n", layer, (short)x,
                    (short)y, g_dbg_texels[0] - d0, g_dbg_texels[1] - d1);
        }
        return; /* no picture in it at all */
    }
    if (cols > 0) { /* the tile's picture, all rows together */
        tlo = 16;
        thi = 0;
        for (l = 0; l < 16; l++) {
            tlo = lo[l] < tlo ? lo[l] : tlo;
            thi = hi[l] > thi ? hi[l] : thi;
        }
    } else {
        for (l = 0; l < 16; l++) {
            lo[l] = blo[l] = 0;
            hi[l] = bhi[l] = 16;
        }
    }
    if (!g_rows_reset) {
        for (l = 0; l < ROWS; l++) {
            g_row_l[l] = 32767;
            g_row_r[l] = -32768;
        }
        g_rows_reset = 1;
    }
    for (l = y < 0 ? 0 : y; l < y + 16 && l < ROWS; l++) {
        if (blo[l - y] < bhi[l - y]) {
            g_row_l[l] = x + blo[l - y] < g_row_l[l] ? (short)(x + blo[l - y]) : g_row_l[l];
            g_row_r[l] = x + bhi[l - y] > g_row_r[l] ? (short)(x + bhi[l - y]) : g_row_r[l];
        }
    }
    if (ws_log_level() >= 3) {
        fprintf(stderr, "pe: ws tile %p %d,%d cols %d..%d transparent %u black %u\n", layer, x, y, tlo, thi, g_dbg_texels[0] - d0, g_dbg_texels[1] - d1);
    }
    for (l = 0; l < LINES; l++) {
        int line = l * 8;
        if (line + LINE_BAND <= y || line - LINE_BAND >= y + 16) {
            continue;
        }
        if (!(g_lines_used & (1 << l))) {
            g_lines_used |= 1 << l;
            g_line_min[l] = x + tlo;
            g_line_max[l] = x + thi;
        } else {
            g_line_min[l] = x + tlo < g_line_min[l] ? x + tlo : g_line_min[l];
            g_line_max[l] = x + thi > g_line_max[l] ? x + thi : g_line_max[l];
        }
    }
}

/* An ordering table is about to be drawn: set the window for its buffer from
 * the tiles linked since the last one. */
void pe_host_ws_commit(void) {
    float left = -1e9f, right = 1e9f, shift = 0.0f, max_shift = 0.0f;
    int l;
    if (!g_lines_used || !Psyz_VideoGetWidescreen()) {
        g_lines_used = 0;
        g_rows_reset = 0;
        g_tiles = 0;
        return;
    }
    /* The picture's ends: where most sampled lines end (at least 70% of them
     * reach that far), not the innermost line. A room's picture can stop
     * short on a minority of lines: in the precinct basement the floor layer
     * runs 11 pixels lower than the walls but is only 320 wide, and in a
     * museum room (Maeda's cutscene) the stored picture's left edge has an
     * empty diagonal wedge over a quarter of the lines. Counted, they moved
     * the window back and forth as the camera scrolled; the soft borders now
     * cover such lines (line by line). A room end shared by most lines still
     * stops the window. */
    {
        int mins[LINES], maxs[LINES], n = 0, k, i, j;
        for (l = 0; l < LINES; l++) {
            if (g_lines_used & (1 << l)) {
                mins[n] = g_line_min[l];
                maxs[n] = g_line_max[l];
                n++;
            }
        }
        for (i = 1; i < n; i++) { /* sort: mins ascending, maxs descending */
            int a = mins[i], b = maxs[i];
            for (j = i; j > 0 && mins[j - 1] > a; j--) {
                mins[j] = mins[j - 1];
            }
            mins[j] = a;
            for (j = i; j > 0 && maxs[j - 1] < b; j--) {
                maxs[j] = maxs[j - 1];
            }
            maxs[j] = b;
        }
        k = (n * 7 + 9) / 10 - 1; /* ceil(70% of n) - 1 */
        if (k < 0) {
            k = 0;
        }
        left = (float)mins[k];
        right = (float)maxs[k];
    }
    {
        /* the window: 16:9 of the lines shown, for the game's pixel aspect */
        /* the slide limit in whole game pixels, rounded up (PsyZ allows the
         * same): with it rounded to nearest, the last step at a room's end
         * came out a pixel short and the whole picture jumped there. The
         * game's screen may then pass the window's edge by a fraction of a
         * pixel. */
        float half = Psyz_VideoGetWidescreenHalfView();
        max_shift = half - 160.0f > 0.0f ? ceilf(half - 160.0f) : 0.0f;
        float need_left = -max_shift, need_right = 320.0f + max_shift;
        /* A picture narrower than the window is centred (equal bars) by its
         * width, wherever it sits: tested by its ends instead, a room a bit
         * narrower than the window (NYPD upstairs, 360 wide) was pushed to
         * one end and the whole gap went to the other side, switching sides
         * as the camera scrolled (owner, 2026-10-23). */
        if (right - left <= 2.0f * half) {
            shift = (left + right) * 0.5f - 160.0f; /* narrower: equal bars */
        } else if (left > need_left) {
            shift = left - need_left;
        } else if (right < need_right) {
            shift = right - need_right;
        }
    }
    shift = shift > max_shift ? max_shift : (shift < -max_shift ? -max_shift : shift);
    /* whole game pixels, so the window moves by whole render pixels at 4x/8x */
    shift = (float)(int)(shift < 0 ? shift - 0.5f : shift + 0.5f);
    if (shift > max_shift) {
        shift -= 1.0f;
    } else if (shift < -max_shift) {
        shift += 1.0f;
    }
    Psyz_VideoSetWidescreenShift(shift);
    Psyz_VideoSetWidescreenPicture(left, right); /* for soft borders */
    Psyz_VideoSetWidescreenPictureRows(g_row_l, g_row_r, ROWS);
    g_last_shift = shift;
    g_last_left = left;
    g_last_right = right;
    if (ws_log_level()) { /* debug: the picture's ends and the slide */
        unsigned int pe_host_frame_number(void);
        fprintf(stderr, "pe: frame %u widescreen picture %.0f..%.0f shift %.1f (%d tiles)\n",
                pe_host_frame_number(), left, right, shift, g_tiles);
        if (ws_log_level() >= 2) {
            for (l = 0; l < LINES; l++) {
                if (g_lines_used & (1 << l)) {
                    fprintf(stderr, " %d:%d..%d", l * 8, g_line_min[l], g_line_max[l]);
                } else {
                    fprintf(stderr, " %d:-", l * 8);
                }
            }
            fprintf(stderr, "\n");
        }
    }
    g_lines_used = 0;
    g_rows_reset = 0;
    g_tiles = 0;
}

/* Once per game frame (VSync): a frame with no background (title, menus
 * over black, movies) goes back to the centred window. */
void pe_host_ws_frame(void) {
    if (g_tiles_frame == 0) {
        Psyz_VideoResetWidescreenShift();
    }
    g_tiles_frame = 0;
}
