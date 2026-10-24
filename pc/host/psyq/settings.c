/* Player settings, kept in settings.ini in the folder the game runs from
 * (the same folder as memcards/). The file is plain text:
 *
 *   resolution=1   1 = original PS1 graphics; 8 = HD: the 3D models, text and
 *                  effects drawn at 8x the PS1 resolution (PsyZ's internal
 *                  resolution), with the game's own letters redrawn smooth at
 *                  that size (font_hires.c). Textures and the pre-rendered
 *                  backgrounds keep their original pixels. Only 1 and 8:
 *                  the widescreen squeeze (3/4) and the precise geometry are
 *                  made for whole render pixels at 8x (at 6x a game pixel was
 *                  4.5 render pixels wide and the picture shimmered), so any
 *                  other value above 1 is read as 8.
 *
 *   smooth_pictures=0 1: smooth the menu icons and labels at HD (font_hires.c)
 *   smooth_shadows=1  1: at HD, shadows (small grainy textures drawn
 *                     subtractively) are mixed between texels instead of
 *                     showing each texel as a block (PsyZ 0032). 0: as drawn.
 *   soft_borders=0    1: bars beside the picture (4:3 movies, Original, rooms
 *                     stored only as 4:3) show the picture's edge mirrored,
 *                     blurred and darkened instead of black (PsyZ 0020). Also
 *                     Borders in Set User Preferences.
 * Above 1x, geometry is also precise: models are drawn at their exact
 * projected positions instead of whole pixels (PsyZ 0010).
 *   widescreen=1      16:9 above 1x (0: stay 4:3). The game's stored
 *                     backgrounds are wider than its 4:3 screen, so the sides
 *                     show the real picture (PsyZ 0009, host patch of
 *                     Render_MeshListAndFade.c, full-screen rectangles in gpu.c).
 *
 *   disc=C:\path\Parasite Eve (USA) (Disc 1).cue
 *                     Disc 1 for a start without a command line (launch.c
 *                     writes it when you pick the disc).
 *
 * Graphics in the game's Set User Preferences screen switches between HD
 * (8x) and Original (1x) (host patch of Menu_CreateMainMenuView.c). F7
 * switches between them while playing (hotkeys.c). Both write the file. A
 * missing or unreadable file means 1x.
 */
#include <psyz.h>
#include <psyz/video.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SETTINGS_FILE "settings.ini"
#define RES_HD 8 /* the only resolution above 1x */

static unsigned g_resolution = 1;
static unsigned g_widescreen_pref = 1;
static unsigned g_smooth_pictures = 0;
static unsigned g_soft_borders = 0;
static unsigned g_smooth_shadows = 1;
static char g_disc[1024];
void pe_host_font_set_scale(unsigned scale);

static void settings_write(void) {
    FILE *f = fopen(SETTINGS_FILE, "w");
    if (!f) {
        fprintf(stderr, "pe: settings: cannot write %s\n", SETTINGS_FILE);
        return;
    }
    fprintf(f, "# Parasite Eve settings\n"
               "# resolution: 1 = original PS1 graphics, 8 = HD (sharper 3D, text and effects)\n"
               "resolution=%u\n"
               "# widescreen: 1 = 16:9 with HD, 0 = always 4:3\n"
               "widescreen=%u\n"
               "# smooth_pictures: 1 = smooth the menu icons and labels at HD (Scale2x), 0 = keep their pixels\n"
               "smooth_pictures=%u\n"
               "# soft_borders: 1 = blurred picture edges beside a 4:3 picture, 0 = black bars\n"
               "soft_borders=%u\n"
               "# smooth_shadows: 1 = smooth the grainy shadows at HD, 0 = keep their pixels\n"
               "smooth_shadows=%u\n",
            g_resolution, g_widescreen_pref, g_smooth_pictures, g_soft_borders, g_smooth_shadows);
    if (g_disc[0]) {
        fprintf(f, "# disc: your Disc 1 image (Disc 2 is found next to it)\n"
                   "disc=%s\n", g_disc);
    }
    fclose(f);
}

static int g_movie_low; /* HD set aside while a movie plays (pe_host_movie_check) */
static void apply_resolution_as(unsigned res);
static void apply_resolution(void) {
    g_movie_low = 0;
    apply_resolution_as(g_resolution);
}
static void apply_resolution_as(unsigned res) {
    if (res != g_resolution) { /* a movie: Original for now, the setting stays HD */
        Psyz_VideoSetInternalResolution(res);
        fprintf(stderr, "pe: movie: resolution %ux while it plays\n", res);
        pe_host_font_set_scale(res);
        Psyz_VideoSetWidescreen(0);
        Psyz_VideoSetPrecision(0);
        return;
    }
    if (Psyz_VideoSetInternalResolution(g_resolution) != 0) {
        fprintf(stderr, "pe: settings: resolution %ux not accepted, using 1x\n", g_resolution);
        g_resolution = 1;
        Psyz_VideoSetInternalResolution(1);
    }
    fprintf(stderr, "pe: resolution %ux%s\n", g_resolution,
            g_resolution == 1 ? " (original PS1)" : "");
    pe_host_font_set_scale(g_resolution); /* sharp text from the game's font */
    Psyz_VideoSetWidescreen(g_resolution > 1 && g_widescreen_pref);
    /* precise geometry with HD: models drawn at their exact projected
     * positions, so they stop wobbling (PsyZ 0010) */
    Psyz_VideoSetPrecision(g_resolution > 1 && !getenv("PE_NO_PGXP"));
    Psyz_VideoSetSoftBorders(g_soft_borders != 0);
    Psyz_VideoSetSmoothShadows(g_smooth_shadows != 0);
}

/* Movies (24-bit display) at HD: the game's own time went up by about
 * 15 ms a frame and they stuttered (owner's timing logs; uploads, decoding,
 * VRAM copies and drawing cost the same as at 1x, the rest was not found).
 * A movie is a 320x240 picture shown as it is, so HD adds nothing to it:
 * while one plays the resolution is Original, and HD comes back after it.
 * Once per game frame; back to HD only after 10 frames without 24 bits, so
 * a short switch between movies does not flip it back and forth. */
unsigned pe_mdec_decodes(void); /* mdec.c */
void pe_host_movie_check(void) {
    static int off_frames;
    /* A movie decodes a new picture every frame; the Square logo at boot is
     * a still 24-bit picture, and switching during it broke it (owner). So
     * Original only once the 24-bit display has decoded movie pictures in 8
     * of the last 10 frames. */
    static unsigned last_decodes, history;
    unsigned d = pe_mdec_decodes();
    history = ((history << 1) | (d != last_decodes)) & 0x3FF;
    last_decodes = d;
    {
        unsigned busy = 0, h;
        for (h = history; h; h >>= 1) {
            busy += h & 1;
        }
        if (!g_movie_low && (busy < 8 || !Psyz_VideoGetRgb24())) {
            return;
        }
    }
    if (g_resolution <= 1) {
        g_movie_low = 0;
        return;
    }
    if (Psyz_VideoGetRgb24()) {
        off_frames = 0;
        if (!g_movie_low) {
            g_movie_low = 1;
            apply_resolution_as(1);
        }
    } else if (g_movie_low && ++off_frames >= 10) {
        apply_resolution();
        fprintf(stderr, "pe: movie over: back to HD\n");
    }
}

/* Widescreen margin: how much further the game draws on each side (a sixth
 * of 320, rounded up: PsyZ squeezes to 3/4 and shows a 16:9 window that can
 * slide, widescreen.c), 0 when 4:3. */
int pe_host_widescreen_margin(void) { return Psyz_VideoGetWidescreen() ? 54 : 0; }

/* Read settings.ini into the values above (nothing applied yet). */
static void settings_parse(void) {
    char line[1200];
    FILE *f = fopen(SETTINGS_FILE, "r");
    if (!f) {
        return;
    }
    while (fgets(line, sizeof(line), f)) {
        unsigned v;
        if (line[0] == '#') {
            continue;
        }
        if (strncmp(line, "disc=", 5) == 0) {
            size_t n;
            snprintf(g_disc, sizeof(g_disc), "%s", line + 5);
            n = strlen(g_disc);
            while (n && (g_disc[n - 1] == '\n' || g_disc[n - 1] == '\r' || g_disc[n - 1] == ' ')) {
                g_disc[--n] = 0;
            }
        } else if (sscanf(line, " soft_borders = %u", &v) == 1) {
            g_soft_borders = v != 0;
        } else if (sscanf(line, " smooth_shadows = %u", &v) == 1) {
            g_smooth_shadows = v != 0;
        } else if (sscanf(line, " smooth_pictures = %u", &v) == 1) {
            g_smooth_pictures = v != 0;
        } else if (sscanf(line, " widescreen = %u", &v) == 1) {
            g_widescreen_pref = v != 0;
        } else if (sscanf(line, " hd_resolution = %u", &v) == 1) {
            /* older files: HD is always 8x now */
        } else if (sscanf(line, " resolution = %u", &v) == 1) {
            g_resolution = v > 1 ? RES_HD : 1;
        }
    }
    fclose(f);
}

/* Once, at the first game frame. */
void pe_host_settings_load(void) {
    settings_parse();
    apply_resolution();
}

/* Disc 1 saved in settings.ini ("" when none); read before the game starts. */
const char *pe_host_settings_disc(void) {
    settings_parse();
    return g_disc;
}

/* Remember Disc 1; the rest of the file is kept as it is. */
void pe_host_settings_set_disc(const char *path) {
    settings_parse();
    snprintf(g_disc, sizeof(g_disc), "%s", path);
    settings_write();
}

/* F7: HD <-> Original, saved at once. */
void pe_host_settings_next_resolution(void) {
    g_resolution = g_resolution > 1 ? 1 : RES_HD;
    apply_resolution();
    settings_write();
}

int pe_host_smooth_pictures(void) { return g_smooth_pictures != 0; }

/* Borders in Set User Preferences (host patch of Menu_CreateMainMenuView.c). */
int pe_host_soft_borders(void) { return g_soft_borders != 0; }
void pe_host_set_soft_borders(int on) {
    if ((on != 0) == (g_soft_borders != 0)) {
        return;
    }
    g_soft_borders = on != 0;
    Psyz_VideoSetSoftBorders(g_soft_borders != 0);
    settings_write();
}

/* Graphics in Set User Preferences (host patch of Menu_CreateMainMenuView.c):
 * HD (8x) or Original (1x). */
int pe_host_graphics_is_hd(void) { return g_resolution > 1; }

void pe_host_graphics_set_hd(int hd) {
    unsigned want = hd ? RES_HD : 1;
    if (want == g_resolution) {
        return;
    }
    g_resolution = want;
    apply_resolution();
    settings_write();
}

/* Text for the Graphics row and popup, in the game's own text encoding: the
 * code of a character is its cell in the font (0-9, + - = * %, space, A-Z,
 * & ! ? " ' ., a-z, : , /), and 0xFF ends the text. So it is drawn by the
 * game's own text routines in its own letters. */
static const char *const g_menu_text_ascii[] = {
    "Graphics",                       /* 0: the row in Set User Preferences */
    "HD",                             /* 1: popup */
    "Original",                       /* 2: popup */
    "Select Graphics Mode",           /* 3: help line for the row */
    "Widescreen, Sharper 3D and Text", /* 4: help line for HD */
    "Original PlayStation Graphics",  /* 5: help line for Original */
    "Borders",                        /* 6: the row in Set User Preferences */
    "On",                             /* 7: popup */
    "Off",                            /* 8: popup */
    "Select Screen Borders",          /* 9: help line for the row */
    "Soft Blurred Picture Edges",     /* 10: help line for On */
    "Black Bars, as Original",        /* 11: help line for Off */
};
#define MENU_TEXTS (sizeof(g_menu_text_ascii) / sizeof(g_menu_text_ascii[0]))

static unsigned char encode_char(char c) {
    static const char order[] = "0123456789+-=*% ABCDEFGHIJKLMNOPQRSTUVWXYZ&!?\"'.abcdefghijklmnopqrstuvwxyz:,/";
    const char *p = strchr(order, c);
    return p && c ? (unsigned char)(p - order) : 0x0F; /* unknown: a space */
}

unsigned char *pe_host_menu_text(int id) {
    static unsigned char encoded[MENU_TEXTS][40];
    static int ready;
    unsigned i, j;
    if (!ready) {
        for (i = 0; i < MENU_TEXTS; i++) {
            for (j = 0; g_menu_text_ascii[i][j] && j + 1 < sizeof(encoded[i]); j++) {
                encoded[i][j] = encode_char(g_menu_text_ascii[i][j]);
            }
            encoded[i][j] = 0xFF;
        }
        ready = 1;
    }
    return encoded[id >= 0 && (unsigned)id < MENU_TEXTS ? id : 0];
}
