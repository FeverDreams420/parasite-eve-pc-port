/* PS1 BIOS functions the game calls (through PSY-Q libc/libapi trampolines),
 * implemented with the host C library. BIOS memcpy/memset return the
 * destination like the C functions. printf goes to stderr with a prefix. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void *pe_psyq_memcpy(void *d, const void *s, unsigned int n) { return memcpy(d, s, n); }
void *pe_psyq_memset(void *d, int c, unsigned int n) { return memset(d, c, n); }
void pe_psyq_bzero(void *d, unsigned int n) { memset(d, 0, n); }
void *pe_psyq_memchr(const void *s, int c, unsigned int n) { return memchr(s, c, n); }
unsigned int pe_psyq_strlen(const char *s) { return (unsigned int)strlen(s); }
char *pe_psyq_strcpy(char *d, const char *s) { return strcpy(d, s); }
int pe_psyq_strncmp(const char *a, const char *b, unsigned int n) { return strncmp(a, b, n); }

int pe_psyq_printf(const char *fmt, ...) {
    va_list ap;
    int r;
    fputs("[game] ", stderr);
    va_start(ap, fmt);
    r = vfprintf(stderr, fmt, ap);
    va_end(ap);
    return r;
}

void pe_psyq_exit(int code) {
    fprintf(stderr, "pe: game called exit(%d)\n", code);
    exit(code);
}

long GetVideoMode(void);           /* PsyZ libetc */
void DeliverEvent(unsigned ev1, unsigned ev2); /* PsyZ kernel */

long pe_psyq_GetVideoMode(void) { return GetVideoMode(); }
void pe_psyq_DeliverEvent(unsigned int a, unsigned int b) { DeliverEvent(a, b); }

/* BIOS rand/srand: x = x * 0x41C64E6D + 0x3039, return (x >> 16) & 0x7FFF.
 * Same generator as PSY-Q libc2 (third_party/psyz/decomp/src/libc2/rand.c).
 * The initial seed before the game's first srand is not modelled (0). */
static unsigned int g_rand_seed;
int pe_psyq_rand(void) {
    g_rand_seed = g_rand_seed * 0x41C64E6Du + 0x3039u;
    return (int)((g_rand_seed >> 16) & 0x7FFF);
}
void pe_psyq_srand(unsigned int seed) { g_rand_seed = seed; }

/* _patch_gte (called by InitGeom) copies a handler into the BIOS exception
 * code so GTE state survives interrupts, through the B0 function table. The
 * host has no BIOS and its interrupt model never touches GTE state, so there
 * is nothing to patch. */
void pe_psyq__patch_gte(void) {}

/* More BIOS string functions used by overlays. */
char *pe_psyq_strcat(char *d, const char *s) { return strcat(d, s); }
