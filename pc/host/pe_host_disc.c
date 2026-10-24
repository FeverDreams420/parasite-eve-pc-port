/* Disc image access: a CUE sheet naming one MODE2/2352 BIN (the usual
 * Redump layout), or a BIN directly. A BIN can also be given as numbered
 * pieces "<name>.part0", "<name>.part1", ... (a split copy). Disc 2 is
 * opened too when it is found next to Disc 1 (open_disc2). */
#include "pe_host_disc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RAW 2352
#define MAX_PARTS 16
#define MAX_DISCS 2

/* One disc image: a BIN, or its numbered pieces. */
typedef struct {
    FILE *part[MAX_PARTS];
    long long part_size[MAX_PARTS];
    int parts;
} DiscImage;

/* Disc 1 and (when found) Disc 2. Every read goes to the disc "in the
 * drive", g_cur; pe_host_disc_select changes it (the game's disc screen,
 * see DsStatus in psyq/ds.c). */
static DiscImage g_disc[MAX_DISCS];
static int g_cur;

static void close_all(DiscImage *d) {
    int i;
    for (i = 0; i < d->parts; i++) {
        fclose(d->part[i]);
    }
    d->parts = 0;
}

static int open_bin(DiscImage *d, const char *bin) {
    FILE *f = fopen(bin, "rb");
    close_all(d);
    if (f) {
        d->part[0] = f;
        fseek(f, 0, SEEK_END);
        d->part_size[0] = ftell(f);
        d->parts = 1;
        return 0;
    }
    for (d->parts = 0; d->parts < MAX_PARTS; d->parts++) {
        char name[1024];
        snprintf(name, sizeof(name), "%s.part%d", bin, d->parts);
        f = fopen(name, "rb");
        if (!f) {
            break;
        }
        d->part[d->parts] = f;
        fseek(f, 0, SEEK_END);
        d->part_size[d->parts] = ftell(f);
    }
    return d->parts ? 0 : -1;
}

static int open_image(DiscImage *d, const char *path, int quiet) {
    const char *dot = strrchr(path, '.');
    if (dot && (strcmp(dot, ".cue") == 0 || strcmp(dot, ".CUE") == 0)) {
        char line[1024], bin[1024], dir[1024];
        const char *slash;
        FILE *cue = fopen(path, "r");
        if (!cue) {
            if (!quiet) {
                fprintf(stderr, "pe: cannot open %s\n", path);
            }
            return -1;
        }
        bin[0] = 0;
        while (fgets(line, sizeof(line), cue)) {
            char *q1 = strchr(line, '"'), *q2;
            if (strncmp(line, "FILE", 4) == 0 && q1 && (q2 = strchr(q1 + 1, '"'))) {
                *q2 = 0;
                snprintf(bin, sizeof(bin), "%s", q1 + 1);
                break;
            }
        }
        fclose(cue);
        if (!bin[0]) {
            fprintf(stderr, "pe: no FILE line in %s\n", path);
            return -1;
        }
        slash = strrchr(path, '/');
        if (!slash) {
            slash = strrchr(path, '\\');
        }
        if (slash) {
            snprintf(dir, sizeof(dir), "%.*s/%s", (int)(slash - path), path, bin);
        } else {
            snprintf(dir, sizeof(dir), "%s", bin);
        }
        if (open_bin(d, dir) != 0) {
            fprintf(stderr, "pe: cannot open disc image %s\n", dir);
            return -1;
        }
        return 0;
    }
    if (open_bin(d, path) != 0) {
        if (!quiet) {
            fprintf(stderr, "pe: cannot open disc image %s\n", path);
        }
        return -1;
    }
    return 0;
}

/* Disc 2 is looked for with the same name as Disc 1 but "Disc 2" in place
 * of "Disc 1" (the usual Redump names), or named by PE_DISC2. */
static void open_disc2(const char *disc1) {
    const char *env = getenv("PE_DISC2");
    char name[1024];
    const char *at;
    if (env && env[0]) {
        if (open_image(&g_disc[1], env, 0) == 0) {
            fprintf(stderr, "pe: Disc 2: %s\n", env);
        }
        return;
    }
    at = strstr(disc1, "Disc 1");
    if (!at) {
        at = strstr(disc1, "disc 1");
    }
    if (!at) {
        fprintf(stderr, "pe: no Disc 2 image (name has no \"Disc 1\" to swap; set PE_DISC2)\n");
        return;
    }
    snprintf(name, sizeof(name), "%.*s%.4s 2%s", (int)(at - disc1), disc1, at, at + 6);
    if (open_image(&g_disc[1], name, 1) == 0) {
        fprintf(stderr, "pe: Disc 2: %s\n", name);
    } else {
        fprintf(stderr, "pe: no Disc 2 image (looked for %s)\n", name);
    }
}

int pe_host_disc_open(const char *path) {
    g_cur = 0;
    if (open_image(&g_disc[0], path, 0) != 0) {
        return -1;
    }
    open_disc2(path);
    return 0;
}

int pe_host_disc_ok(void) { return g_disc[g_cur].parts > 0; }

int pe_host_disc_available(int index) {
    return index >= 0 && index < MAX_DISCS && g_disc[index].parts > 0;
}

int pe_host_disc_current(void) { return g_cur; }

int pe_host_disc_select(int index) {
    if (!pe_host_disc_available(index)) {
        return -1;
    }
    g_cur = index;
    return 0;
}

int pe_host_disc_read_raw(int lba, void *dst) {
    DiscImage *d = &g_disc[g_cur];
    long long off = (long long)lba * RAW;
    int i;
    for (i = 0; i < d->parts; i++) {
        if (off < d->part_size[i]) {
            if (off + RAW > d->part_size[i]) {
                /* sector straddles two pieces */
                long long first = d->part_size[i] - off;
                if (i + 1 >= d->parts) {
                    return -1;
                }
                fseek(d->part[i], (long)off, SEEK_SET);
                if (fread(dst, 1, (size_t)first, d->part[i]) != (size_t)first) {
                    return -1;
                }
                fseek(d->part[i + 1], 0, SEEK_SET);
                return fread((char *)dst + first, 1, (size_t)(RAW - first), d->part[i + 1]) ==
                               (size_t)(RAW - first) ? 0 : -1;
            }
            fseek(d->part[i], (long)off, SEEK_SET);
            return fread(dst, 1, RAW, d->part[i]) == RAW ? 0 : -1;
        }
        off -= d->part_size[i];
    }
    return -1;
}

int pe_host_disc_read(int lba, void *dst, int count) {
    unsigned char raw[RAW];
    int i;
    for (i = 0; i < count; i++) {
        if (pe_host_disc_read_raw(lba + i, raw) != 0) {
            return -1;
        }
        /* MODE2 form 1: 12 sync + 4 header + 8 subheader, then 2048 data */
        memcpy((char *)dst + i * 2048, raw + 24, 2048);
    }
    return 0;
}

/* ---------------------------------------------------------------- ISO 9660 */

static unsigned int le32(const unsigned char *p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((unsigned int)p[3] << 24);
}

/* Look up a file in the root directory by name (without ";1"). */
int pe_host_disc_find_root_file(const char *want, unsigned int *lba, unsigned int *size) {
    unsigned char sec[2048];
    unsigned int dir_lba, dir_size, n, off;
    if (pe_host_disc_read(16, sec, 1) != 0 || memcmp(sec + 1, "CD001", 5) != 0) {
        return -1;
    }
    dir_lba = le32(sec + 156 + 2);
    dir_size = le32(sec + 156 + 10);
    for (n = 0; n < (dir_size + 2047) / 2048; n++) {
        if (pe_host_disc_read((int)(dir_lba + n), sec, 1) != 0) {
            return -1;
        }
        for (off = 0; off < 2048 && sec[off];) {
            unsigned int nl = sec[off + 32], i;
            char name[64];
            if (nl >= sizeof(name)) {
                nl = sizeof(name) - 1;
            }
            memcpy(name, sec + off + 33, nl);
            name[nl] = 0;
            for (i = 0; i < nl; i++) {
                if (name[i] == ';') {
                    name[i] = 0;
                }
            }
            if (strcmp(name, want) == 0) {
                *lba = le32(sec + off + 2);
                *size = le32(sec + off + 10);
                return 0;
            }
            off += sec[off];
        }
    }
    return -1;
}

/* Copy a whole file from the disc into a host buffer (malloc'd). */
void *pe_host_disc_load_file(const char *name, unsigned int *size_out) {
    unsigned int lba, size;
    unsigned char *buf;
    if (pe_host_disc_find_root_file(name, &lba, &size) != 0) {
        return NULL;
    }
    buf = (unsigned char *)malloc((size + 2047) & ~2047u);
    if (!buf || pe_host_disc_read((int)lba, buf, (int)((size + 2047) / 2048)) != 0) {
        free(buf);
        return NULL;
    }
    *size_out = size;
    return buf;
}
