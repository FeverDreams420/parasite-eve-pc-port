/* PSY-Q libds (the CD command queue the game uses) on the host.
 *
 * The disc is your own CUE/BIN image. Reads complete at once: DsRead copies
 * the sectors before it returns, so DsReadSync always reports 0 sectors
 * left and the command queue is always empty.
 *
 * Return conventions follow the retail library (src/main/psyq/libds):
 *   DsRead          -> command id (non-zero) when started, 0 when refused
 *   DsReadSync      -> sectors remaining, 0 when done, -1 on timeout/error
 *   DsSystemStatus  -> 1 ready, 2 busy, 3 error
 *   DsGetDiskType   -> 4 PlayStation disc ("CD001" at sector 16 offset 1),
 *                      2 other disc, 1 error, 16 shell open
 *   DsShellOpen     -> the library's shell-open counter (g_CdDiscType);
 *                      it changes when the lid is opened (disc swap)
 *   DsSearchFile    -> the DslFILE on success, 0 not found, -1 disc error
 *   DsSync/DsCommand-> command completes immediately (DslComplete = 2)
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "pe_host_disc.h"
#include "pe_host_overlays.h"

typedef struct {
    unsigned char minute, second, sector, track;
} DslLOC;

typedef struct {
    DslLOC pos;
    unsigned int size;
    char name[16];
} DslFILE;

typedef char dslfile_size_check[sizeof(DslFILE) == 24 ? 1 : -1];

#define DSL_COMPLETE 2
#define DSL_STAT_STANDBY 0x02

static int g_shell_count;
static int g_last_lba;
static unsigned char g_last_result[8];

static int bcd(int v) { return ((v / 10) << 4) | (v % 10); }
static int unbcd(int v) { return (v >> 4) * 10 + (v & 15); }

DslLOC *pe_psyq_DsIntToPos(int i, DslLOC *p) {
    i += 150;
    p->minute = (unsigned char)bcd(i / 4500);
    p->second = (unsigned char)bcd((i / 75) % 60);
    p->sector = (unsigned char)bcd(i % 75);
    return p;
}

int pe_psyq_DsPosToInt(DslLOC *p) {
    return (unbcd(p->minute) * 60 + unbcd(p->second)) * 75 + unbcd(p->sector) - 150;
}

int pe_psyq_DsInit(void) { return 1; }
int pe_psyq_DsReset(void) { return 1; }
void pe_psyq_DsSetDebug(int level) { (void)level; }
int pe_psyq_DsSystemStatus(void) { return pe_host_disc_ok() ? 1 : 3; }
int pe_psyq_DsQueueLen(void) { return 0; }
int pe_psyq_DsStatus(void) { return DSL_STAT_STANDBY; }
int pe_psyq_DsShellOpen(void) { return g_shell_count; }
void pe_host_disc_shell_cycled(void) { g_shell_count++; }
int pe_psyq_DsMix(void *vol) { (void)vol; return 1; }
int pe_psyq_CdMix(void *vol) { (void)vol; return 1; }

int pe_psyq_DsGetDiskType(void) {
    unsigned char s[2048];
    if (pe_host_disc_read(16, s, 1) != 0) {
        return 1;
    }
    return memcmp(s + 1, "CD001", 5) == 0 ? 4 : 2;
}

static void set_result(unsigned char *result) {
    memset(g_last_result, 0, sizeof(g_last_result));
    g_last_result[0] = DSL_STAT_STANDBY;
    if (result) {
        memcpy(result, g_last_result, sizeof(g_last_result));
    }
}

static int g_next_id = 1;

static int next_id(void) {
    int id = g_next_id++;
    if (g_next_id > 0x7FFF) {
        g_next_id = 1;
    }
    return id;
}

int pe_psyq_DsRead(DslLOC *pos, int sectors, void *buf, int mode) {
    int lba = pos ? pe_psyq_DsPosToInt(pos) : g_last_lba;
    (void)mode; /* the game reads 2048-byte data sectors (mode | 0x20) */
    if (getenv("PE_LOG_CD")) {
        fprintf(stderr, "pe: DsRead sector %d x%d -> 0x%08X (mode 0x%X)\n", lba, sectors,
                (unsigned int)(uintptr_t)buf, mode);
    }
    if (sectors <= 0) {
        return 0;
    }
    if (pe_host_disc_read(lba, buf, sectors) != 0) {
        fprintf(stderr, "pe: CD read failed at sector %d (+%d)\n", lba, sectors);
        return 0;
    }
    g_last_lba = lba + sectors;
    pe_host_overlays_note_write((unsigned int)(uintptr_t)buf, (unsigned int)sectors * 2048u);
    return next_id();
}

int pe_psyq_DsReadSync(unsigned char *result) {
    set_result(result);
    return 0;
}

void pe_psyq_DsReadBreak(void) {}

int pe_psyq_DsCommand(int com, unsigned char *param, void *cb, int count) {
    (void)param;
    (void)count;
    if (cb) {
        fprintf(stderr, "pe: DsCommand 0x%02X with a callback is not handled yet\n", com);
    }
    return next_id();
}

int pe_psyq_DsSync(int id, unsigned char *result) {
    (void)id;
    set_result(result);
    return DSL_COMPLETE;
}

/* ---------------------------------------------------------------- ISO 9660 */

static unsigned int le32(const unsigned char *p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((unsigned int)p[3] << 24);
}

/* Find one name in a directory extent. Names compare without ";1" and
 * case-insensitively, like the retail library. */
static int find_in_dir(unsigned int lba, unsigned int size, const char *want, int want_dir,
                       unsigned int *out_lba, unsigned int *out_size) {
    unsigned char sec[2048];
    unsigned int off, n;
    for (n = 0; n < (size + 2047) / 2048; n++) {
        if (pe_host_disc_read((int)(lba + n), sec, 1) != 0) {
            return -1;
        }
        off = 0;
        while (off < 2048) {
            unsigned int len = sec[off];
            char name[64];
            unsigned int nl, i;
            if (len == 0) {
                break;
            }
            nl = sec[off + 32];
            if (nl >= sizeof(name)) {
                nl = sizeof(name) - 1;
            }
            memcpy(name, sec + off + 33, nl);
            name[nl] = 0;
            for (i = 0; i < nl; i++) {
                if (name[i] == ';') {
                    name[i] = 0;
                    break;
                }
            }
            if (((sec[off + 25] & 2) != 0) == want_dir && strcasecmp(name, want) == 0) {
                *out_lba = le32(sec + off + 2);
                *out_size = le32(sec + off + 10);
                return 1;
            }
            off += len;
        }
    }
    return 0;
}

DslFILE *pe_psyq_DsSearchFile(DslFILE *fp, char *path) {
    unsigned char pvd[2048];
    unsigned int dir_lba, dir_size;
    char comp[64];
    const char *p = path;
    if (pe_host_disc_read(16, pvd, 1) != 0 || memcmp(pvd + 1, "CD001", 5) != 0) {
        return (DslFILE *)-1;
    }
    dir_lba = le32(pvd + 156 + 2);
    dir_size = le32(pvd + 156 + 10);
    for (;;) {
        unsigned int n = 0, lba, size;
        int last, r;
        while (*p == '\\' || *p == '/') {
            p++;
        }
        while (*p && *p != '\\' && *p != '/' && *p != ';' && n < sizeof(comp) - 1) {
            comp[n++] = *p++;
        }
        comp[n] = 0;
        last = (*p == 0 || *p == ';');
        r = find_in_dir(dir_lba, dir_size, comp, !last, &lba, &size);
        if (r < 0) {
            return (DslFILE *)-1;
        }
        if (r == 0) {
            return NULL;
        }
        if (last) {
            pe_psyq_DsIntToPos((int)lba, &fp->pos);
            fp->size = size;
            strncpy(fp->name, comp, sizeof(fp->name) - 1);
            fp->name[sizeof(fp->name) - 1] = 0;
            return fp;
        }
        dir_lba = lba;
        dir_size = size;
    }
}

/* Direct CD control (used by the movie player in boot_display and
 * menu_memcard). Streaming playback is not implemented yet: commands are
 * accepted and report success so callers do not wait forever. */
static void *g_ready_cb;
void pe_host_stream_setloc(const unsigned char *param);

int pe_psyq_DsControl(unsigned char com, unsigned char *param, unsigned char *result) {
    if (com == 0x02) { /* CdlSetloc */
        pe_host_stream_setloc(param);
    }
    if (getenv("PE_LOG_CD")) {
        fprintf(stderr, "pe: DsControl 0x%02X (not implemented)\n", com);
    }
    set_result(result);
    return 1;
}
int pe_psyq_DsControlB(unsigned char com, unsigned char *param, unsigned char *result) {
    return pe_psyq_DsControl(com, param, result);
}
void *pe_psyq_DsReadyCallback(void *cb) {
    void *old = g_ready_cb;
    g_ready_cb = cb;
    return old;
}
