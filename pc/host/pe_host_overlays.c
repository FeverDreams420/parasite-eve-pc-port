/* Recognising loaded overlays and switching their host code in.
 *
 * At start-up every overlay is read from Disc 1's PE.IMG at the place its
 * config records and checked against its SHA-1; the first 64 bytes are kept
 * as a fingerprint. After each CD read into RAM, the overlays whose window
 * the read touched are compared with their fingerprint, and a candidate whose
 * whole window now has the right SHA-1 gets its jumps written: from then on
 * a call to one of its retail addresses runs the host function compiled from
 * the same C.
 */
#include "pe_host_overlays.h"
#include "pe_host_disc.h"
#include "pe_host_mem.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void pe_host_sha1(const void *data, unsigned int len, unsigned char out[20]);

#define FP_LEN 64

typedef struct {
    int verified;
    unsigned char fp[FP_LEN];
} OverlayState;

static OverlayState *g_state;
static int g_current[8]; /* last installed overlay index per window, -1 none */
static unsigned int g_window_vram[8];
static int g_windows;

static int window_slot(unsigned int vram) {
    int i;
    for (i = 0; i < g_windows; i++) {
        if (g_window_vram[i] == vram) {
            return i;
        }
    }
    if (g_windows < 8) {
        g_window_vram[g_windows] = vram;
        g_current[g_windows] = -1;
        return g_windows++;
    }
    return -1;
}

void pe_host_overlays_init(void) {
    unsigned int lba, size, i, ok = 0, bad = 0;
    g_state = (OverlayState *)calloc(pe_overlay_count, sizeof(OverlayState));
    if (pe_host_disc_find_root_file("PE.IMG", &lba, &size) != 0) {
        fprintf(stderr, "pe: PE.IMG not found; overlays will not be recognised\n");
        return;
    }
    for (i = 0; i < pe_overlay_count; i++) {
        const PeHostOverlay *o = &pe_overlay_table[i];
        unsigned int first, last, n;
        unsigned char *buf, h[20];
        if (o->pe_img_offset < 0 || o->size == 0) {
            continue;
        }
        first = (unsigned int)o->pe_img_offset / 2048;
        last = ((unsigned int)o->pe_img_offset + o->size + 2047) / 2048;
        n = last - first;
        buf = (unsigned char *)malloc(n * 2048);
        if (!buf || pe_host_disc_read((int)(lba + first), buf, (int)n) != 0) {
            free(buf);
            continue;
        }
        pe_host_sha1(buf + (o->pe_img_offset % 2048), o->size, h);
        if (memcmp(h, o->sha1, 20) == 0) {
            g_state[i].verified = 1;
            memcpy(g_state[i].fp, buf + (o->pe_img_offset % 2048), FP_LEN);
            ok++;
        } else {
            bad++;
        }
        free(buf);
    }
    fprintf(stderr, "pe: %u overlays verified against the disc, %u differ\n", ok, bad);
}

static void install(unsigned int i) {
    const PeHostOverlay *o = &pe_overlay_table[i];
    int slot = window_slot(o->vram);
    if (slot >= 0 && g_current[slot] == (int)i) {
        /* Same overlay reloaded: the read overwrote the jumps, write again. */
    }
    pe_host_install_jumps(o->funcs, o->func_count);
    if (slot >= 0) {
        g_current[slot] = (int)i;
    }
    if (getenv("PE_LOG_OVERLAYS")) {
        fprintf(stderr, "pe: overlay %s loaded at 0x%08X (%u functions)\n", o->name, o->vram,
                o->func_count);
    }
}

void pe_host_overlays_note_write(unsigned int addr, unsigned int len) {
    unsigned int i, end = addr + len;
    if (!g_state) {
        return;
    }
    for (i = 0; i < pe_overlay_count; i++) {
        const PeHostOverlay *o = &pe_overlay_table[i];
        unsigned char h[20];
        if (!g_state[i].verified || !o->func_count) {
            continue;
        }
        if (end <= o->vram || addr >= o->vram + o->size) {
            continue;
        }
        if (memcmp((const void *)(uintptr_t)o->vram, g_state[i].fp, FP_LEN) != 0) {
            continue;
        }
        pe_host_sha1((const void *)(uintptr_t)o->vram, o->size, h);
        if (memcmp(h, o->sha1, 20) == 0) {
            install(i);
        }
    }
}
