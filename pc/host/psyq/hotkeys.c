/* Test hotkeys.
 *
 *   ` / ~ (the key under Escape)  skip the movie that is playing: while it
 *        is held, the next movie frame handed to the game is marked as the
 *        movie's last frame (stream.c), so the game ends the movie the normal
 *        way. Only the opening movies' player has a skip button of its own,
 *        so this works for the story movies too.
 *   Tab  fast forward while held: the frame waits are skipped (one refresh
 *        step per game frame, no clock waits) and PsyZ's frame limiter and
 *        driver VSync are switched off; they are restored on release.
 *        Movies keep their own pace (CD streaming runs on the clock).
 *
 * Keys are read from SDL's keyboard state, so only while the game window has
 * focus. Scan codes, not characters: 53 is the key top-left under Escape on
 * any layout, 43 is Tab (SDL_scancode.h).
 *
 * For tests without a keyboard, PE_KEY_SCRIPT="first-last:mask,..." holds
 * keys for game frames first..last (mask 1 = skip movie, 2 = fast forward).
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
    }
    return m;
}

static unsigned int g_keys;
static int g_fast;
static PsyzVsyncMode g_saved_mode;

/* Once per game frame, from VSync. */
void pe_host_hotkeys_frame(void) {
    int fast;
    g_keys = keys_now();
    fast = (g_keys & KEY_FAST) != 0;
    if (fast != g_fast) {
        if (fast) {
            g_saved_mode = Psyz_VideoGetVsyncMode();
            Psyz_VideoSetVsyncMode(PSYZ_VSYNC_LIMITLESS);
        } else {
            Psyz_VideoSetVsyncMode(g_saved_mode);
        }
        g_fast = fast;
        fprintf(stderr, "pe: fast forward %s\n", fast ? "on" : "off");
    }
}

int pe_host_fast_forward(void) { return g_fast; }

/* While true, movie frames are marked as the last one (stream.c). */
int pe_host_skip_key(void) { return (g_keys & KEY_SKIP) != 0; }
