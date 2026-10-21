#ifndef PE_HOST_TRAPS_H
#define PE_HOST_TRAPS_H

typedef struct {
    unsigned int mips_addr;
    const char *name;
} PeHostTrap;

extern const PeHostTrap pe_psyq_trap_table[];
extern const unsigned int pe_psyq_trap_count;

#endif
