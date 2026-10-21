/* Host C for the five game functions the decomp keeps as hand-written MIPS
 * (GAME_ASM_FUNCTION in include/pe1/game_asm.h). Each is a line-for-line
 * reading of the instructions quoted in its source file:
 *   src/main/math/math_fixed.c        Math_FixedRoundToInt, Math_FixedRoundToByte
 *   src/main/math/Math_FixedMul.c     Math_FixedMul
 *   src/main/math/Math_SqrtApprox3.c  Math_SqrtApprox3
 *   src/main/task/Task_GpuPackPrimColor.c  Task_GpuPackPrimColor
 * Retail uses the trapping `add`/`sub`; on overflow the PS1 would raise an
 * exception. The host wraps instead (the game is not expected to overflow
 * here; nothing in the sources handles that exception).
 */
#include <stdint.h>

unsigned int Task_GpuFlushPrimQueue(void);

/* ori $at,$zero,0x8000 ; add $v0,$a0,$at ; sra $v0,$v0,16 */
int Math_FixedRoundToInt(int a0) {
    return (int)((uint32_t)a0 + 0x8000u) >> 16;
}

/* same, sra 8 */
int Math_FixedRoundToByte(int a0) {
    return (int)((uint32_t)a0 + 0x8000u) >> 8;
}

/* mult a0,a1 ; v0 = (LO >> 16) | (HI << 16) */
int Math_FixedMul(int a0, int a1) {
    int64_t p = (int64_t)a0 * (int64_t)a1;
    uint32_t lo = (uint32_t)p;
    uint32_t hi = (uint32_t)((uint64_t)p >> 32);
    return (int)((lo >> 16) | (hi << 16));
}

/* |x|,|y|,|z| (negu), three signed mults, 64-bit add with carry chain,
 * result = (sum_lo >> 20) | (sum_hi << 12). */
unsigned int Math_SqrtApprox3(int a0, int a1, int a2) {
    uint32_t x = (uint32_t)a0, y = (uint32_t)a1, z = (uint32_t)a2;
    uint64_t px, py, pz, sum;
    if (a0 < 0) x = 0u - x;
    if (a1 < 0) y = 0u - y;
    if (a2 < 0) z = 0u - z;
    px = (uint64_t)((int64_t)(int32_t)x * (int64_t)(int32_t)x);
    py = (uint64_t)((int64_t)(int32_t)y * (int64_t)(int32_t)y);
    pz = (uint64_t)((int64_t)(int32_t)z * (int64_t)(int32_t)z);
    sum = px + py + pz;
    return ((uint32_t)sum >> 20) | ((uint32_t)(sum >> 32) << 12);
}

/* jal Task_GpuFlushPrimQueue with a1 = high - low (delay slot);
 * that function leaves $a0/$a1 untouched (it only uses $8-$15 and $24).
 * v0 = ((v0 & 0xFFFF) * a1) -> (LO >> 16) | (HI << 16) ; return v0 + low. */
int Task_GpuPackPrimColor(int low, int high) {
    uint32_t range = (uint32_t)high - (uint32_t)low;
    uint32_t r = Task_GpuFlushPrimQueue() & 0xFFFFu;
    int64_t p = (int64_t)(int32_t)r * (int64_t)(int32_t)range;
    uint32_t lo = (uint32_t)p;
    uint32_t hi = (uint32_t)((uint64_t)p >> 32);
    return (int)(((lo >> 16) | (hi << 16)) + (uint32_t)low);
}
