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

static void pad_vsync(void) {
    int i;
    for (i = 0; i < 2; i++) {
        if (g_pad_buf[i]) {
            Psyz_PadsGet(i, (char *)g_pad_buf[i], PAD_BUF_LEN);
        }
    }
    if (g_prev_cb) {
        g_prev_cb();
    }
}

void pe_psyq_PadInitDirect(unsigned char *p1, unsigned char *p2) {
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
