/* Data code interpreter test (host only): pc/host/pe_host_datacode.c against
 * the C that tools/pc/gen_data_code.py translates from the same executable.
 * Each entry point runs on many random RAM states, once per side, each in its
 * own child process; RAM, scratchpad, $v0 and every GTE access must match.
 * Needs your SLUS_006.62; the translated C is made from it here only.
 *
 *   python3 tools/pc/gen_data_code.py assets/USA/main.exe $T/ref.c
 *   gcc -m32 -O1 -Ipc/host -include pc/host/pe_host_prelude.h -Dpe_data_code_table=ref_data_code_table \
 *       -Dpe_data_code_count=ref_data_code_count -c $T/ref.c -o $T/ref.o
 *   gcc -m32 -O1 -Ipc/host pc/tests/datacode_test.c pc/host/pe_host_datacode.c $T/ref.o -o $T/datacode_test
 *   $T/datacode_test assets/USA/main.exe [trials]
 */
#define _GNU_SOURCE
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "pe_host_func_table.h"

extern const PeHostFunc ref_data_code_table[];
extern const unsigned int ref_data_code_count;
void pe_host_datacode_take(unsigned *calls, unsigned long long *insns, double *total_ms, double *worst_ms);

#define RAM 0x80000000u
#define RAM_SIZE 0x800000u
#define SCRATCH 0x1F800000u

/* Stubs for what the routines reach in the host. */
uint32_t pe_cop0_status = 0x40000401u;
static uint32_t g_gte_data[32], g_gte_ctrl[32], g_gte_hash;
static void mix(uint32_t v) { g_gte_hash = (g_gte_hash ^ v) * 16777619u; }
unsigned int pe_gte_data_read(unsigned reg) { mix(0x100 + reg); return g_gte_data[reg & 31]; }
void pe_gte_data_write(unsigned reg, unsigned int v) { mix(0x200 + reg); mix(v); g_gte_data[reg & 31] = v; }
unsigned int pe_gte_ctrl_read(unsigned reg) { mix(0x300 + reg); return g_gte_ctrl[reg & 31]; }
void pe_gte_ctrl_write(unsigned reg, unsigned int v) { mix(0x400 + reg); mix(v); g_gte_ctrl[reg & 31] = v; }
void Psyz_GteCommand(unsigned int cmd) {
    int k;
    mix(cmd);
    for (k = 0; k < 32; k++) {
        g_gte_data[k] = g_gte_data[k] * 1103515245u + g_gte_ctrl[k] + cmd;
    }
}
void pe_asm_break(const char *name) { (void)name; _exit(77); }
double pe_host_now_s(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static uint32_t g_rng;
static uint32_t rnd(void) {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}
/* RAM pointers, small numbers and anything, so loads give usable pointers */
static uint32_t value(void) {
    uint32_t k = rnd() % 10;
    if (k < 4) {
        return RAM + (rnd() % 0x200000u & ~3u);
    }
    return k < 7 ? rnd() % 16 : rnd();
}

static unsigned char *g_image;

static void setup(uint32_t seed, uint32_t *args) {
    uint32_t i;
    g_rng = seed * 2654435761u + 1;
    memcpy((void *)(uintptr_t)RAM, g_image, 0x200000);
    for (i = 0; i < 0x200000 / 4; i++) {
        if (rnd() % 4 == 0) {
            ((uint32_t *)(uintptr_t)RAM)[i] = value();
        }
    }
    for (i = 0; i < 0x400 / 4; i++) {
        ((uint32_t *)(uintptr_t)SCRATCH)[i] = value();
    }
    for (i = 0; i < 32; i++) {
        g_gte_data[i] = value();
        g_gte_ctrl[i] = value();
    }
    g_gte_hash = 2166136261u;
    for (i = 0; i < 8; i++) {
        args[i] = value();
    }
}

static uint32_t hash_mem(uint32_t addr, uint32_t len, uint32_t h) {
    const uint32_t *p = (const uint32_t *)(uintptr_t)addr;
    uint32_t i;
    for (i = 0; i < len / 4; i++) {
        h = (h ^ p[i]) * 16777619u;
    }
    return h;
}

typedef uint32_t (*Fn8)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);

/* Result of one run in a child: hash of everything it changed, or a crash. */
static uint64_t run_child(void *fn, uint32_t seed) {
    int p[2];
    uint64_t out = 0;
    pid_t pid;
    int st;
    if (pipe(p) != 0) {
        abort();
    }
    pid = fork();
    if (pid == 0) {
        uint32_t a[8], v, h;
        close(p[0]);
        setup(seed, a);
        alarm(5);
        v = ((Fn8)fn)(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]);
        h = hash_mem(RAM, RAM_SIZE, 2166136261u);
        h = hash_mem(SCRATCH, 0x400, h);
        out = (uint64_t)(h ^ g_gte_hash) << 32 | v;
        if (write(p[1], &out, sizeof(out)) != sizeof(out)) {
            _exit(1);
        }
        _exit(0);
    }
    close(p[1]);
    if (read(p[0], &out, sizeof(out)) != sizeof(out)) {
        out = 0;
    }
    close(p[0]);
    waitpid(pid, &st, 0);
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
        /* crash, break or timeout: the kind must match on both sides */
        out = 0xDEAD000000000000ull | (WIFSIGNALED(st) ? WTERMSIG(st) : 0x100 + WEXITSTATUS(st));
    }
    return out;
}

int main(int argc, char **argv) {
    FILE *f;
    unsigned char hdr[0x800];
    uint32_t t_addr, t_size, trials = argc > 2 ? (uint32_t)atoi(argv[2]) : 300;
    unsigned e, t, fail = 0, clean = 0, crashed = 0;
    if (argc < 2) {
        fprintf(stderr, "usage: %s SLUS_006.62 [trials]\n", argv[0]);
        return 2;
    }
    if (mmap((void *)(uintptr_t)RAM, RAM_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
             -1, 0) != (void *)(uintptr_t)RAM ||
        mmap((void *)(uintptr_t)SCRATCH, 0x1000, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) != (void *)(uintptr_t)SCRATCH) {
        perror("mmap");
        return 2;
    }
    f = fopen(argv[1], "rb");
    if (!f || fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr)) {
        perror(argv[1]);
        return 2;
    }
    memcpy(&t_addr, hdr + 0x18, 4);
    memcpy(&t_size, hdr + 0x1C, 4);
    if (fread((void *)(uintptr_t)t_addr, 1, t_size, f) != t_size) {
        perror(argv[1]);
        return 2;
    }
    fclose(f);
    if (pe_host_datacode_init() != 0) {
        printf("FAIL: init\n");
        return 1;
    }
    g_image = malloc(0x200000);
    memcpy(g_image, (void *)(uintptr_t)RAM, 0x200000);
    if (ref_data_code_count != pe_data_code_count) {
        printf("FAIL: %u entry points, translated C has %u\n", pe_data_code_count, ref_data_code_count);
        return 1;
    }
    for (e = 0; e < pe_data_code_count; e++) {
        unsigned ok = 0, bad = 0;
        if (ref_data_code_table[e].mips_addr != pe_data_code_table[e].mips_addr) {
            printf("FAIL: entry %u at 0x%08X, translated C 0x%08X\n", e, pe_data_code_table[e].mips_addr,
                   ref_data_code_table[e].mips_addr);
            return 1;
        }
        for (t = 0; t < trials; t++) {
            uint32_t seed = e * 1000003u + t;
            uint64_t a = run_child(ref_data_code_table[e].host, seed);
            uint64_t b = run_child(pe_data_code_table[e].host, seed);
            if (a != b) {
                if (bad++ < 3) {
                    printf("FAIL: 0x%08X seed %u: translated %016llX, interpreter %016llX\n",
                           pe_data_code_table[e].mips_addr, seed, (unsigned long long)a, (unsigned long long)b);
                }
                fail++;
            } else if ((a >> 48) == 0xDEAD) {
                crashed++;
            } else {
                ok++;
                clean++;
            }
        }
        printf("0x%08X: %u of %u runs completed and matched, %u differ\n", pe_data_code_table[e].mips_addr, ok,
               trials, bad);
    }
    /* speed: the interpreter on states where the routine completes */
    {
        unsigned calls = 0, c;
        unsigned long long insns = 0, n;
        double ms = 0, worst = 0, m, w, ref_ms = 0, t0;
        pe_host_datacode_take(&c, &n, &m, &w);
        for (e = 0; e < pe_data_code_count; e++) {
            for (t = 0; t < trials; t++) {
                uint32_t seed = e * 1000003u + t, a[8];
                if ((run_child(ref_data_code_table[e].host, seed) >> 48) == 0xDEAD) {
                    continue;
                }
                setup(seed, a);
                t0 = pe_host_now_s();
                ((Fn8)ref_data_code_table[e].host)(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]);
                ref_ms += (pe_host_now_s() - t0) * 1000.0;
                setup(seed, a);
                ((Fn8)pe_data_code_table[e].host)(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]);
                pe_host_datacode_take(&c, &n, &m, &w);
                calls += c;
                insns += n;
                ms += m;
                worst = w > worst ? w : worst;
            }
        }
        if (calls) {
            printf("speed: %u calls, %llu instructions, %.1f ns per instruction, longest call %.3f ms; "
                   "%.1f ms in all, translated C %.1f ms\n",
                   calls, insns, insns ? ms * 1e6 / insns : 0.0, worst, ms, ref_ms);
        }
    }
    printf("%s: %u matched, %u crashed or timed out on both sides alike, %u differ\n", fail ? "FAIL" : "PASS", clean,
           crashed, fail);
    return fail ? 1 : 0;
}
