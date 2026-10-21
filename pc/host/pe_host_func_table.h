#ifndef PE_HOST_FUNC_TABLE_H
#define PE_HOST_FUNC_TABLE_H

/* One retail MIPS function address and the host function compiled from it. */
typedef struct {
    unsigned int mips_addr;
    void *host;
} PeHostFunc;

extern const PeHostFunc pe_main_func_table[];
extern const unsigned int pe_main_func_count;
/* tools/pc/gen_data_code.py: code the decomp keeps as data words */
extern const PeHostFunc pe_data_code_table[];
extern const unsigned int pe_data_code_count;

#endif
