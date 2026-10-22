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

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void pe_host_sha1(const void *data, unsigned int len, unsigned char out[20]);

#define FP_LEN 64

typedef struct {
    int verified;
    unsigned char fp[FP_LEN];
    unsigned char *image; /* fragmented overlays: the verified binary */
} OverlayState;

static OverlayState *g_state;
static unsigned int g_pe_img_lba, g_pe_img_size; /* where PE.IMG is on the disc */
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
    g_pe_img_lba = lba;
    g_pe_img_size = size;
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
            if (o->fragmented) {
                g_state[i].image = (unsigned char *)malloc(o->size);
                memcpy(g_state[i].image, buf + (o->pe_img_offset % 2048), o->size);
            }
            ok++;
        } else {
            bad++;
        }
        free(buf);
    }
    fprintf(stderr, "pe: %u overlays verified against the disc, %u differ\n", ok, bad);
}

/* For crash reports: the overlay installed in each load window, and the last
 * recognitions (linear overlays and fragmented-overlay function batches). */
static char g_recent[8][96];
static unsigned int g_recent_n;
static void note_recent(const char *what, const char *name, unsigned int a, unsigned int b) {
    snprintf(g_recent[g_recent_n % 8], sizeof(g_recent[0]), "%s %s 0x%08X..0x%08X", what, name, a, b);
    g_recent_n++;
}

void pe_host_overlays_report(void) {
    int i;
    unsigned int k;
    for (i = 0; i < g_windows; i++) {
        fprintf(stderr, "pe:   load window 0x%08X: %s\n", g_window_vram[i],
                g_current[i] >= 0 ? pe_overlay_table[g_current[i]].name : "(no overlay recognised)");
    }
    for (k = g_recent_n > 8 ? g_recent_n - 8 : 0; k < g_recent_n; k++) {
        fprintf(stderr, "pe:   recognised: %s\n", g_recent[k % 8]);
    }
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
    note_recent("overlay", o->name, o->vram, o->vram + o->size);
    if (getenv("PE_LOG_OVERLAYS")) {
        fprintf(stderr, "pe: overlay %s loaded at 0x%08X (%u functions)\n", o->name, o->vram,
                o->func_count);
    }
}

void pe_host_overlays_note_write(unsigned int addr, unsigned int len) {
    pe_host_overlays_note_read(addr, len, -1);
}

void pe_host_overlays_note_read(unsigned int addr, unsigned int len, int disc_lba) {
    unsigned int i, end = addr + len;
    long long src_off = -1; /* byte offset in PE.IMG the data came from */
    if (disc_lba >= 0 && g_pe_img_size && (unsigned int)disc_lba >= g_pe_img_lba &&
        (unsigned long long)((unsigned int)disc_lba - g_pe_img_lba) * 2048u < g_pe_img_size) {
        src_off = (long long)((unsigned int)disc_lba - g_pe_img_lba) * 2048;
    }
    if (!g_state) {
        return;
    }
    for (i = 0; i < pe_overlay_count; i++) {
        const PeHostOverlay *o = &pe_overlay_table[i];
        unsigned char h[20];
        if (!g_state[i].verified || !o->func_count) {
            continue;
        }
        if (o->fragmented) {
            /* Pieces of this overlay are loaded or copied separately to
             * unrelated addresses: recognise each function by its own bytes. */
            /* Their link addresses do not always match where the game puts
             * them (menu_memcard's fade handler is linked at 0x80192F9C but
             * the game's own read puts it at 0x80192FE8). When the read came
             * from PE.IMG, the disc position says exactly which bytes of the
             * overlay landed where; a function is switched in at that address
             * when its bytes there are the verified ones. Otherwise only the
             * link address is checked. */
            unsigned int k, n = 0;
            for (k = 0; k < o->func_count; k++) {
                unsigned int fa = o->funcs[k].mips_addr, sz = o->meta[k].size;
                const unsigned char *want = g_state[i].image + o->meta[k].file_off;
                if (sz < 5 || o->meta[k].file_off + sz > o->size) {
                    continue;
                }
                if (src_off >= 0) {
                    long long at = (long long)o->pe_img_offset + o->meta[k].file_off - src_off;
                    if (at >= 0 && at + sz <= len) {
                        unsigned int ra = addr + (unsigned int)at;
                        if (memcmp((const void *)(uintptr_t)ra, want, sz) == 0) {
                            pe_host_write_jump(ra, o->funcs[k].host);
                            n++;
                            if (ra != fa && getenv("PE_LOG_OVERLAYS")) {
                                fprintf(stderr, "pe: %s function linked at 0x%08X loaded at 0x%08X\n",
                                        o->name, fa, ra);
                            }
                        }
                        continue;
                    }
                }
                if (fa >= addr && fa + sz <= end && memcmp((const void *)(uintptr_t)fa, want, sz) == 0) {
                    pe_host_write_jump(fa, o->funcs[k].host);
                    n++;
                }
            }
            if (n) {
                note_recent("functions of", o->name, addr, end);
            }
            if (n && getenv("PE_LOG_OVERLAYS")) {
                fprintf(stderr, "pe: %u functions of %s recognised at 0x%08X..0x%08X\n", n, o->name,
                        addr, end);
            }
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
