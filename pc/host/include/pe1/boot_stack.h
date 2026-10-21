#ifndef PE1_BOOT_STACK_H
#define PE1_BOOT_STACK_H

/* Host replacement for include/pe1/boot_stack.h.
 *
 * Retail moves $sp to the top of the 1 KiB scratchpad for one call (the
 * boot loop's func_8019234C) and stores the old $sp in the scratchpad's top
 * word. The host keeps its own stack: the call runs on it unchanged. */

#define BOOT_SCRATCHPAD_STACK_TOP ((u32 *)0x1F8003FC)

#define BOOT_CALL_ON_SCRATCHPAD_STACK(top, call) \
    { \
        (void)(top); \
        call; \
    }

#endif
