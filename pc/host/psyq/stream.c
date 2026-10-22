/* CD streaming for movies (PSY-Q libcd St* ring + libds DsRead2).
 *
 * The movie player starts a stream with DsRead2(position, mode) and pulls
 * frames with StGetNext(&data, &header). A movie file (STR) interleaves
 * video sectors - each 2048-byte data sector is a 32-byte header
 *   +0 u16 0x0160  +2 u16 type (0x8001 video)  +4 u16 sector index in frame
 *   +6 u16 sectors in frame  +8 u32 frame number  +C u32 frame data size
 *   +10 u16 width  +12 u16 height
 * followed by 2016 bytes of the frame's data - with XA audio sectors
 * (sub-header submode bit 2). This reads the video sectors of the next frame
 * from your disc image, joins their 2016-byte payloads into the ring buffer
 * the game gave StSetRing, and returns the first sector's header (the game
 * reads width/height/frame number from it, see menu_memcard_video.h).
 *
 * Timing: a real drive delivers 150 sectors per second at double speed
 * (75 at single speed). A frame is handed out only once the virtual drive
 * would have read its last sector, so movies run at their recorded rate.
 *
 * Movie audio (XA sectors) is not played yet.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pe_host_disc.h"

int pe_host_skip_key(void);

#ifdef _WIN32
#include <windows.h>
static double now_s(void) {
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
}
static void sleep_s(double s) { Sleep((DWORD)(s * 1000.0)); }
#else
#include <time.h>
static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}
static void sleep_s(double s) {
    struct timespec ts;
    ts.tv_sec = (time_t)s;
    ts.tv_nsec = (long)((s - (double)ts.tv_sec) * 1e9);
    nanosleep(&ts, NULL);
}
#endif

typedef struct {
    unsigned char minute, second, sector, track;
} CdLoc;

static int bcd2i(int v) { return (v >> 4) * 10 + (v & 15); }
static int loc_to_lba(const CdLoc *p) {
    return (bcd2i(p->minute) * 60 + bcd2i(p->second)) * 75 + bcd2i(p->sector) - 150;
}
static int i2bcd(int v) { return ((v / 10) << 4) | (v % 10); }
static void lba_to_loc(int lba, CdLoc *p) {
    lba += 150;
    p->minute = (unsigned char)i2bcd(lba / 4500);
    p->second = (unsigned char)i2bcd((lba / 75) % 60);
    p->sector = (unsigned char)i2bcd(lba % 75);
    p->track = 0;
}

static uint8_t *g_ring;
static uint32_t g_ring_sectors;
static int g_lba;          /* next sector to read */
static int g_start_lba;    /* where the stream started */
static double g_start_time;
static double g_rate = 150.0;
static int g_active;
static int g_slot;
static int g_end_frame = -1;

/* StSetRing(ring, sectors) */
void pe_psyq_StSetRing(uint32_t *ring, uint32_t sectors) {
    g_ring = (uint8_t *)ring;
    g_ring_sectors = sectors;
    g_slot = 0;
}

void pe_psyq_StUnSetRing(void) {
    g_ring = NULL;
    g_active = 0;
}

/* StSetStream(mode, start_frame, end_frame, func1, func2) */
void pe_psyq_StSetStream(uint32_t mode, uint32_t start, uint32_t end, void *f1, void *f2) {
    (void)mode;
    (void)start;
    (void)f1;
    (void)f2;
    g_end_frame = (int)end;
}

/* DsRead2(position, mode): start streaming. Returns non-zero when started. */
int pe_psyq_DsRead2(CdLoc *pos, int mode) {
    g_lba = pos ? loc_to_lba(pos) : g_lba;
    g_start_lba = g_lba;
    g_start_time = now_s();
    g_rate = (mode & 0x80) ? 150.0 : 75.0;
    g_active = 1;
    if (getenv("PE_LOG_CD")) {
        fprintf(stderr, "pe: DsRead2 stream from sector %d (mode 0x%X)\n", g_lba, mode);
    }
    return 1;
}

static void wait_for_sector(int lba) {
    double due = g_start_time + (double)(lba - g_start_lba + 1) / g_rate;
    double t = now_s();
    if (due > t) {
        sleep_s(due - t);
    }
}

/* StGetNext(&data, &header): 0 when a frame is ready, 1 when not. */
int pe_psyq_StGetNext(uint32_t **data, uint32_t **header) {
    uint8_t raw[2352];
    uint8_t *slot;
    uint32_t slot_bytes, have = 0, frame = 0, count = 0, got = 0;
    int guard = 0;
    if (!g_active || !g_ring || g_ring_sectors < 4) {
        return 1;
    }
    slot_bytes = (g_ring_sectors / 2) * 2048;
    slot = g_ring + (g_slot ? slot_bytes : 0);
    while (guard++ < 4096) {
        uint8_t *ud = raw + 24;
        uint16_t magic, type, idx, cnt;
        uint32_t fn;
        if (pe_host_disc_read_raw(g_lba, raw) != 0) {
            g_active = 0;
            return 1;
        }
        g_lba++;
        if (raw[18] & 0x04) {
            continue; /* XA audio sector */
        }
        magic = (uint16_t)(ud[0] | ud[1] << 8);
        type = (uint16_t)(ud[2] | ud[3] << 8);
        if (magic != 0x0160 || (type & 0x8000) == 0) {
            continue; /* not a movie video sector */
        }
        idx = (uint16_t)(ud[4] | ud[5] << 8);
        cnt = (uint16_t)(ud[6] | ud[7] << 8);
        fn = (uint32_t)(ud[8] | ud[9] << 8 | ud[10] << 16 | (uint32_t)ud[11] << 24);
        if (got == 0) {
            if (idx != 0) {
                continue; /* resync to the start of a frame */
            }
            frame = fn;
            count = cnt;
            memcpy(slot, ud, 32); /* header */
        } else if (fn != frame) {
            /* frame ended early: restart on this sector */
            g_lba--;
            break;
        }
        if (32 + (uint32_t)(idx + 1) * 2016 <= slot_bytes) {
            memcpy(slot + 32 + idx * 2016, ud + 32, 2016);
            have += 2016;
        }
        got++;
        if (got >= count) {
            break;
        }
    }
    if (got == 0) {
        return 1;
    }
    if (pe_host_skip_key()) {
        /* Skip hotkey (hotkeys.c): every movie player stops once a frame's
         * number reaches the movie's last frame (Memcard_UpdateVideoFrame:
         * time >= entry->end, a 16-bit value), so this frame claims to be
         * the last one and the movie ends the normal way. */
        slot[8] = 0xFF;
        slot[9] = 0x7F;
        slot[10] = 0;
        slot[11] = 0;
    } else {
        wait_for_sector(g_lba - 1);
    }
    g_slot ^= 1;
    *header = (uint32_t *)slot;
    *data = (uint32_t *)(slot + 32);
    return 0;
}

/* Is a movie stream running (for the skip hotkey, hotkeys.c)? */
int pe_host_stream_active(void) { return g_active && g_ring != NULL; }

int pe_psyq_StFreeRing(uint32_t *base) {
    (void)base;
    return 0;
}

/* StGetBackloc(loc): where to resume after an error. */
int pe_psyq_StGetBackloc(CdLoc *loc) {
    lba_to_loc(g_lba, loc);
    return 0;
}

void pe_psyq_StCdInterrupt(void) {}

/* DsControl(CdlSetloc) points the next DsRead2 at a position. */
void pe_host_stream_setloc(const unsigned char *param) {
    if (param) {
        g_lba = loc_to_lba((const CdLoc *)param);
    }
}

/* ---------------------------------------------------------------- DMA */

typedef void (*DmaCallback)(void);
static DmaCallback g_dma_cb[7];

DmaCallback pe_host_dma_callback(int channel) {
    return (channel >= 0 && channel < 7) ? g_dma_cb[channel] : NULL;
}

/* DMACallback(channel, func): returns the previous one. On the host only the
 * MDEC channels (0 in, 1 out) complete through software (mdec.c). */
DmaCallback pe_psyq_DMACallback(int channel, DmaCallback f) {
    DmaCallback old = NULL;
    if (channel >= 0 && channel < 7) {
        old = g_dma_cb[channel];
        g_dma_cb[channel] = f;
    }
    return old;
}
