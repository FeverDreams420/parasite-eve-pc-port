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

void pe_host_spu_sync(void) {
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
        }
    }
    reload();
}
