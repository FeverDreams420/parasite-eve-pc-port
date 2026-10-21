#ifndef PE_HOST_OVERLAYS_H
#define PE_HOST_OVERLAYS_H

#include "pe_host_func_table.h"

typedef struct {
    unsigned int file_off; /* offset of the function in the overlay binary */
    unsigned int size;     /* bytes */
} PeHostOverlayFunc;

typedef struct {
    const char *name;
    unsigned int vram;      /* load address */
    unsigned int size;      /* bytes */
    int pe_img_offset;      /* byte offset inside Disc 1 PE.IMG, -1 if unknown */
    unsigned char sha1[20]; /* of the retail overlay binary */
    const PeHostFunc *funcs;
    const PeHostOverlayFunc *meta; /* parallel to funcs */
    unsigned int func_count;
    int fragmented; /* pieces linked at unrelated addresses */
} PeHostOverlay;

extern const PeHostOverlay pe_overlay_table[];
extern const unsigned int pe_overlay_count;

/* Read every overlay from Disc 1's PE.IMG, check its SHA-1, keep a
 * fingerprint for recognising it in RAM later. */
void pe_host_overlays_init(void);
/* The game wrote [addr, addr+len) (a CD read): install any overlay that is
 * now complete in RAM. */
void pe_host_overlays_note_write(unsigned int addr, unsigned int len);
/* The same for a CD read of `len` bytes from disc sector `disc_lba`. */
void pe_host_overlays_note_read(unsigned int addr, unsigned int len, int disc_lba);

#endif
