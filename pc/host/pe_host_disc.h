#ifndef PE_HOST_DISC_H
#define PE_HOST_DISC_H

/* Your disc image (CUE + single MODE2/2352 BIN). */
int pe_host_disc_open(const char *cue_or_bin);
int pe_host_disc_ok(void);
/* Read `count` 2048-byte data sectors starting at `lba` (0 = first sector). */
int pe_host_disc_read(int lba, void *dst, int count);
/* Read one raw 2352-byte sector. */
int pe_host_disc_read_raw(int lba, void *dst);
void pe_host_disc_shell_cycled(void);
int pe_host_disc_find_root_file(const char *name, unsigned int *lba, unsigned int *size);
void *pe_host_disc_load_file(const char *name, unsigned int *size_out);

#endif
