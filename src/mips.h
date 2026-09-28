#pragma once
#include <stdbool.h>
#include <stdint.h>

#define MIPS_TLB_ENTRIES 32

enum {
    MIPS_EXC_INT  = 0,
    MIPS_EXC_MOD  = 1,
    MIPS_EXC_TLBL = 2,
    MIPS_EXC_TLBS = 3,
    MIPS_EXC_ADEL = 4,
    MIPS_EXC_ADES = 5,
    MIPS_EXC_IBE  = 6,
    MIPS_EXC_DBE  = 7,
    MIPS_EXC_SYS  = 8,
    MIPS_EXC_BP   = 9,
    MIPS_EXC_RI   = 10,
    MIPS_EXC_CPU  = 11,
    MIPS_EXC_OV   = 12,
};

enum {
    CP0_INDEX    = 0,
    CP0_RANDOM   = 1,
    CP0_ENTRYLO  = 2,
    CP0_CONFIG   = 3,
    CP0_CONTEXT  = 4,
    CP0_BADVADDR = 8,
    CP0_ENTRYHI  = 10,
    CP0_STATUS   = 12,
    CP0_CAUSE    = 13,
    CP0_EPC      = 14,
    CP0_PRID     = 15,
};

typedef struct {
    uint32_t vpn;
    uint32_t pid;
    uint32_t pfn;
    bool     global;
    bool     valid;
    bool     dirty;
    bool     noncache;
} mips_tlb_entry_t;

typedef struct mips_cpu mips_cpu_t;

typedef struct {
    void    *context;
    bool   (*read)(void *context, uint32_t pa, int size, uint32_t *value);
    bool   (*write)(void *context, uint32_t pa, int size, uint32_t value);
    uint8_t *(*fetch_page)(void *context, uint32_t pa);
} mips_bus_t;

struct mips_cpu {
    uint32_t gpr[32];
    uint32_t hi, lo;
    uint32_t pc;
    uint32_t next_pc;
    bool     in_delay_slot;
    bool     next_in_delay_slot;
    uint32_t cp0[32];
    uint32_t external_ip;
    mips_tlb_entry_t tlb[MIPS_TLB_ENTRIES];
    uint32_t random_state;
    uint64_t cycles;
    uint32_t speed;
    uint32_t speed_count;
    uint64_t exceptions[16];
    bool     fault;
    bool     yield;
    mips_bus_t bus;
    uint32_t last_fetch_vpn;
    uint8_t *last_fetch_page;
    bool     last_fetch_valid;
};

void mips_reset(mips_cpu_t *cpu, uint32_t entry);
void mips_set_external_ip(mips_cpu_t *cpu, uint32_t ip_bits);
void mips_run(mips_cpu_t *cpu, uint64_t until_cycle);
bool mips_translate(mips_cpu_t *cpu, uint32_t va, bool write, uint32_t *pa);
bool mips_read_virtual(mips_cpu_t *cpu, uint32_t va, int size, uint32_t *value);
