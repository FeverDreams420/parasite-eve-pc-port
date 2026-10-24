/* F12 recorder: with every F12 snapshot, the next seconds (PE_RECORD_SECONDS,
 * default 10) are recorded for bug hunting:
 *   record-NNNNN.rgb  what the screen showed, every present, 480x270 (PsyZ
 *                     Psyz_VideoRecordStart; read back without stalling),
 *   record-NNNNN.txt  one line per game frame: time, how long the frame took,
 *                     refreshes waited and whether it was late, the game's own
 *                     time and the time waiting in VSync, the process's CPU
 *                     time and memory, and the widescreen window (slide and
 *                     the picture's ends).
 * NNNNN is the game frame the recording started at.
 *
 * Rolling mode (PE_RECORD_BEFORE=n, playtest.bat): the last n seconds are
 * always kept in memory, one picture per game frame, and F12 saves them
 * together with the PE_RECORD_AFTER seconds that follow (default 5), so a
 * glitch already gone when F12 is pressed is still in the file. The files are
 * the same as above (one present per game frame), plus
 *   record-NNNNN-flashes.txt  frames where part of the picture changed for
 *                     that one frame only (different from the frames before
 *                     and after, which agree with each other): flicker and
 *                     one-frame glitches, with how many pixels and where. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define PSAPI_VERSION 2
#include <windows.h>
#include <psapi.h>
#else
#include <sys/resource.h>
#endif

/* PsyZ (psyz/video.h), declared here: psyz.h's PS1 types clash with windows.h */
int Psyz_VideoRecordStart(const char *path, int w, int h);
typedef void (*Psyz_RecordSinkFn)(unsigned present, unsigned tag, float t_ms, const unsigned char *rgb);
int Psyz_VideoRecordStartSink(Psyz_RecordSinkFn fn, int w, int h);
void Psyz_VideoRecordTag(unsigned frame);
void Psyz_VideoRecordStop(void);
const char *pe_host_f12_path(char *buf, size_t size, const char *fmt, ...);
double pe_host_now_s(void);
float pe_host_ws_last_shift(void);
void pe_host_ws_last_picture(float *left, float *right);

static FILE *g_txt;
static double g_t0, g_end, g_last, g_cpu_last;
static unsigned g_start_frame;

static double cpu_s(void) { /* the process's CPU time, all threads */
#ifdef _WIN32
    FILETIME c, e, k, u;
    if (GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) {
        ULARGE_INTEGER a, b;
        a.LowPart = k.dwLowDateTime, a.HighPart = k.dwHighDateTime;
        b.LowPart = u.dwLowDateTime, b.HighPart = u.dwHighDateTime;
        return (double)(a.QuadPart + b.QuadPart) / 1e7;
    }
    return 0.0;
#else
    struct rusage r;
    getrusage(RUSAGE_SELF, &r);
    return r.ru_utime.tv_sec + r.ru_stime.tv_sec + (r.ru_utime.tv_usec + r.ru_stime.tv_usec) / 1e6;
#endif
}

static double ram_mb(void) { /* the process's memory in use (working set / resident) */
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS m;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &m, sizeof(m))) {
        return m.WorkingSetSize / 1048576.0;
    }
    return 0.0;
#else
    long pages = 0, resident = 0;
    FILE *f = fopen("/proc/self/statm", "r");
    if (f) {
        if (fscanf(f, "%ld %ld", &pages, &resident) != 2) {
            resident = 0;
        }
        fclose(f);
    }
    return resident * 4096.0 / 1048576.0;
#endif
}

/* ---- rolling mode ---- */
#define RW 480
#define RH 270
typedef struct {
    unsigned frame, present;
    float t_ms;
    unsigned char *rgb; /* RW*RH*3 */
} RingPic;
typedef struct {
    unsigned frame;
    double t, frame_ms, game_ms, wait_ms, cpu_ms, ram;
    int refreshes, mode, ff;
    float shift, l, r;
} RingLine;
static RingPic *g_pic;
static RingLine *g_line;
static int g_cap, g_pic_n, g_pic_head, g_line_n, g_line_head; /* head = next slot */
static int g_ring = -1;                                          /* -1 not decided */
static double g_save_at, g_ring_last;
static unsigned g_mark;

static void ring_sink(unsigned present, unsigned tag, float t_ms, const unsigned char *rgb) {
    RingPic *p;
    int last = (g_pic_head + g_cap - 1) % g_cap;
    if (g_pic_n > 0 && g_pic[last].frame == tag) {
        p = &g_pic[last]; /* a later present of the same game frame replaces it */
    } else {
        p = &g_pic[g_pic_head];
        if (!p->rgb && !(p->rgb = malloc(RW * RH * 3))) {
            return;
        }
        g_pic_head = (g_pic_head + 1) % g_cap;
        if (g_pic_n < g_cap) {
            g_pic_n++;
        }
    }
    p->frame = tag;
    p->present = present;
    p->t_ms = t_ms;
    memcpy(p->rgb, rgb, RW * RH * 3);
}

static void ring_init(void) {
    const char *b = getenv("PE_RECORD_BEFORE"), *a = getenv("PE_RECORD_AFTER");
    double before = b ? atof(b) : 0.0, after = a ? atof(a) : 5.0;
    g_ring = 0;
    if (before <= 0.0) {
        return;
    }
    if (after < 0.0) {
        after = 0.0;
    }
    g_cap = (int)((before + after) * 30.0) + 8; /* the game runs at 30 frames a second */
    g_pic = calloc(g_cap, sizeof(*g_pic));
    g_line = calloc(g_cap, sizeof(*g_line));
    if (!g_pic || !g_line || !Psyz_VideoRecordStartSink(ring_sink, RW, RH)) {
        fprintf(stderr, "pe: recorder: rolling mode could not start\n");
        free(g_pic), free(g_line);
        g_pic = NULL, g_line = NULL;
        return;
    }
    g_ring = 1;
    g_ring_last = pe_host_now_s();
    fprintf(stderr, "pe: recorder: keeping the last %.0f s; F12 saves them and the next %.0f s\n", before, after);
}

static unsigned char lum(const unsigned char *p) { return (unsigned char)((p[0] + 2 * p[1] + p[2]) / 4); }

/* Frames where pixels differ from both neighbours by more than 40 levels of
 * brightness while the neighbours agree within 12: something shown for one
 * frame only. Lines: frame, pixels, box (x0 y0 x1 y1, 480x270 picture),
 * darker or lighter. */
static void ring_flashes(const char *path, int first, int n) {
    FILE *f = fopen(path, "w");
    int i, found = 0;
    if (!f) {
        return;
    }
    fprintf(f, "# frame pixels x0 y0 x1 y1 darker|lighter (one-frame changes, 480x270 picture)\n");
    for (i = 1; i + 1 < n; i++) {
        const RingPic *a = &g_pic[(first + i - 1) % g_cap], *b = &g_pic[(first + i) % g_cap],
                      *c = &g_pic[(first + i + 1) % g_cap];
        int k, cnt = 0, dark = 0, x0 = RW, y0 = RH, x1 = -1, y1 = -1;
        if (b->frame != a->frame + 1 || c->frame != b->frame + 1) {
            continue; /* only between consecutive game frames */
        }
        for (k = 0; k < RW * RH; k++) {
            int la = lum(a->rgb + k * 3), lb = lum(b->rgb + k * 3), lc = lum(c->rgb + k * 3);
            int mid = (la + lc) / 2;
            if (abs(la - lc) <= 12 && abs(lb - mid) > 40) {
                int x = k % RW, y = k / RW;
                cnt++;
                dark += lb < mid;
                x0 = x < x0 ? x : x0, x1 = x > x1 ? x : x1;
                y0 = y < y0 ? y : y0, y1 = y > y1 ? y : y1;
            }
        }
        if (cnt >= 40) {
            fprintf(f, "%u %d %d %d %d %d %s\n", b->frame, cnt, x0, y0, x1, y1, dark * 2 >= cnt ? "darker" : "lighter");
            found++;
        }
    }
    fclose(f);
    fprintf(stderr, "pe: recorder: %d frames with one-frame changes (%s)\n", found, path);
}

static void ring_save(void) {
    char path[128];
    int i, first, n = g_pic_n;
    FILE *f;
    if (n == 0) {
        return;
    }
    first = (g_pic_head + g_cap - n) % g_cap;
    pe_host_f12_path(path, sizeof(path), "record-%05u.rgb", g_pic[first].frame);
    if ((f = fopen(path, "wb")) != NULL) {
        for (i = 0; i < n; i++) {
            const RingPic *p = &g_pic[(first + i) % g_cap];
            const unsigned hdr[2] = {p->present, p->frame}, zero = 0;
            float t = p->t_ms - g_pic[first].t_ms;
            fwrite(hdr, 4, 2, f);
            fwrite(&t, 4, 1, f);
            fwrite(&zero, 4, 1, f);
            fwrite(p->rgb, 3, RW * RH, f);
        }
        fclose(f);
    }
    pe_host_f12_path(path, sizeof(path), "record-%05u.txt", g_pic[first].frame);
    if ((f = fopen(path, "w")) != NULL && g_line_n > 0) {
        int lf = (g_line_head + g_cap - g_line_n) % g_cap;
        fprintf(f, "# frame t_ms frame_ms refreshes mode late game_ms wait_ms fast_forward cpu_ms ram_mb "
                   "ws_shift ws_left ws_right\n");
        for (i = 0; i < g_line_n; i++) {
            const RingLine *L = &g_line[(lf + i) % g_cap];
            fprintf(f, "%u %.1f %.1f %d %d %s %.1f %.1f %d %.1f %.0f %.0f %.0f %.0f\n", L->frame,
                    (L->t - g_line[lf].t) * 1000.0, L->frame_ms, L->refreshes, L->mode,
                    L->mode >= 2 && L->refreshes > L->mode ? "LATE" : "-", L->game_ms, L->wait_ms, L->ff,
                    L->cpu_ms, L->ram, L->shift, L->l, L->r);
        }
    }
    if (f) {
        fclose(f);
    }
    pe_host_f12_path(path, sizeof(path), "record-%05u-flashes.txt", g_pic[first].frame);
    ring_flashes(path, first, n);
    fprintf(stderr, "pe: recording around frame %u saved (record-%05u.rgb/.txt, frames %u..%u)\n", g_mark,
            g_pic[first].frame, g_pic[first].frame, g_pic[(first + n - 1) % g_cap].frame);
}

int pe_host_recording(void) { return g_txt != NULL || g_save_at > 0.0; }

void pe_host_record_start(unsigned frame) {
    char path[128];
    const char *e = getenv("PE_RECORD_SECONDS");
    double seconds = e ? atof(e) : 10.0;
    if (g_ring < 0) {
        ring_init();
    }
    if (g_ring == 1) {
        const char *a = getenv("PE_RECORD_AFTER");
        if (g_save_at <= 0.0) {
            g_mark = frame;
            g_save_at = pe_host_now_s() + (a ? atof(a) : 5.0);
            fprintf(stderr, "pe: recording around frame %u: saving the last seconds and the next ones\n", frame);
        }
        return;
    }
    if (g_txt || seconds <= 0.0) {
        return;
    }
    pe_host_f12_path(path, sizeof(path), "record-%05u.rgb", frame);
    if (!Psyz_VideoRecordStart(path, 480, 270)) {
        fprintf(stderr, "pe: recorder: cannot create %s\n", path);
        return;
    }
    pe_host_f12_path(path, sizeof(path), "record-%05u.txt", frame);
    g_txt = fopen(path, "w");
    if (!g_txt) {
        Psyz_VideoRecordStop();
        return;
    }
    fprintf(g_txt, "# frame t_ms frame_ms refreshes mode late game_ms wait_ms fast_forward cpu_ms ram_mb "
                   "ws_shift ws_left ws_right\n");
    g_start_frame = frame;
    g_t0 = g_last = pe_host_now_s();
    g_end = g_t0 + seconds;
    g_cpu_last = cpu_s();
    fprintf(stderr, "pe: recording %.0f s from frame %u (record-%05u.rgb/.txt)\n", seconds, frame, frame);
}

/* Once per game frame, as it leaves VSync. */
void pe_host_record_frame(unsigned frame, int refreshes, int mode, double game_s, double wait_s, int ff) {
    double t, cpu;
    float l = 0.0f, r = 0.0f;
    if (g_ring < 0) {
        ring_init();
    }
    if (g_ring == 1) {
        RingLine *L = &g_line[g_line_head];
        t = pe_host_now_s();
        cpu = cpu_s();
        pe_host_ws_last_picture(&l, &r);
        L->frame = frame, L->t = t, L->frame_ms = (t - g_ring_last) * 1000.0;
        L->refreshes = refreshes, L->mode = mode, L->ff = ff;
        L->game_ms = game_s * 1000.0, L->wait_ms = wait_s * 1000.0;
        L->cpu_ms = (cpu - g_cpu_last) * 1000.0, L->ram = ram_mb();
        L->shift = pe_host_ws_last_shift(), L->l = l, L->r = r;
        g_line_head = (g_line_head + 1) % g_cap;
        if (g_line_n < g_cap) {
            g_line_n++;
        }
        g_ring_last = t;
        g_cpu_last = cpu;
        Psyz_VideoRecordTag(frame + 1);
        if (g_save_at > 0.0 && t >= g_save_at) {
            ring_save();
            g_save_at = 0.0;
        }
        return;
    }
    if (!g_txt) {
        return;
    }
    t = pe_host_now_s();
    cpu = cpu_s();
    pe_host_ws_last_picture(&l, &r);
    fprintf(g_txt, "%u %.1f %.1f %d %d %s %.1f %.1f %d %.1f %.0f %.0f %.0f %.0f\n", frame, (t - g_t0) * 1000.0,
            (t - g_last) * 1000.0, refreshes, mode, mode >= 2 && refreshes > mode ? "LATE" : "-", game_s * 1000.0,
            wait_s * 1000.0, ff, (cpu - g_cpu_last) * 1000.0, ram_mb(), pe_host_ws_last_shift(), l, r);
    g_last = t;
    g_cpu_last = cpu;
    Psyz_VideoRecordTag(frame + 1);
    if (t >= g_end) {
        Psyz_VideoRecordStop();
        fclose(g_txt);
        g_txt = NULL;
        fprintf(stderr, "pe: recording from frame %u done\n", g_start_frame);
    }
}
