#define _GNU_SOURCE
/* Host entry point: set up the PS1 address space, then run the game's main.
 *
 * Usage: parasite-eve <disc 1 .cue>            (the program is read from the disc)
 *        parasite-eve <disc .cue> <SLUS_006.62> (use a separate copy of the program)
 * The disc can also be given in $PE_DISC.
 */
#include "pe_host_mem.h"
#include "pe_host_traps.h"
#include "pe_host_disc.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <ucontext.h>
#include <unistd.h>
#endif

void pe_game_main(void); /* the game's main(), renamed at link time */
void pe_host_sha1(const void *data, unsigned int len, unsigned char out[20]);

/* SHA-1 of SLUS_006.62 (configs/USA/main.yaml); every function address in
 * the build was checked against this file. */
static const unsigned char k_exe_sha1[20] = {
    0x45, 0x2f, 0xb0, 0x33, 0xf2, 0xea, 0xa4, 0xb1, 0x8a, 0xa2,
    0x0a, 0x5b, 0xca, 0x60, 0xb8, 0x12, 0x5a, 0xf3, 0xa3, 0x7b};

static int check_exe(const unsigned char *img, unsigned int len) {
    unsigned char h[20];
    pe_host_sha1(img, len, h);
    if (memcmp(h, k_exe_sha1, 20) != 0) {
        fprintf(stderr, "pe: this is not the SLUS_006.62 the port is built for "
                        "(SHA-1 452fb033f2eaa4b18aa20a5bca60b8125af3a37b)\n");
        return -1;
    }
    return 0;
}

static const char *trap_name(unsigned int addr) {
    unsigned int i;
    for (i = 0; i < pe_psyq_trap_count; i++) {
        if (pe_psyq_trap_table[i].mips_addr == addr) {
            return pe_psyq_trap_table[i].name;
        }
    }
    return NULL;
}

void pe_host_stub_hit(const char *name) {
    /* Report each unimplemented PSY-Q function once. */
    static const char *seen[512];
    static int count;
    int i;
    for (i = 0; i < count; i++) {
        if (seen[i] == name) {
            return;
        }
    }
    if (count < 512) {
        seen[count++] = name;
    }
    fprintf(stderr, "pe: PSY-Q %s is not implemented yet (returns 0)\n", name);
}

#ifndef _WIN32
static void on_fault(int sig, siginfo_t *si, void *ctx) {
    ucontext_t *uc = (ucontext_t *)ctx;
    unsigned int eip = (unsigned int)uc->uc_mcontext.gregs[REG_EIP];
    unsigned int esp = (unsigned int)uc->uc_mcontext.gregs[REG_ESP];
    if (sig == SIGTRAP) {
        const char *n = trap_name(eip - 1);
        fprintf(stderr, "pe: call into PSY-Q code with no host version: %s at 0x%08X "
                        "(return address 0x%08X)\n", n ? n : "?", eip - 1, *(unsigned int *)esp);
    } else {
        fprintf(stderr, "pe: signal %d at eip=0x%08X, address 0x%08X\n", sig, eip,
                (unsigned int)(uintptr_t)si->si_addr);
    }
    _exit(3);
}
#endif

#ifdef _WIN32
static LONG CALLBACK on_exception(EXCEPTION_POINTERS *ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    unsigned int eip = (unsigned int)ep->ContextRecord->Eip;
    if (code == EXCEPTION_BREAKPOINT) {
        const char *n = trap_name(eip);
        if (!n) {
            n = trap_name(eip - 1);
        }
        fprintf(stderr, "pe: call into PSY-Q code with no host version: %s at 0x%08X "
                        "(return address 0x%08X)\n", n ? n : "?", eip,
                *(unsigned int *)(uintptr_t)ep->ContextRecord->Esp);
    } else if (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_ILLEGAL_INSTRUCTION ||
               code == EXCEPTION_PRIV_INSTRUCTION) {
        fprintf(stderr, "pe: exception 0x%08lX at eip=0x%08X, address 0x%08lX\n",
                (unsigned long)code, eip,
                (unsigned long)ep->ExceptionRecord->ExceptionInformation[1]);
    } else {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    fflush(stderr);
    ExitProcess(3);
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

static void install_traps(void) {
    unsigned int i;
    for (i = 0; i < pe_psyq_trap_count; i++) {
        *(unsigned char *)(uintptr_t)pe_psyq_trap_table[i].mips_addr = 0xCC; /* int3 */
    }
}

/* What __SN_ENTRY_POINT (src/main/psyq/libsn/snmain.c) does before main():
 * clear BSS 0x8009CDF8..0x800C20C8 and record the heap start and size.
 * The stack switch is skipped: the game runs on the host stack. */
static void sn_entry(void) {
    unsigned int top, a0, a1;
    memset((void *)0x8009CDF8u, 0, 0x800C20C8u - 0x8009CDF8u);
    top = *(unsigned int *)0x8009CD70u - 8;
    a0 = 0x800C20C8u & 0x1FFFFFFFu;
    a1 = top - *(unsigned int *)0x8009CD74u - a0;
    *(unsigned int *)0x80094540u = a1;
    *(unsigned int *)0x8009453Cu = a0 | 0x80000000u;
}

int main(int argc, char **argv) {
    const char *disc = argc > 1 ? argv[1] : getenv("PE_DISC");
    const char *exe = argc > 2 ? argv[2] : NULL;
    PeExeInfo info;
#ifdef _WIN32
    AddVectoredExceptionHandler(1, on_exception);
#else
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGTRAP, &sa, NULL);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
#endif
    if (!disc || pe_host_disc_open(disc) != 0) {
        fprintf(stderr, "pe: no disc image. Usage: parasite-eve \"Parasite Eve (USA) (Disc 1).cue\"\n");
        if (!exe) {
            return 1;
        }
    }
    if (pe_host_map_memory() != 0) {
        return 1;
    }
    if (exe) {
        if (pe_host_load_exe(exe, &info) != 0) {
            return 1;
        }
    } else {
        unsigned int len;
        unsigned char *img = (unsigned char *)pe_host_disc_load_file("SLUS_006.62", &len);
        if (!img) {
            fprintf(stderr, "pe: SLUS_006.62 not found on the disc (is this Disc 1 of the USA release?)\n");
            return 1;
        }
        if (check_exe(img, len) != 0 || pe_host_load_exe_image(img, len, &info) != 0) {
            return 1;
        }
        free(img);
        exe = "SLUS_006.62 from the disc";
    }
    fprintf(stderr, "pe: loaded %s: 0x%08X..0x%08X, entry 0x%08X\n", exe, info.t_addr,
            info.t_addr + info.t_size, info.pc);
    install_traps();
    if (pe_host_install_jumps(pe_main_func_table, pe_main_func_count) != 0) {
        return 1;
    }
    fprintf(stderr, "pe: %u functions mapped, %u PSY-Q entry points trapped\n",
            pe_main_func_count, pe_psyq_trap_count);
    sn_entry();
    pe_game_main();
    fprintf(stderr, "pe: main returned\n");
    return 0;
}
