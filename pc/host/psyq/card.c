/* Memory cards on the host: the BIOS card functions the game uses, on raw
 * 128 KiB card images (the .mcd/.mcr format emulators use), so existing
 * saves work as they are.
 *
 * Card files: $PE_CARD1 / $PE_CARD2, default memcards/card1.mcd and
 * memcards/card2.mcd next to the working directory. If card 1's file is
 * missing, a blank formatted card is inserted (written to the file at the
 * first save); a missing card 2 file is a missing card. A card is written
 * back to its file after every change.
 *
 * Layout (psx-spx, "Memory Card Data Format"): 16 blocks of 8 KiB, 128-byte
 * frames. Block 0 frame 0 holds "MC"; frames 1-15 are the directory entries
 * for blocks 1-15:
 *   +0x00 u32 state: 0x51 first block of a file, 0x52 middle, 0x53 last,
 *         0xA0 free, 0xA1-0xA3 deleted
 *   +0x04 u32 file size in bytes (first block only)
 *   +0x08 u16 next block of the file minus 1, 0xFFFF at the end
 *   +0x0A name, 20 characters + NUL
 *   +0x7F XOR of bytes 0x00-0x7E
 * Frames 16-35 list broken sectors (0xFFFFFFFF = none). Frame 63 holds "MC".
 *
 * Events, as the game's state machine (src/main/memcard) expects them:
 *   _card_info / _card_load  -> SwCARD (0xF4000001): EvSpIOE (0x0004) when a
 *                               card is present, EvSpTIMOUT (0x0100) when not
 *   _card_clear              -> HwCARD (0xF0000011): EvSpIOE / EvSpTIMOUT
 * They are raised before the call returns; the game clears its flags before
 * each call and checks them on later frames, so that order is safe.
 *
 * File calls ("bu00:NAME" = port 1, "bu10:NAME" = port 2) are synchronous,
 * like the BIOS without FASYNC: read/write move whole 128-byte frames.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

void Psyz_KernelRaise(unsigned int desc, unsigned int spec);

#define SW_CARD 0xF4000001u
#define HW_CARD 0xF0000011u
#define EV_IOE 0x0004u
#define EV_TIMEOUT 0x0100u

#define CARD_SIZE 0x20000
#define BLOCK 0x2000
#define FRAME 0x80

#define FREAD 0x0001
#define FWRITE 0x0002
#define FCREAT 0x0200

typedef struct {
    int present;
    char path[512];
    unsigned char data[CARD_SIZE];
} Card;

static Card g_card[2];
static int g_loaded;

typedef struct {
    int used;
    int port;
    int first_block;
    unsigned int size;
    unsigned int pos;
    int mode;
} CardFile;

static CardFile g_fd[8];

/* PSY-Q struct DIRENTRY (sys/file.h): 40 bytes. */
typedef struct DirEntry {
    char name[20];
    int attr;
    int size;
    struct DirEntry *next;
    int head;
    char system[4];
} DirEntry;

typedef char direntry_size_check[sizeof(DirEntry) == 40 ? 1 : -1];

static void card_path(int port, char *out, size_t n) {
    const char *env = getenv(port ? "PE_CARD2" : "PE_CARD1");
    if (env && *env) {
        snprintf(out, n, "%s", env);
    } else {
        snprintf(out, n, "memcards/card%d.mcd", port + 1);
    }
}

static void format_card(int port);

static void load_cards(void) {
    int p;
    if (g_loaded) {
        return;
    }
    g_loaded = 1;
    for (p = 0; p < 2; p++) {
        FILE *f;
        card_path(p, g_card[p].path, sizeof(g_card[p].path));
        f = fopen(g_card[p].path, "rb");
        if (f) {
            size_t n = fread(g_card[p].data, 1, CARD_SIZE, f);
            fclose(f);
            g_card[p].present = (n == CARD_SIZE);
            if (!g_card[p].present) {
                fprintf(stderr, "pe: %s is not a 128 KiB card image, ignored\n", g_card[p].path);
            }
        }
        if (!f && p == 0) {
            format_card(0);
            g_card[0].present = 1;
            fprintf(stderr, "pe: memory card 1: %s not found, a blank formatted card is inserted "
                            "(saved there on the first write)\n", g_card[0].path);
            continue;
        }
        fprintf(stderr, "pe: memory card %d: %s%s\n", p + 1, g_card[p].path,
                g_card[p].present ? "" : " (not present)");
    }
}

static void save_card(int port) {
    FILE *f;
#ifdef _WIN32
    mkdir("memcards");
#else
    mkdir("memcards", 0755);
#endif
    f = fopen(g_card[port].path, "wb");
    if (!f) {
        fprintf(stderr, "pe: cannot write %s\n", g_card[port].path);
        return;
    }
    fwrite(g_card[port].data, 1, CARD_SIZE, f);
    fclose(f);
}

static unsigned char *frame(int port, int block, int f) {
    return g_card[port].data + block * BLOCK + f * FRAME;
}

static void fix_checksum(unsigned char *fr) {
    unsigned char x = 0;
    int i;
    for (i = 0; i < 0x7F; i++) {
        x ^= fr[i];
    }
    fr[0x7F] = x;
}

static unsigned int rd32(const unsigned char *p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((unsigned int)p[3] << 24);
}

static void wr32(unsigned char *p, unsigned int v) {
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
}

static void format_card(int port) {
    int i;
    memset(g_card[port].data, 0, CARD_SIZE);
    frame(port, 0, 0)[0] = 'M';
    frame(port, 0, 0)[1] = 'C';
    fix_checksum(frame(port, 0, 0));
    for (i = 1; i < 16; i++) {
        unsigned char *fr = frame(port, 0, i);
        wr32(fr, 0xA0);
        fr[8] = 0xFF;
        fr[9] = 0xFF;
        fix_checksum(fr);
    }
    for (i = 16; i < 36; i++) {
        unsigned char *fr = frame(port, 0, i);
        wr32(fr, 0xFFFFFFFFu);
        fr[8] = 0xFF;
        fr[9] = 0xFF;
        fix_checksum(fr);
    }
    frame(port, 0, 63)[0] = 'M';
    frame(port, 0, 63)[1] = 'C';
    fix_checksum(frame(port, 0, 63));
}

static int chan_port(long chan) { return (int)((chan >> 4) & 1); }

static int chan_ok(long chan) { return (chan & 15) == 0 && (chan >> 4) < 2; }

static void card_event(unsigned int desc, long chan) {
    load_cards();
    Psyz_KernelRaise(desc, (chan_ok(chan) && g_card[chan_port(chan)].present) ? EV_IOE : EV_TIMEOUT);
}

void pe_psyq_InitCARD(long val) { (void)val; load_cards(); }
long pe_psyq_StartCARD(void) { return 1; }
void pe_psyq__bu_init(void) { load_cards(); }
long pe_psyq__card_auto(long val) { (void)val; return 1; }
long pe_psyq__card_info(long chan) { card_event(SW_CARD, chan); return 1; }
long pe_psyq__card_load(long chan) { card_event(SW_CARD, chan); return 1; }
long pe_psyq__card_clear(long chan) { card_event(HW_CARD, chan); return 1; }

/* "bu00:NAME" -> port, name. Returns -1 if not a card path. */
static int parse_path(const char *path, char *name, size_t n) {
    int port;
    if (strncmp(path, "bu", 2) != 0 || path[2] < '0' || path[2] > '1' || path[4] != ':') {
        return -1;
    }
    port = path[2] - '0';
    snprintf(name, n, "%s", path + 5);
    return port;
}

static int find_file(int port, const char *name) {
    int b;
    for (b = 1; b < 16; b++) {
        unsigned char *fr = frame(port, 0, b);
        if (rd32(fr) == 0x51 && strncmp((const char *)fr + 0x0A, name, 20) == 0) {
            return b;
        }
    }
    return -1;
}

static int file_block(int port, int first, unsigned int n) {
    int b = first;
    while (n-- && b > 0) {
        unsigned int next = frame(port, 0, b)[8] | (frame(port, 0, b)[9] << 8);
        b = next == 0xFFFF ? -1 : (int)next + 1;
    }
    return b;
}

static int create_file(int port, const char *name, int blocks) {
    int free_list[15], nfree = 0, b, i;
    if (blocks <= 0) {
        blocks = 1;
    }
    for (b = 1; b < 16; b++) {
        unsigned int st = rd32(frame(port, 0, b));
        if (st == 0xA0 || (st >= 0xA1 && st <= 0xA3)) {
            free_list[nfree++] = b;
        }
    }
    if (nfree < blocks) {
        return -1;
    }
    for (i = 0; i < blocks; i++) {
        unsigned char *fr = frame(port, 0, free_list[i]);
        memset(fr, 0, FRAME);
        wr32(fr, i == 0 ? 0x51 : (i == blocks - 1 ? 0x53 : 0x52));
        if (i == 0) {
            wr32(fr + 4, (unsigned int)blocks * BLOCK);
            snprintf((char *)fr + 0x0A, 21, "%s", name);
        }
        if (i + 1 < blocks) {
            fr[8] = (unsigned char)(free_list[i + 1] - 1);
            fr[9] = (unsigned char)((free_list[i + 1] - 1) >> 8);
        } else {
            fr[8] = 0xFF;
            fr[9] = 0xFF;
        }
        fix_checksum(fr);
    }
    if (blocks == 1) {
        /* a one-block file is first and last: state stays 0x51 */
    }
    save_card(port);
    return free_list[0];
}

int pe_psyq_open(const char *path, int mode) {
    char name[64];
    int port, first, fd;
    load_cards();
    port = parse_path(path, name, sizeof(name));
    if (port < 0 || !g_card[port].present) {
        return -1;
    }
    first = find_file(port, name);
    if (mode & FCREAT) {
        if (first >= 0) {
            return -1;
        }
        first = create_file(port, name, (mode >> 16) & 0xF);
        if (first < 0) {
            return -1;
        }
    } else if (first < 0) {
        return -1;
    }
    for (fd = 0; fd < 8; fd++) {
        if (!g_fd[fd].used) {
            g_fd[fd].used = 1;
            g_fd[fd].port = port;
            g_fd[fd].first_block = first;
            g_fd[fd].size = rd32(frame(port, 0, first) + 4);
            g_fd[fd].pos = 0;
            g_fd[fd].mode = mode;
            return fd;
        }
    }
    return -1;
}

int pe_psyq_close(int fd) {
    if (fd < 0 || fd >= 8 || !g_fd[fd].used) {
        return -1;
    }
    g_fd[fd].used = 0;
    return fd;
}

static int xfer(int fd, unsigned char *buf, unsigned int n, int write) {
    CardFile *f;
    unsigned int done = 0;
    if (fd < 0 || fd >= 8 || !g_fd[fd].used) {
        return -1;
    }
    f = &g_fd[fd];
    if (n % FRAME) {
        n -= n % FRAME;
    }
    while (done < n && f->pos < f->size) {
        int b = file_block(f->port, f->first_block, f->pos / BLOCK);
        unsigned int in = f->pos % BLOCK, len = BLOCK - in;
        if (b < 0) {
            break;
        }
        if (len > n - done) {
            len = n - done;
        }
        if (write) {
            memcpy(g_card[f->port].data + b * BLOCK + in, buf + done, len);
        } else {
            memcpy(buf + done, g_card[f->port].data + b * BLOCK + in, len);
        }
        done += len;
        f->pos += len;
    }
    if (write && done) {
        save_card(f->port);
    }
    return (int)done;
}

int pe_psyq_read(int fd, void *buf, int n) { return xfer(fd, (unsigned char *)buf, (unsigned int)n, 0); }
int pe_psyq_write(int fd, void *buf, int n) { return xfer(fd, (unsigned char *)buf, (unsigned int)n, 1); }

unsigned int pe_psyq_lseek(int fd, unsigned int off, int whence) {
    if (fd < 0 || fd >= 8 || !g_fd[fd].used) {
        return (unsigned int)-1;
    }
    if (whence == 0) {
        g_fd[fd].pos = off;
    } else if (whence == 1) {
        g_fd[fd].pos += off;
    }
    return g_fd[fd].pos;
}

int pe_psyq_erase(const char *path) {
    char name[64];
    int port, b;
    load_cards();
    port = parse_path(path, name, sizeof(name));
    if (port < 0 || !g_card[port].present || (b = find_file(port, name)) < 0) {
        return 0;
    }
    while (b > 0) {
        unsigned char *fr = frame(port, 0, b);
        unsigned int st = rd32(fr), next = fr[8] | (fr[9] << 8);
        wr32(fr, 0xA0 | (st & 0x0F));
        fix_checksum(fr);
        b = next == 0xFFFF ? -1 : (int)next + 1;
    }
    save_card(port);
    return 1;
}

int pe_psyq_format(const char *dev) {
    char name[64];
    int port;
    load_cards();
    port = parse_path(dev, name, sizeof(name));
    if (port < 0 || !g_card[port].present) {
        return 0;
    }
    format_card(port);
    save_card(port);
    return 1;
}

/* Directory search. Pattern after "buXX:" may end in '*'; '?' matches one
 * character. */
static int g_search_port, g_search_next;
static char g_search_pat[32];

static int match(const char *pat, const char *s) {
    for (; *pat; pat++, s++) {
        if (*pat == '*') {
            return 1;
        }
        if (*pat != '?' && *pat != *s) {
            return 0;
        }
        if (!*s) {
            return 0;
        }
    }
    return *s == 0;
}

static DirEntry *search(DirEntry *e) {
    for (; g_search_next < 16; g_search_next++) {
        unsigned char *fr = frame(g_search_port, 0, g_search_next);
        char nm[21];
        if (rd32(fr) != 0x51) {
            continue;
        }
        memcpy(nm, fr + 0x0A, 20);
        nm[20] = 0;
        if (!match(g_search_pat, nm)) {
            continue;
        }
        memset(e, 0, sizeof(*e));
        memcpy(e->name, nm, 20);
        e->size = (int)rd32(fr + 4);
        e->head = g_search_next;
        g_search_next++;
        return e;
    }
    return NULL;
}

DirEntry *pe_psyq_firstfile(const char *path, DirEntry *e) {
    char name[64];
    int port;
    load_cards();
    port = parse_path(path, name, sizeof(name));
    if (port < 0 || !g_card[port].present) {
        return NULL;
    }
    g_search_port = port;
    g_search_next = 1;
    snprintf(g_search_pat, sizeof(g_search_pat), "%s", name[0] ? name : "*");
    return search(e);
}

DirEntry *pe_psyq_nextfile(DirEntry *e) { return search(e); }
