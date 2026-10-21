/* Disc image access: a CUE sheet naming one MODE2/2352 BIN (the usual
 * Redump layout), or a BIN directly. A BIN can also be given as numbered
 * pieces "<name>.part0", "<name>.part1", ... (a split copy). */
#include "pe_host_disc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RAW 2352
#define MAX_PARTS 16

static FILE *g_part[MAX_PARTS];
static long long g_part_size[MAX_PARTS];
static int g_parts;

static void close_all(void) {
    int i;
    for (i = 0; i < g_parts; i++) {
        fclose(g_part[i]);
    }
    g_parts = 0;
}

static int open_bin(const char *bin) {
    FILE *f = fopen(bin, "rb");
    close_all();
    if (f) {
        g_part[0] = f;
        fseek(f, 0, SEEK_END);
        g_part_size[0] = ftell(f);
        g_parts = 1;
        return 0;
    }
    for (g_parts = 0; g_parts < MAX_PARTS; g_parts++) {
        char name[1024];
        snprintf(name, sizeof(name), "%s.part%d", bin, g_parts);
        f = fopen(name, "rb");
        if (!f) {
            break;
        }
        g_part[g_parts] = f;
        fseek(f, 0, SEEK_END);
        g_part_size[g_parts] = ftell(f);
    }
    return g_parts ? 0 : -1;
}

int pe_host_disc_open(const char *path) {
    const char *dot = strrchr(path, '.');
    if (dot && (strcmp(dot, ".cue") == 0 || strcmp(dot, ".CUE") == 0)) {
        char line[1024], bin[1024], dir[1024];
        const char *slash;
        FILE *cue = fopen(path, "r");
        if (!cue) {
            fprintf(stderr, "pe: cannot open %s\n", path);
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
        if (open_bin(dir) != 0) {
            fprintf(stderr, "pe: cannot open disc image %s\n", dir);
            return -1;
        }
        return 0;
    }
    if (open_bin(path) != 0) {
        fprintf(stderr, "pe: cannot open disc image %s\n", path);
        return -1;
    }
    return 0;
}

int pe_host_disc_ok(void) { return g_parts > 0; }

int pe_host_disc_read_raw(int lba, void *dst) {
    long long off = (long long)lba * RAW;
    int i;
    for (i = 0; i < g_parts; i++) {
        if (off < g_part_size[i]) {
            if (off + RAW > g_part_size[i]) {
                /* sector straddles two pieces */
                long long first = g_part_size[i] - off;
                if (i + 1 >= g_parts) {
                    return -1;
                }
                fseek(g_part[i], (long)off, SEEK_SET);
                if (fread(dst, 1, (size_t)first, g_part[i]) != (size_t)first) {
                    return -1;
                }
                fseek(g_part[i + 1], 0, SEEK_SET);
                return fread((char *)dst + first, 1, (size_t)(RAW - first), g_part[i + 1]) ==
                               (size_t)(RAW - first) ? 0 : -1;
            }
            fseek(g_part[i], (long)off, SEEK_SET);
            return fread(dst, 1, RAW, g_part[i]) == RAW ? 0 : -1;
        }
        off -= g_part_size[i];
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
