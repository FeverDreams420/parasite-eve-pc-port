/* SPU registers written by game code directly.
 *
 * The sound driver (src/main/akao) and three retail libspu routines
 * (SpuSetVoiceVolumeAttr, SpuGetVoiceEnvelope, SpuSetReverbModeDepth, compiled
 * from the decomp) store to the SPU registers at 0x1F801C00..0x1F801DFF.
 * That window is ordinary memory on the host, so it acts as a shadow:
 * pe_host_spu_sync() forwards every register the game changed since the
 * last sync to PsyZ's SPU (Psyz_SpuWrite), then reloads the shadow from
 * PsyZ so register reads (envelope, status, ENDX) return live values.
 * Key on/off (0x188..0x18F) are write-only triggers: they are cleared in the
 * shadow after each sync so the same value written again is seen again.
 * The sound driver's own key-on/key-off writes (Spu_WriteKeyOn/Off) do not
 * go through the shadow: pe_host_spu_key() first forwards every pending
 * register change, then the trigger, so each one reaches the SPU in the
 * game's order and a second key-off before the next sync cannot replace
 * the first (which left a voice playing until it was reused).
 *
 * Sync points: after every root-counter/VBlank handler the game installed
 * (the sound driver runs from the 240 Hz counter 2 event), at VSync, and
 * around the PsyZ libspu calls.
 */
#include <stdint.h>
#include <string.h>

void Psyz_SpuWrite(unsigned int reg_offset, unsigned short value);
unsigned short Psyz_SpuRead(unsigned int reg_offset);

#define SPU_BASE 0x1F801C00u
#define SPU_REGS 0x100 /* halfwords, 0x200 bytes */

static unsigned short g_synced[SPU_REGS];
static int g_ready;

static volatile unsigned short *shadow(void) {
    return (volatile unsigned short *)(uintptr_t)SPU_BASE;
}

static int is_trigger(unsigned int off) { return off >= 0x188 && off < 0x190; }

static void reload(void) {
    volatile unsigned short *s = shadow();
    unsigned int i;
    for (i = 0; i < SPU_REGS; i++) {
        unsigned int off = i * 2;
        unsigned short v = is_trigger(off) ? 0 : Psyz_SpuRead(off);
        s[i] = v;
        g_synced[i] = v;
    }
    g_ready = 1;
}

/* Debug: PE_LOG_SPU=1 prints, every 300 frames, the key-ons seen since the
 * last report, SPUCNT, main/reverb/CD volumes, and the voices whose
 * envelope (register +0xC) is not zero, with pitch and volume. */
#include <stdio.h>
#include <stdlib.h>
unsigned int pe_host_frame_number(void);
static unsigned int g_keyons, g_log_frame;

static void spu_log(void) {
    volatile unsigned short *s = shadow();
    unsigned int f = pe_host_frame_number(), v;
    if (f < g_log_frame + 300) {
        return;
    }
    g_log_frame = f;
    fprintf(stderr, "pe: spu frame %u: key-ons %u, SPUCNT %04X, main vol %04X/%04X, reverb vol %04X/%04X, "
                    "CD vol %04X/%04X, reverb on %04X%04X\n",
            f, g_keyons, s[0x1AA / 2], s[0x180 / 2], s[0x182 / 2], s[0x184 / 2], s[0x186 / 2], s[0x1B0 / 2],
            s[0x1B2 / 2], s[0x19A / 2], s[0x198 / 2]);
    {
        unsigned char *Psyz_SpuGetRam(void);
        unsigned char *ram = Psyz_SpuGetRam();
        unsigned int base = (unsigned int)s[0x1A2 / 2] << 3, a, nz = 0;
        for (a = base; a < 0x80000; a++) {
            nz += ram[a] != 0;
        }
        fprintf(stderr, "pe:   reverb work area 0x%05X..0x7FFFF (%u bytes), %u non-zero; first bytes:", base,
                0x80000 - base, nz);
        for (a = base; a < base + 16 && a < 0x80000; a++) {
            fprintf(stderr, " %02X", ram[a]);
        }
        fprintf(stderr, "\n");
    }
    for (v = 0; v < 24; v++) {
        volatile unsigned short *r = s + v * 8;
        if (r[6]) {
            fprintf(stderr, "pe:   voice %u: vol %04X/%04X pitch %04X start %04X env %04X\n", v, r[0], r[1], r[2],
                    r[3], r[6]);
        }
    }
    g_keyons = 0;
}

/* The audio thread mixes from PsyZ's SPU state while the game thread
 * changes it here; PsyZ's audio lock (an SDL mutex, which SDL documents as
 * recursive) keeps a mix from seeing half-applied register changes. */
void Psyz_AudioLock(void);
void Psyz_AudioUnlock(void);

static void spu_sync_locked(void);
static void key_watchdog(void);

void pe_host_spu_sync(void) {
    Psyz_AudioLock();
    spu_sync_locked();
    Psyz_AudioUnlock();
}

static void spu_sync_locked(void) {
    volatile unsigned short *s = shadow();
    unsigned int i;
    if (!g_ready) {
        reload();
        return;
    }
    for (i = 0; i < SPU_REGS; i++) {
        unsigned short v = s[i];
        if (v != g_synced[i]) {
            Psyz_SpuWrite(i * 2, v);
            if (i * 2 == 0x188 || i * 2 == 0x18A) {
                unsigned short b;
                for (b = v; b; b &= (unsigned short)(b - 1)) {
                    g_keyons++;
                }
            }
        }
    }
    reload();
    key_watchdog();
    if (getenv("PE_LOG_SPU")) {
        spu_log();
    }
}

/* Spu_WriteKeyOn/Off (host patch of src/main/akao/Akao_SpuVoiceControl.c):
 * reg 0x188/0x18A key on, 0x18C/0x18E key off. */
/* Watchdog for sounds that never end: a voice still sounding 10 s after its
 * key-on (and not keyed off since) is reported once in the log, with its
 * addresses, to find looping effect sounds. Long music notes can show up too.
 * Pitch-0 voices (held silent by the driver) are not reported. The 10 s are
 * the sound driver's own time (2400 ticks of its 240 Hz timer, root counter
 * 2), so fast forward does not report sounds that are simply not done yet. */
unsigned Psyz_KernelTimerTicks(int i);
#define pe_host_frame_number() Psyz_KernelTimerTicks(2)
static unsigned int g_key_frame[24];
static unsigned char g_key_live[24], g_key_reported[24];
static void key_note(unsigned int reg, unsigned short value) {
    unsigned int base = (reg == 0x18A || reg == 0x18E) ? 16 : 0, v;
    for (v = 0; v < 16 && base + v < 24; v++) {
        if (!(value & (1u << v))) {
            continue;
        }
        if (reg == 0x188 || reg == 0x18A) {
            g_key_frame[base + v] = pe_host_frame_number();
            g_key_live[base + v] = 1;
            g_key_reported[base + v] = 0;
        } else {
            g_key_live[base + v] = 0;
        }
    }
}
static void key_watchdog(void) {
    volatile unsigned short *s = shadow();
    unsigned int now = pe_host_frame_number(), v;
    for (v = 0; v < 24; v++) {
        volatile unsigned short *r = s + v * 8;
        if (g_key_live[v] && !g_key_reported[v] && r[6] != 0 && r[2] != 0 && now - g_key_frame[v] > 2400) {
            g_key_reported[v] = 1;
            fprintf(stderr, "pe: sound watchdog: voice %u still playing %u driver ticks (240 a second) after key-on "
                            "(start %05X, loop %05X, pitch %04X, envelope %04X), no key-off\n",
                    v, now - g_key_frame[v], (unsigned)r[3] << 3, (unsigned)r[7] << 3, r[2], r[6]);
        }
    }
}

void pe_host_spu_key(unsigned int reg, unsigned short value) {
    Psyz_AudioLock();
    spu_sync_locked();
    Psyz_SpuWrite(reg, value);
    key_note(reg, value);
    if (reg == 0x188 || reg == 0x18A) {
        unsigned short b;
        for (b = value; b; b &= (unsigned short)(b - 1)) {
            g_keyons++;
        }
    }
    Psyz_AudioUnlock();
}
