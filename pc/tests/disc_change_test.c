/* Disc change test (host only, no game code): two small test discs made by
 * make_test_disc.py, opened by Disc 1's name, switched by DsStatus the way
 * the game's disc screen polls it.
 *
 *   python3 pc/tests/make_test_disc.py "$T/t (Disc 1).bin" PE.IMG=@65536 FMV1/PEDISC01.IDF=@2048
 *   python3 pc/tests/make_test_disc.py "$T/t (Disc 2).bin" PE.IMG=@65536 FMV2/PEDISC02.IDF=@2048
 *   (a .cue with FILE "t (Disc N).bin" BINARY next to each; Disc 2 may be split into .partN)
 *   gcc -Ipc/host pc/tests/disc_change_test.c pc/host/pe_host_disc.c pc/host/psyq/ds.c -o disc_change_test
 *   ./disc_change_test "$T/t (Disc 1).cue"
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

#include "pe_host_disc.h"

typedef struct {
    unsigned char minute, second, sector, track;
} DslLOC;
typedef struct {
    DslLOC pos;
    unsigned int size;
    char name[16];
} DslFILE;

int pe_psyq_DsStatus(void);
int pe_psyq_DsShellOpen(void);
DslFILE *pe_psyq_DsSearchFile(DslFILE *fp, char *path);

/* Stubs for what ds.c calls elsewhere in the host. */
void pe_host_overlays_note_read(unsigned int addr, unsigned int len, int disc_lba) {
    (void)addr, (void)len, (void)disc_lba;
}
int CD_vol(void *vol) { (void)vol; return 0; }
void pe_host_stream_setloc(const unsigned char *param) { (void)param; }
void pe_host_stream_control(unsigned char com, const unsigned char *param) { (void)com, (void)param; }

static int g_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); g_fail = 1; } } while (0)

static int found(const char *path) {
    DslFILE f;
    char p[64];
    DslFILE *r;
    snprintf(p, sizeof(p), "%s", path);
    r = pe_psyq_DsSearchFile(&f, p);
    return r != NULL && r != (DslFILE *)-1;
}

int main(int argc, char **argv) {
    volatile unsigned char *mount;
    int shell;
    void *m = mmap((void *)0x800B0000u, 0x10000, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (m == MAP_FAILED || argc < 2) {
        printf("usage: disc_change_test <disc 1 .cue>\n");
        return 2;
    }
    mount = (volatile unsigned char *)(uintptr_t)0x800B0DCDu;

    CHECK(pe_host_disc_open(argv[1]) == 0);
    CHECK(pe_host_disc_available(0));
    CHECK(pe_host_disc_available(1));
    CHECK(pe_host_disc_current() == 0);
    CHECK(found("\\FMV1\\PEDISC01.IDF;1"));
    CHECK(!found("\\FMV2\\PEDISC02.IDF;1"));

    /* No disc recorded yet (boot): nothing changes. */
    *mount = 0;
    shell = pe_psyq_DsShellOpen();
    CHECK(pe_psyq_DsStatus() == 0x02);
    CHECK(pe_host_disc_current() == 0 && pe_psyq_DsShellOpen() == shell);

    /* Disc 1 recorded: the screen wants Disc 2. Lid opens once. */
    *mount = 1;
    CHECK(pe_psyq_DsStatus() == 0x12);
    CHECK(pe_host_disc_current() == 1);
    CHECK(pe_psyq_DsShellOpen() == shell + 1);
    CHECK(found("\\FMV2\\PEDISC02.IDF;1"));
    CHECK(!found("\\FMV1\\PEDISC01.IDF;1"));
    CHECK(found("\\PE.IMG;1"));
    CHECK(pe_psyq_DsStatus() == 0x02); /* still flags 1, Disc 2 in: no second swap */

    /* The game found Disc 2 (flags 2); a later disc screen wants Disc 1. */
    *mount = 2;
    CHECK(pe_psyq_DsStatus() == 0x12);
    CHECK(pe_host_disc_current() == 0);
    CHECK(pe_psyq_DsShellOpen() == shell + 2);
    CHECK(found("\\FMV1\\PEDISC01.IDF;1"));

    printf(g_fail ? "disc change test: FAILED\n" : "disc change test: ok\n");
    return g_fail;
}
