/* Test hotkeys.
 *
 *   ` / ~ (the key under Escape)  skip the movie that is playing: while it
 *        is held, the next movie frame handed to the game is marked as the
 *        movie's last frame (stream.c), so the game ends the movie the normal
 *        way. Only the opening movies' player has a skip button of its own,
 *        so this works for the story movies too.
 *   Tab  fast forward while held: the frame waits are skipped (one refresh
 *        step per game frame, no clock waits) and PsyZ's frame limiter and
 *        driver VSync are switched off; they are restored on release. The
 *        timers (the sound driver's) follow the game's blanks meanwhile.
 *        Movies keep their own pace (CD streaming runs on the clock).
 *
 * Keys are read from SDL's keyboard state, so only while the game window has
 * focus. Scan codes, not characters: 53 is the key top-left under Escape on
 * any layout, 43 is Tab (SDL_scancode.h).
 *
 *   F12  snapshot: the next frame's GPU commands (snapshot-NNNNN-gpu.txt), its
 *        picture and video memory (snapshot-NNNNN-frame.ppm / -vram.ppm),
 *        written next to the program, for finding drawing glitches.
 *
 *   F5   save: opens the memory-card save screen, as a phone does, wherever
 *        the in-game menu could be opened (host patch of Entity_FrameUpdate).
 *
 *   F7   graphics: switches between HD (8x) and Original (1x, PS1) and
 *        saves it in settings.ini (settings.c).
 *
 * For tests without a keyboard, PE_KEY_SCRIPT="first-last:mask,..." holds
 * keys for game frames first..last (mask 1 = skip movie, 2 = fast forward,
 * 4 = snapshot, 8 = save, 16 = HD / Original).
 */
#include <psyz.h>
#include <psyz/video.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

extern const bool *SDL_GetKeyboardState(int *numkeys);
unsigned int pe_host_frame_number(void);

#define KEY_SKIP 1
#define KEY_FAST 2
#define SCANCODE_GRAVE 53
#define SCANCODE_TAB 43
#define SCANCODE_F12 69
#define KEY_SNAP 4
#define KEY_SAVE 8
#define SCANCODE_F5 62
#define KEY_RES 16
#define SCANCODE_F7 64

static unsigned int script_keys(unsigned int frame) {
    static const char *script;
    static int init;
    const char *p;
    unsigned int m = 0;
    if (!init) {
        init = 1;
        script = getenv("PE_KEY_SCRIPT");
    }
    for (p = script; p && *p;) {
        char *e;
        unsigned long a = strtoul(p, &e, 10), b = a, mask = 0;
        if (*e == '-') {
            b = strtoul(e + 1, &e, 10);
        }
        if (*e == ':') {
            mask = strtoul(e + 1, &e, 0);
        }
        if (frame >= a && frame <= b) {
            m |= (unsigned int)mask;
        }
        p = (*e == ',') ? e + 1 : NULL;
    }
    return m;
}

static unsigned int keys_now(void) {
    int n = 0;
    const bool *k = SDL_GetKeyboardState(&n);
    unsigned int m = script_keys(pe_host_frame_number());
    if (k && n > SCANCODE_GRAVE) {
        if (k[SCANCODE_GRAVE]) {
            m |= KEY_SKIP;
        }
        if (k[SCANCODE_TAB]) {
            m |= KEY_FAST;
        }
        if (n > SCANCODE_F12 && k[SCANCODE_F12]) {
            m |= KEY_SNAP;
        }
        if (n > SCANCODE_F5 && k[SCANCODE_F5]) {
            m |= KEY_SAVE;
        }
        if (n > SCANCODE_F7 && k[SCANCODE_F7]) {
            m |= KEY_RES;
        }
    }
    return m;
}

static unsigned int g_keys;
static int g_save_request;
static int g_fast;
static PsyzVsyncMode g_saved_mode;

/* Once per game frame, from VSync. */
void pe_host_snapshot_request(void);
void pe_host_settings_load(void);
void pe_host_settings_next_resolution(void);
void pe_host_ws_frame(void);
void Psyz_KernelSetVBlankClock(int on);

void pe_host_hotkeys_frame(void) {
    int fast;
    unsigned int before = g_keys;
    static int settings_loaded;
    if (!settings_loaded) {
        settings_loaded = 1;
        pe_host_settings_load();
    }
    pe_host_ws_frame(); /* widescreen.c */
    g_keys = keys_now();
    if ((g_keys & KEY_RES) && !(before & KEY_RES)) {
        pe_host_settings_next_resolution();
    }
    if ((g_keys & KEY_SNAP) && !(before & KEY_SNAP)) {
        pe_host_snapshot_request();
    }
    if ((g_keys & KEY_SAVE) && !(before & KEY_SAVE)) {
        g_save_request = 1;
    }
    fast = (g_keys & KEY_FAST) != 0;
    if (fast != g_fast) {
        if (fast) {
            g_saved_mode = Psyz_VideoGetVsyncMode();
            Psyz_VideoSetVsyncMode(PSYZ_VSYNC_LIMITLESS);
        } else {
            Psyz_VideoSetVsyncMode(g_saved_mode);
        }
        /* The sound driver runs on a timer (root counter 2). While fast
         * forward runs the game faster than real time, that timer follows the
         * game's blanks, not the clock; otherwise the game gave the driver
         * commands faster than it took them, stops were lost and sounds kept
         * looping (the battle healing sound, every watchdog report in the log
         * came with fast forward on). */
        Psyz_KernelSetVBlankClock(fast);
        g_fast = fast;
        fprintf(stderr, "pe: fast forward %s\n", fast ? "on" : "off");
    }
}

int pe_host_fast_forward(void) { return g_fast; }

/* While true, movie frames are marked as the last one (stream.c). */
int pe_host_skip_key(void) { return (g_keys & KEY_SKIP) != 0; }

/* F5 pressed since the last call: the field code opens the save screen when
 * the in-game menu could open (src/main/entity/Entity_FrameUpdate.c patch).
 * A request made where that is not possible is dropped. */
int pe_host_take_save_request(void) {
    int r = g_save_request;
    g_save_request = 0;
    return r;
}

/* The in-game menu's Save Game entry (host patch of
 * Menu_StepItemSelectScreen) closes the menu and asks for the save screen,
 * which the field code then opens like F5. */
void pe_host_request_save(void) { g_save_request = 1; }

/* "Save Game" in the game's text encoding (capitals from 0x10, lower case
 * from 0x30, space 0x0F, 0xFF ends it), for the menu's help line. */
static unsigned char g_save_game_text[] = {0x22, 0x30, 0x45, 0x34, 0x0F, 0x16, 0x30, 0x3C, 0x34, 0xFF};
unsigned char *pe_host_save_game_text(void) { return g_save_game_text; }
