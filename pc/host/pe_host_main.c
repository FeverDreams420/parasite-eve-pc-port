#define _GNU_SOURCE
/* Host entry point: set up the PS1 address space, then run the game's main.
 *
 * Usage: parasite-eve <disc 1 .cue>            (the program is read from the disc)
 *        parasite-eve <disc .cue> <SLUS_006.62> (use a separate copy of the program)
 * The disc can also be given in $PE_DISC, or found without a command line
 * (psyq/launch.c: settings.ini disc=, next to the exe, or a file window).
 * Disc 2 is opened from the same name with "Disc 2" (or $PE_DISC2), else
 * from settings.ini disc2= or a file window (launch.c), and changed in by
 * the game's disc screen.
 */
#include "pe_host_mem.h"
#include "pe_host_traps.h"
#include "pe_host_disc.h"
#include "pe_host_overlays.h"

/* psyq/launch.c: double-click start, finding Disc 1 */
void pe_host_launch_setup(void);
const char *pe_host_launch_find_disc(void);
const char *pe_host_launch_find_disc2(void);
void pe_host_launch_error(const char *msg);

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
int pe_host_lowmem_emulate(const unsigned char *ip, unsigned int addr, unsigned int *r[8]);

void pe_host_cd_report(void);

/* After a fatal fault: the stack words at esp, the return addresses found by
 * following the saved-ebp chain (the game is built with frame pointers), the
 * frame number, the last CD reads and what overlays are loaded. Map the
 * addresses back to functions with addr2line on the same .exe. */
unsigned int pe_host_frame_number(void);

/* crash-dump.bin next to the program: "PECRASH1", eip, esp, ebp (u32 each),
 * then 16 KiB of the host stack from esp, the 2 MiB of PS1 RAM and the 1 KiB
 * scratchpad. It holds the game's memory, so it stays on your computer. */
static void crash_dump(unsigned int eip, unsigned int esp, unsigned int ebp) {
    FILE *f = fopen("crash-dump.bin", "wb");
    unsigned int hdr[3] = {eip, esp, ebp};
    if (!f) {
        fprintf(stderr, "pe:   (could not write crash-dump.bin)\n");
        return;
    }
    unsigned int n = 0x4000, room = 0;
    static unsigned char zero[0x4000];
#ifdef _WIN32
    {
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery((const void *)(uintptr_t)esp, &mbi, sizeof(mbi)) == sizeof(mbi) && mbi.State == MEM_COMMIT) {
            room = (unsigned int)((uintptr_t)mbi.BaseAddress + mbi.RegionSize - esp);
        }
    }
#else
    {
        /* the stack mapping's end, from /proc/self/maps */
        FILE *m = fopen("/proc/self/maps", "r");
        char line[256];
        while (m && fgets(line, sizeof(line), m)) {
            unsigned long a, b;
            if (sscanf(line, "%lx-%lx", &a, &b) == 2 && esp >= a && esp < b) {
                room = (unsigned int)(b - esp);
                break;
            }
        }
        if (m) {
            fclose(m);
        }
    }
#endif
    if (room < n) {
        n = room;
    }
    fwrite("PECRASH1", 1, 8, f);
    fwrite(hdr, 4, 3, f);
    fwrite((const void *)(uintptr_t)esp, 1, n, f);
    fwrite(zero, 1, 0x4000 - n, f); /* unreadable part as zeros */
    fwrite((const void *)(uintptr_t)0x80000000u, 1, 0x200000, f);
    fwrite((const void *)(uintptr_t)0x1F800000u, 1, 0x400, f);
    fclose(f);
    fprintf(stderr, "pe:   memory saved to crash-dump.bin\n");
}

static void crash_report(unsigned int eip, unsigned int esp, unsigned int ebp) {
    unsigned int i, *sp = (unsigned int *)(uintptr_t)esp;
    fprintf(stderr, "pe: crash report (frame %u): eip=0x%08X esp=0x%08X ebp=0x%08X\n", pe_host_frame_number(),
            eip, esp, ebp);
    fprintf(stderr, "pe:   stack:");
    for (i = 0; i < 16; i++) {
        fprintf(stderr, " %08X", sp[i]);
    }
    fprintf(stderr, "\n");
    fprintf(stderr, "pe:   call chain:");
    for (i = 0; i < 24; i++) {
        unsigned int *fp = (unsigned int *)(uintptr_t)ebp;
        if (ebp < esp || ebp - esp > 0x100000u || (ebp & 3)) {
            break;
        }
        fprintf(stderr, " %08X", fp[1]);
        if (fp[0] <= ebp) {
            break;
        }
        ebp = fp[0];
    }
    fprintf(stderr, "\n");
    pe_host_cd_report();
    pe_host_overlays_report();
    crash_dump(eip, esp, ebp);
    fflush(stderr);
}

/* Is `eip` inside this program's own code (where the game's code is)? */
#ifdef _WIN32
static int in_own_code(unsigned int eip) {
    const unsigned char *base = (const unsigned char *)GetModuleHandleA(NULL);
    const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)(base + ((const IMAGE_DOS_HEADER *)base)->e_lfanew);
    return eip >= (unsigned int)(uintptr_t)base && eip < (unsigned int)(uintptr_t)base + nt->OptionalHeader.SizeOfImage;
}
#else
extern char __executable_start[], etext[];
static int in_own_code(unsigned int eip) {
    return eip >= (unsigned int)(uintptr_t)__executable_start && eip < (unsigned int)(uintptr_t)etext;
}
#endif

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

/* A translated assembler routine reached a `break` instruction. */
void pe_asm_break(const char *name) {
    fprintf(stderr, "pe: break instruction in %s\n", name);
    abort();
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
    if (sig == SIGSEGV && (uintptr_t)si->si_addr < 0x10000u && in_own_code(eip)) {
        greg_t *g = uc->uc_mcontext.gregs;
        unsigned int *r[8] = {(unsigned int *)&g[REG_EAX], (unsigned int *)&g[REG_ECX],
                              (unsigned int *)&g[REG_EDX], (unsigned int *)&g[REG_EBX],
                              (unsigned int *)&g[REG_ESP], (unsigned int *)&g[REG_EBP],
                              (unsigned int *)&g[REG_ESI], (unsigned int *)&g[REG_EDI]};
        int len = pe_host_lowmem_emulate((const unsigned char *)(uintptr_t)eip,
                                         (unsigned int)(uintptr_t)si->si_addr, r);
        if (len > 0) {
            g[REG_EIP] = (greg_t)(eip + (unsigned int)len);
            return;
        }
    }
    if (sig == SIGTRAP) {
        const char *n = trap_name(eip - 1);
        fprintf(stderr, "pe: call into PSY-Q code with no host version: %s at 0x%08X "
                        "(return address 0x%08X)\n", n ? n : "?", eip - 1, *(unsigned int *)esp);
    } else {
        fprintf(stderr, "pe: signal %d at eip=0x%08X, address 0x%08X\n", sig, eip,
                (unsigned int)(uintptr_t)si->si_addr);
        crash_report(eip, esp, (unsigned int)uc->uc_mcontext.gregs[REG_EBP]);
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
    } else if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->ExceptionInformation[1] < 0x10000u &&
               in_own_code(eip)) {
        CONTEXT *c = ep->ContextRecord;
        unsigned int *r[8] = {(unsigned int *)&c->Eax, (unsigned int *)&c->Ecx, (unsigned int *)&c->Edx,
                              (unsigned int *)&c->Ebx, (unsigned int *)&c->Esp, (unsigned int *)&c->Ebp,
                              (unsigned int *)&c->Esi, (unsigned int *)&c->Edi};
        int len = pe_host_lowmem_emulate((const unsigned char *)(uintptr_t)eip,
                                         (unsigned int)ep->ExceptionRecord->ExceptionInformation[1], r);
        if (len > 0) {
            c->Eip = eip + (unsigned int)len;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        fprintf(stderr, "pe: exception 0x%08lX at eip=0x%08X, address 0x%08lX\n", (unsigned long)code, eip,
                (unsigned long)ep->ExceptionRecord->ExceptionInformation[1]);
        crash_report(eip, (unsigned int)ep->ContextRecord->Esp, (unsigned int)ep->ContextRecord->Ebp);
    } else if (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_ILLEGAL_INSTRUCTION ||
               code == EXCEPTION_PRIV_INSTRUCTION) {
        fprintf(stderr, "pe: exception 0x%08lX at eip=0x%08X, address 0x%08lX\n",
                (unsigned long)code, eip,
                (unsigned long)ep->ExceptionRecord->ExceptionInformation[1]);
        crash_report(eip, (unsigned int)ep->ContextRecord->Esp, (unsigned int)ep->ContextRecord->Ebp);
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
    pe_host_launch_setup();
    if (!disc) {
        disc = pe_host_launch_find_disc();
    }
    if (!disc || pe_host_disc_open(disc) != 0) {
        if (!exe) {
            pe_host_launch_error(disc ? "Cannot open the disc image (see run-log.txt)."
                                      : "No disc image. Choose your \"Parasite Eve (USA) (Disc 1).cue\", "
                                        "or start: parasite-eve \"Parasite Eve (USA) (Disc 1).cue\"");
            return 1;
        }
        fprintf(stderr, "pe: no disc image\n");
    } else if (!pe_host_disc_available(1)) {
        const char *disc2 = pe_host_launch_find_disc2();
        if (disc2) {
            pe_host_disc_open_disc2(disc2);
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
            pe_host_launch_error("SLUS_006.62 not found on the disc (is this Disc 1 of the USA release?)");
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
    if (pe_host_datacode_init() != 0) {
        pe_host_launch_error("This SLUS_006.62 does not match the port (see run-log.txt).");
        return 1;
    }
    if (pe_host_install_jumps(pe_main_func_table, pe_main_func_count) != 0 ||
        pe_host_install_jumps(pe_data_code_table, pe_data_code_count) != 0) {
        return 1;
    }
    fprintf(stderr, "pe: %u functions mapped (%u of them kept as data by the decomp), %u PSY-Q entry points trapped\n",
            pe_main_func_count + pe_data_code_count, pe_data_code_count, pe_psyq_trap_count);
    pe_host_overlays_init();
    sn_entry();
    pe_game_main();
    fprintf(stderr, "pe: main returned\n");
    return 0;
}
