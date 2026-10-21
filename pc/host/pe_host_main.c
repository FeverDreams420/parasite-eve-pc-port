#define _GNU_SOURCE
/* Host entry point: set up the PS1 address space, then run the game's main.
 *
 * Usage: parasite-eve [path/to/SLUS_006.62]   (default assets/USA/main.exe)
 */
#include "pe_host_mem.h"
#include "pe_host_traps.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <ucontext.h>
#include <unistd.h>
#endif

void pe_game_main(void); /* the game's main(), renamed at link time */

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
    const char *exe = argc > 1 ? argv[1] : "assets/USA/main.exe";
    PeExeInfo info;
#ifndef _WIN32
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGTRAP, &sa, NULL);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
#endif
    if (pe_host_map_memory() != 0 || pe_host_load_exe(exe, &info) != 0) {
        return 1;
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
