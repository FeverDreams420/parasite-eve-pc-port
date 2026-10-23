/* PSY-Q libpad (PadInitDirect family) on PsyZ's controller input.
 *
 * PadInitDirect registers two receive buffers. On hardware libpad refreshes
 * them every VBlank in the PS1 controller format (byte 0 status, 0 = ok;
 * byte 1 type, 0x41 digital / 0x73 analog; bytes 2-3 buttons, active low;
 * analog axes after). PsyZ's Psyz_PadsGet produces that format, so it is
 * copied into the game's buffers from PsyZ's VSync hook.
 *
 * Vibration is not supported yet, so PadGetState reports 2
 * (PadStateFindCTP1: controller without vibration, see PsyZ libetc.h).
 */
#include <psyz.h>
#include <string.h>

void Psyz_PadsGet(int port, char *dst, int len);
typedef void (*PsyzVSyncCb)(void);
PsyzVSyncCb Psyz_SetVSyncCb(PsyzVSyncCb cb);

#define PAD_BUF_LEN 0x22 /* the game passes buffers 0x22 bytes apart */

static unsigned char *g_pad_buf[2];
static PsyzVSyncCb g_prev_cb;

/* Debug: PE_PAD_SCRIPT="frame:mask,frame:mask,..." holds buttons on port 1
 * for 6 frames from each frame number, or over "from-to:mask" (game frames: VSync calls, the same
 * numbering as PE_CAPTURE_DIR frame files).
 * mask uses the controller's button bits: low byte = byte 2 (Select 0x01,
 * L3 0x02, R3 0x04, Start 0x08, Up 0x10, Right 0x20, Down 0x40, Left 0x80),
 * high byte = byte 3 (L2 0x100, R2 0x200, L1 0x400, R1 0x800, Triangle
 * 0x1000, Circle 0x2000, Cross 0x4000, Square 0x8000). */
#include <stdio.h>
#include <stdlib.h>
unsigned int pe_host_frame_number(void);
static unsigned int g_pad_frame;
static unsigned int script_mask(unsigned int frame) {
    static const char *script;
    static int init;
    const char *p;
    unsigned int m = 0;
    if (!init) {
        init = 1;
        script = getenv("PE_PAD_SCRIPT");
    }
    for (p = script; p && *p;) {
        char *e;
        unsigned long f = strtoul(p, &e, 10), mask = 0, to = f + 6;
        if (*e == '-') {
            to = strtoul(e + 1, &e, 10) + 1;
        }
        if (*e == ':') {
            mask = strtoul(e + 1, &e, 0);
        }
        if (frame >= f && frame < to) {
            m |= (unsigned int)mask;
        }
        p = (*e == ',') ? e + 1 : NULL;
    }
    return m;
}

static void pad_vsync(void) {
    int i;
    unsigned int m;
    for (i = 0; i < 2; i++) {
        if (g_pad_buf[i]) {
            Psyz_PadsGet(i, (char *)g_pad_buf[i], PAD_BUF_LEN);
        }
    }
    g_pad_frame = pe_host_frame_number();
    if (getenv("PE_LOG_PAD") && g_pad_buf[0] && script_mask(g_pad_frame)) {
        fprintf(stderr, "pe: pad frame %u: %02X %02X %02X %02X  game buttons 0x%08X\n", g_pad_frame,
                g_pad_buf[0][0], g_pad_buf[0][1], g_pad_buf[0][2], g_pad_buf[0][3],
                *(volatile unsigned int *)0x8009D26Cu);
    }
    m = script_mask(g_pad_frame);
    if (g_pad_buf[0] && m != 0) {
        g_pad_buf[0][0] = 0;
        if (g_pad_buf[0][1] == 0 || g_pad_buf[0][1] == 0xFF) {
            g_pad_buf[0][1] = 0x41;
        }
        g_pad_buf[0][2] &= (unsigned char)~m;
        g_pad_buf[0][3] &= (unsigned char)~(m >> 8);
    }
    if (g_prev_cb) {
        g_prev_cb();
    }
}

void PadInit(int mode);

void pe_psyq_PadInitDirect(unsigned char *p1, unsigned char *p2) {
    PadInit(0); /* PsyZ reads keyboard and gamepads only after this */
    g_pad_buf[0] = p1;
    g_pad_buf[1] = p2;
    g_prev_cb = Psyz_SetVSyncCb(pad_vsync);
    pad_vsync();
}

void pe_psyq_PadStartCom(void) {}
int pe_psyq_PadGetState(int port) { return (port & 0xF0) == 0 ? 2 : 0; }
int pe_psyq_PadInfoMode(int port, int term, int offs) { (void)port; (void)term; (void)offs; return 0; }
int pe_psyq_PadSetMainMode(int port, int offs, int lock) { (void)port; (void)offs; (void)lock; return 0; }
void pe_psyq_PadSetAct(int port, unsigned char *data, int len) { (void)port; (void)data; (void)len; }
int pe_psyq_PadSetActAlign(int port, unsigned char *data) { (void)port; (void)data; return 1; }
