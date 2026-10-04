#pragma once
#include <stdbool.h>
#include <stdint.h>

#define SH3_TLB_SETS 32
#define SH3_TLB_WAYS 4

#define SH3_SR_T     (1u << 0)
#define SH3_SR_S     (1u << 1)
#define SH3_SR_IMASK (15u << 4)
#define SH3_SR_Q     (1u << 8)
#define SH3_SR_M     (1u << 9)
#define SH3_SR_BL    (1u << 28)
#define SH3_SR_RB    (1u << 29)
#define SH3_SR_MD    (1u << 30)
#define SH3_SR_MASK  0x700003F3u

#define SH3_MMUCR_AT (1u << 0)
#define SH3_MMUCR_IX (1u << 1)
#define SH3_MMUCR_TF (1u << 2)
#define SH3_MMUCR_RC (3u << 6)
#define SH3_MMUCR_SV (1u << 8)

enum {
    SH3_EXP_POWER_ON      = 0x000,
    SH3_EXP_MANUAL_RESET  = 0x020,
    SH3_EXP_TLB_MISS_READ = 0x040,
    SH3_EXP_TLB_MISS_WRITE = 0x060,
    SH3_EXP_INITIAL_WRITE = 0x080,
    SH3_EXP_PROTECT_READ  = 0x0A0,
    SH3_EXP_PROTECT_WRITE = 0x0C0,
    SH3_EXP_ADDRESS_READ  = 0x0E0,
    SH3_EXP_ADDRESS_WRITE = 0x100,
    SH3_EXP_TRAPA         = 0x160,
    SH3_EXP_ILLEGAL       = 0x180,
    SH3_EXP_SLOT_ILLEGAL  = 0x1A0,
    SH3_EXP_USER_BREAK    = 0x1E0,
};

typedef struct {
    uint32_t vpn;
    uint32_t ppn;
    uint8_t  asid;
    uint8_t  protection;
    bool     valid;
    bool     dirty;
    bool     cacheable;
    bool     shared;
    bool     large;
} sh3_tlb_entry_t;

typedef struct {
    void    *context;
    bool   (*read)(void *context, uint32_t pa, int size, uint32_t *value);
    bool   (*write)(void *context, uint32_t pa, int size, uint32_t value);
    uint8_t *(*fetch_page)(void *context, uint32_t pa);
    uint8_t  *dram;
    uint32_t  dram_base;
    uint32_t  dram_size;
} sh3_bus_t;

#define SH3_WATCH_MAX 8
#define SH3_PAGE_CACHE 256
#define SH3_FETCH_CACHE 64

typedef struct {
    uint32_t tag;
    uint32_t pa;
} sh3_page_cache_t;

typedef struct {
    uint32_t tag;
    uint8_t *block;
} sh3_fetch_cache_t;

typedef struct {
    void    *context;
    bool   (*before)(void *context, uint32_t pc);
    bool   (*access)(void *context, uint32_t va, int size, bool write);
    void   (*exception)(void *context, uint32_t code, uint32_t pc, bool user);
    uint32_t filter[128];
    uint32_t pc;
    bool     every;
    bool     data;
    bool     stop;
    bool     undo;
} sh3_debug_t;

typedef struct sh3_cpu sh3_cpu_t;

struct sh3_cpu {
    uint32_t r[16];
    uint32_t bank[8];
    uint32_t sr, gbr, vbr, ssr, spc;
    uint32_t mach, macl, pr, pc;
    uint32_t pteh, ptel, ttb, tea, mmucr;
    uint32_t expevt, intevt, tra;
    sh3_tlb_entry_t tlb[SH3_TLB_SETS][SH3_TLB_WAYS];
    uint32_t replacement;
    uint32_t interrupt_level;
    uint32_t interrupt_code;
    bool     sleeping;
    bool     in_slot;
    uint64_t cycles;
    uint32_t speed;
    uint32_t speed_count;
    uint64_t exceptions[64];
    bool     fault;
    bool     yield;
    sh3_bus_t bus;
    sh3_page_cache_t read_cache[SH3_PAGE_CACHE];
    sh3_page_cache_t write_cache[SH3_PAGE_CACHE];
    sh3_fetch_cache_t fetch_cache[SH3_FETCH_CACHE];
    uint32_t watch[SH3_WATCH_MAX];
    int      watch_count;
    uint32_t watch_filter[128];
    void   (*on_watch)(void *context, uint32_t pc);
    void   (*on_interrupt)(void *context, uint32_t code);
    sh3_debug_t *debug;
    bool   (*on_trapa)(void *context, uint32_t number);
};

void sh3_reset(sh3_cpu_t *cpu);
void sh3_run(sh3_cpu_t *cpu, uint64_t until_cycle);
void sh3_set_interrupt(sh3_cpu_t *cpu, uint32_t level, uint32_t code);
void sh3_set_sr(sh3_cpu_t *cpu, uint32_t value);
void sh3_raise_memory_fault(sh3_cpu_t *cpu, uint32_t va, bool write);
bool sh3_translate(sh3_cpu_t *cpu, uint32_t va, bool write, uint32_t *pa);
void sh3_flush_translations(sh3_cpu_t *cpu);
bool sh3_user_mode(const sh3_cpu_t *cpu);
uint32_t sh3_asid(const sh3_cpu_t *cpu);
uint32_t sh3_register(const sh3_cpu_t *cpu, int index);
bool sh3_control_read(sh3_cpu_t *cpu, uint32_t address, uint32_t *value);
bool sh3_control_write(sh3_cpu_t *cpu, uint32_t address, uint32_t value);
void sh3_debug_filter_add(sh3_debug_t *debug, uint32_t va);
int  sh3_disassemble(uint32_t pc, uint16_t op, char *out, int size);
