/* F12 recorder: with every F12 snapshot, the next seconds (PE_RECORD_SECONDS,
 * default 10) are recorded for bug hunting:
 *   record-NNNNN.rgb  what the screen showed, every present, 480x270 (PsyZ
 *                     Psyz_VideoRecordStart; read back without stalling),
 *   record-NNNNN.txt  one line per game frame: time, how long the frame took,
 *                     refreshes waited and whether it was late, the game's own
 *                     time and the time waiting in VSync, the process's CPU
 *                     time and memory, and the widescreen window (slide and
 *                     the picture's ends).
 * NNNNN is the game frame the recording started at. */
#include <stdio.h>
#include <stdlib.h>
#ifdef _WIN32
#define PSAPI_VERSION 2
#include <windows.h>
#include <psapi.h>
#else
#include <sys/resource.h>
#endif

/* PsyZ (psyz/video.h), declared here: psyz.h's PS1 types clash with windows.h */
int Psyz_VideoRecordStart(const char *path, int w, int h);
void Psyz_VideoRecordTag(unsigned frame);
void Psyz_VideoRecordStop(void);
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

int pe_host_recording(void) { return g_txt != NULL; }

void pe_host_record_start(unsigned frame) {
    char path[64];
    const char *e = getenv("PE_RECORD_SECONDS");
    double seconds = e ? atof(e) : 10.0;
    if (g_txt || seconds <= 0.0) {
        return;
    }
    snprintf(path, sizeof(path), "record-%05u.rgb", frame);
    if (!Psyz_VideoRecordStart(path, 480, 270)) {
        fprintf(stderr, "pe: recorder: cannot create %s\n", path);
        return;
    }
    snprintf(path, sizeof(path), "record-%05u.txt", frame);
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
