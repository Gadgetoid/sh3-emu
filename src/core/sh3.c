#include "core/sh3.h"

#include <string.h>

#define VECTOR_GENERAL   0x100u
#define VECTOR_TLB_MISS  0x400u
#define VECTOR_INTERRUPT 0x600u
#define RESET_VECTOR     0xA0000000u

#define PTEH_ADDRESS   0xFFFFFFF0u
#define PTEL_ADDRESS   0xFFFFFFF4u
#define TTB_ADDRESS    0xFFFFFFF8u
#define TEA_ADDRESS    0xFFFFFFFCu
#define MMUCR_ADDRESS  0xFFFFFFE0u
#define TRA_ADDRESS    0xFFFFFFD0u
#define EXPEVT_ADDRESS 0xFFFFFFD4u
#define INTEVT_ADDRESS 0xFFFFFFD8u

#define TLB_ADDRESS_ARRAY 0xF2000000u
#define TLB_DATA_ARRAY    0xF3000000u
#define CONTROL_SPACE     0xF0000000u

#define PTEL_SH (1u << 1)
#define PTEL_D  (1u << 2)
#define PTEL_C  (1u << 3)
#define PTEL_SZ (1u << 4)
#define PTEL_PR_SHIFT 5
#define PTEL_V  (1u << 8)
#define PTEL_PPN_MASK 0x1FFFFC00u

typedef enum {
    TRANSLATE_OK,
    TRANSLATE_ADDRESS,
    TRANSLATE_MISS,
    TRANSLATE_INVALID,
    TRANSLATE_PROTECTION,
    TRANSLATE_INITIAL_WRITE,
} translate_result_t;

static uint32_t current_pc;
static uint32_t slot_branch_pc;

static int bank_of(uint32_t sr) {
    return (sr & SH3_SR_MD) && (sr & SH3_SR_RB) ? 1 : 0;
}

void sh3_set_sr(sh3_cpu_t *cpu, uint32_t value) {
    value &= SH3_SR_MASK;
    if (bank_of(value) != bank_of(cpu->sr)) {
        for (int i = 0; i < 8; i++) {
            uint32_t swapped = cpu->r[i];
            cpu->r[i] = cpu->bank[i];
            cpu->bank[i] = swapped;
        }
    }
    cpu->sr = value;
}

void sh3_flush_translations(sh3_cpu_t *cpu) {
    memset(cpu->read_cache, 0, sizeof cpu->read_cache);
    memset(cpu->write_cache, 0, sizeof cpu->write_cache);
    memset(cpu->fetch_cache, 0, sizeof cpu->fetch_cache);
}

void sh3_reset(sh3_cpu_t *cpu) {
    sh3_bus_t bus = cpu->bus;
    uint32_t speed = cpu->speed;
    sh3_debug_t *debug = cpu->debug;
    bool (*on_trapa)(void *, uint32_t) = cpu->on_trapa;
    bool (*on_watch)(void *, uint32_t) = cpu->on_watch;
    void (*on_interrupt)(void *, uint32_t) = cpu->on_interrupt;
    memset(cpu, 0, sizeof *cpu);
    cpu->bus = bus;
    cpu->debug = debug;
    cpu->on_trapa = on_trapa;
    cpu->on_watch = on_watch;
    cpu->on_interrupt = on_interrupt;
    cpu->speed = speed ? speed : 1;
    cpu->sr = SH3_SR_MD | SH3_SR_RB | SH3_SR_BL | SH3_SR_IMASK;
    cpu->pc = RESET_VECTOR;
    cpu->expevt = SH3_EXP_POWER_ON;
}

void sh3_set_interrupt(sh3_cpu_t *cpu, uint32_t level, uint32_t code) {
    cpu->interrupt_level = level;
    cpu->interrupt_code = code;
}

bool sh3_user_mode(const sh3_cpu_t *cpu) {
    return (cpu->sr & SH3_SR_MD) == 0;
}

uint32_t sh3_asid(const sh3_cpu_t *cpu) {
    return cpu->pteh & 0xFFu;
}

uint32_t sh3_register(const sh3_cpu_t *cpu, int index) {
    return cpu->r[index & 15];
}

static uint32_t tlb_set(const sh3_cpu_t *cpu, uint32_t va, uint32_t asid) {
    uint32_t set = (va >> 12) & (SH3_TLB_SETS - 1);
    if (cpu->mmucr & SH3_MMUCR_IX) set ^= asid & (SH3_TLB_SETS - 1);
    return set;
}

static sh3_tlb_entry_t *tlb_lookup(sh3_cpu_t *cpu, uint32_t va, bool privileged) {
    uint32_t asid = sh3_asid(cpu);
    bool ignore_asid = privileged && (cpu->mmucr & SH3_MMUCR_SV);
    sh3_tlb_entry_t *ways = cpu->tlb[tlb_set(cpu, va, asid)];
    for (int way = 0; way < SH3_TLB_WAYS; way++) {
        sh3_tlb_entry_t *entry = &ways[way];
        uint32_t mask = entry->large ? 0xFFFFF000u : 0xFFFFFC00u;
        if ((va ^ entry->vpn) & mask) continue;
        if (!entry->shared && !ignore_asid && entry->asid != asid) continue;
        return entry;
    }
    return NULL;
}

static void load_tlb(sh3_cpu_t *cpu) {
    uint32_t way = (cpu->mmucr & SH3_MMUCR_RC) >> 6;
    uint32_t asid = sh3_asid(cpu);
    sh3_tlb_entry_t *entry = &cpu->tlb[tlb_set(cpu, cpu->pteh, asid)][way];
    entry->vpn = cpu->pteh & 0xFFFFFC00u;
    entry->asid = (uint8_t)asid;
    entry->ppn = cpu->ptel & PTEL_PPN_MASK;
    entry->protection = (cpu->ptel >> PTEL_PR_SHIFT) & 3;
    entry->valid = (cpu->ptel & PTEL_V) != 0;
    entry->dirty = (cpu->ptel & PTEL_D) != 0;
    entry->cacheable = (cpu->ptel & PTEL_C) != 0;
    entry->shared = (cpu->ptel & PTEL_SH) != 0;
    entry->large = (cpu->ptel & PTEL_SZ) != 0;
    sh3_flush_translations(cpu);
}

static translate_result_t translate(sh3_cpu_t *cpu, uint32_t va, bool write, uint32_t *pa) {
    bool privileged = (cpu->sr & SH3_SR_MD) != 0;
    if (va >= 0x80000000u) {
        if (!privileged) return TRANSLATE_ADDRESS;
        if (va < 0xC0000000u) { *pa = va & 0x1FFFFFFFu; return TRANSLATE_OK; }
        if (va >= 0xE0000000u) { *pa = va; return TRANSLATE_OK; }
    }
    if (!(cpu->mmucr & SH3_MMUCR_AT)) { *pa = va & 0x1FFFFFFFu; return TRANSLATE_OK; }
    sh3_tlb_entry_t *entry = tlb_lookup(cpu, va, privileged);
    if (!entry) return TRANSLATE_MISS;
    if (!entry->valid) return TRANSLATE_INVALID;
    switch (entry->protection) {
    case 0: if (!privileged || write) return TRANSLATE_PROTECTION; break;
    case 1: if (!privileged) return TRANSLATE_PROTECTION; break;
    case 2: if (write) return TRANSLATE_PROTECTION; break;
    default: break;
    }
    if (write && !entry->dirty) return TRANSLATE_INITIAL_WRITE;
    uint32_t offset_mask = entry->large ? 0xFFFu : 0x3FFu;
    *pa = (entry->ppn & ~offset_mask) | (va & offset_mask);
    return TRANSLATE_OK;
}

bool sh3_translate(sh3_cpu_t *cpu, uint32_t va, bool write, uint32_t *pa) {
    return translate(cpu, va, write, pa) == TRANSLATE_OK;
}

static void take_exception(sh3_cpu_t *cpu, uint32_t code, uint32_t vector, uint32_t return_pc) {
    cpu->exceptions[(code >> 5) & 63]++;
    if (cpu->debug && cpu->debug->exception && code != SH3_EXP_TRAPA)
        cpu->debug->exception(cpu->debug->context, code, return_pc, sh3_user_mode(cpu));
    cpu->fault = true;
    cpu->sleeping = false;
    if (cpu->sr & SH3_SR_BL) {
        cpu->expevt = SH3_EXP_MANUAL_RESET;
        sh3_set_sr(cpu, SH3_SR_MD | SH3_SR_RB | SH3_SR_BL | SH3_SR_IMASK);
        cpu->pc = RESET_VECTOR;
        return;
    }
    cpu->spc = return_pc;
    cpu->ssr = cpu->sr;
    cpu->expevt = code;
    sh3_set_sr(cpu, cpu->sr | SH3_SR_MD | SH3_SR_RB | SH3_SR_BL);
    cpu->pc = cpu->vbr + vector;
}

static uint32_t faulting_pc(const sh3_cpu_t *cpu) {
    return cpu->in_slot ? slot_branch_pc : current_pc;
}

static void general_exception(sh3_cpu_t *cpu, uint32_t code) {
    take_exception(cpu, code, VECTOR_GENERAL, faulting_pc(cpu));
}

static void illegal_instruction(sh3_cpu_t *cpu) {
    general_exception(cpu, cpu->in_slot ? SH3_EXP_SLOT_ILLEGAL : SH3_EXP_ILLEGAL);
}

static void choose_replacement(sh3_cpu_t *cpu, uint32_t va) {
    uint32_t asid = sh3_asid(cpu);
    sh3_tlb_entry_t *ways = cpu->tlb[tlb_set(cpu, va, asid)];
    sh3_tlb_entry_t *match = tlb_lookup(cpu, va, (cpu->sr & SH3_SR_MD) != 0);
    uint32_t way = SH3_TLB_WAYS;
    if (match) way = (uint32_t)(match - ways);
    for (uint32_t i = 0; way == SH3_TLB_WAYS && i < SH3_TLB_WAYS; i++)
        if (!ways[i].valid) way = i;
    if (way == SH3_TLB_WAYS) way = (uint32_t)(cpu->replacement++ % SH3_TLB_WAYS);
    cpu->mmucr = (cpu->mmucr & ~SH3_MMUCR_RC) | (way << 6);
}

static void memory_fault(sh3_cpu_t *cpu, translate_result_t result, uint32_t va, bool write) {
    cpu->tea = va;
    if (result != TRANSLATE_ADDRESS) cpu->pteh = (va & 0xFFFFFC00u) | (cpu->pteh & 0xFFu);
    if (result == TRANSLATE_MISS || result == TRANSLATE_INVALID) choose_replacement(cpu, va);
    uint32_t pc = faulting_pc(cpu);
    switch (result) {
    case TRANSLATE_ADDRESS:
        take_exception(cpu, write ? SH3_EXP_ADDRESS_WRITE : SH3_EXP_ADDRESS_READ, VECTOR_GENERAL, pc);
        break;
    case TRANSLATE_MISS:
        take_exception(cpu, write ? SH3_EXP_TLB_MISS_WRITE : SH3_EXP_TLB_MISS_READ, VECTOR_TLB_MISS, pc);
        break;
    case TRANSLATE_INVALID:
        take_exception(cpu, write ? SH3_EXP_TLB_MISS_WRITE : SH3_EXP_TLB_MISS_READ, VECTOR_GENERAL, pc);
        break;
    case TRANSLATE_PROTECTION:
        take_exception(cpu, write ? SH3_EXP_PROTECT_WRITE : SH3_EXP_PROTECT_READ, VECTOR_GENERAL, pc);
        break;
    case TRANSLATE_INITIAL_WRITE:
        take_exception(cpu, SH3_EXP_INITIAL_WRITE, VECTOR_GENERAL, pc);
        break;
    default:
        break;
    }
}

void sh3_raise_memory_fault(sh3_cpu_t *cpu, uint32_t va, bool write) {
    uint32_t pa;
    translate_result_t result = translate(cpu, va, write, &pa);
    if (result == TRANSLATE_OK) result = TRANSLATE_MISS;
    memory_fault(cpu, result, va, write);
}

static uint32_t cache_tag(const sh3_cpu_t *cpu, uint32_t va) {
    return (va & 0xFFFFFC00u) | (sh3_asid(cpu) << 2) | (sh3_user_mode(cpu) ? 2u : 0u) | 1u;
}

static bool translate_cached(sh3_cpu_t *cpu, sh3_page_cache_t *cache, uint32_t va, bool write, uint32_t *pa) {
    uint32_t tag = cache_tag(cpu, va);
    sh3_page_cache_t *slot = &cache[(va >> 10) & (SH3_PAGE_CACHE - 1)];
    if (slot->tag == tag) {
        *pa = slot->pa | (va & 0x3FFu);
        return true;
    }
    translate_result_t result = translate(cpu, va, write, pa);
    if (result != TRANSLATE_OK) {
        memory_fault(cpu, result, va, write);
        return false;
    }
    slot->tag = tag;
    slot->pa = *pa & ~0x3FFu;
    return true;
}

static uint32_t read_host(const uint8_t *base, int size) {
    if (size == 4) return (uint32_t)base[0] | (uint32_t)base[1] << 8 | (uint32_t)base[2] << 16 | (uint32_t)base[3] << 24;
    if (size == 2) return (uint32_t)base[0] | (uint32_t)base[1] << 8;
    return base[0];
}

static void write_host(uint8_t *base, int size, uint32_t value) {
    base[0] = (uint8_t)value;
    if (size == 1) return;
    base[1] = (uint8_t)(value >> 8);
    if (size == 2) return;
    base[2] = (uint8_t)(value >> 16);
    base[3] = (uint8_t)(value >> 24);
}

static uint32_t tlb_address_field(const sh3_tlb_entry_t *entry) {
    return (entry->vpn & 0xFFFE0C00u) | (entry->valid ? PTEL_V : 0) | entry->asid;
}

static uint32_t tlb_data_field(const sh3_tlb_entry_t *entry) {
    return entry->ppn | (entry->valid ? PTEL_V : 0) | ((uint32_t)entry->protection << PTEL_PR_SHIFT)
           | (entry->large ? PTEL_SZ : 0) | (entry->cacheable ? PTEL_C : 0)
           | (entry->dirty ? PTEL_D : 0) | (entry->shared ? PTEL_SH : 0);
}

bool sh3_control_read(sh3_cpu_t *cpu, uint32_t address, uint32_t *value) {
    if (address < TLB_ADDRESS_ARRAY) {
        *value = 0;
        return true;
    }
    if ((address & 0xFF000000u) == TLB_ADDRESS_ARRAY || (address & 0xFF000000u) == TLB_DATA_ARRAY) {
        const sh3_tlb_entry_t *entry = &cpu->tlb[(address >> 12) & 31][(address >> 8) & 3];
        *value = (address & 0xFF000000u) == TLB_ADDRESS_ARRAY ? tlb_address_field(entry) : tlb_data_field(entry);
        return true;
    }
    switch (address & ~3u) {
    case PTEH_ADDRESS:   *value = cpu->pteh; return true;
    case PTEL_ADDRESS:   *value = cpu->ptel; return true;
    case TTB_ADDRESS:    *value = cpu->ttb; return true;
    case TEA_ADDRESS:    *value = cpu->tea; return true;
    case MMUCR_ADDRESS:  *value = cpu->mmucr; return true;
    case TRA_ADDRESS:    *value = cpu->tra; return true;
    case EXPEVT_ADDRESS: *value = cpu->expevt; return true;
    case INTEVT_ADDRESS: *value = cpu->intevt; return true;
    default: return false;
    }
}

bool sh3_control_write(sh3_cpu_t *cpu, uint32_t address, uint32_t value) {
    if (address < TLB_ADDRESS_ARRAY) return true;
    if ((address & 0xFF000000u) == TLB_ADDRESS_ARRAY) {
        uint32_t set = (address >> 12) & 31;
        if (address & 0x80u) {
            for (int way = 0; way < SH3_TLB_WAYS; way++) {
                sh3_tlb_entry_t *entry = &cpu->tlb[set][way];
                uint32_t mask = entry->large ? 0xFFFE0000u : 0xFFFE0C00u;
                if ((entry->vpn ^ value) & mask) continue;
                if (!entry->shared && entry->asid != (value & 0xFFu)) continue;
                entry->valid = (value & PTEL_V) != 0;
            }
        } else {
            sh3_tlb_entry_t *entry = &cpu->tlb[set][(address >> 8) & 3];
            entry->vpn = (value & 0xFFFE0C00u) | (set << 12);
            entry->valid = (value & PTEL_V) != 0;
            entry->asid = (uint8_t)value;
        }
        sh3_flush_translations(cpu);
        return true;
    }
    if ((address & 0xFF000000u) == TLB_DATA_ARRAY) {
        sh3_tlb_entry_t *entry = &cpu->tlb[(address >> 12) & 31][(address >> 8) & 3];
        entry->ppn = value & PTEL_PPN_MASK;
        entry->valid = (value & PTEL_V) != 0;
        entry->protection = (value >> PTEL_PR_SHIFT) & 3;
        entry->large = (value & PTEL_SZ) != 0;
        entry->cacheable = (value & PTEL_C) != 0;
        entry->dirty = (value & PTEL_D) != 0;
        entry->shared = (value & PTEL_SH) != 0;
        sh3_flush_translations(cpu);
        return true;
    }
    switch (address & ~3u) {
    case PTEH_ADDRESS: cpu->pteh = value & 0xFFFFFCFFu; return true;
    case PTEL_ADDRESS: cpu->ptel = value & 0x1FFFFD7Eu; return true;
    case TTB_ADDRESS:  cpu->ttb = value; return true;
    case TEA_ADDRESS:  cpu->tea = value; return true;
    case MMUCR_ADDRESS:
        if (value & SH3_MMUCR_TF) {
            for (int set = 0; set < SH3_TLB_SETS; set++)
                for (int way = 0; way < SH3_TLB_WAYS; way++) cpu->tlb[set][way].valid = false;
        }
        cpu->mmucr = value & (SH3_MMUCR_AT | SH3_MMUCR_IX | SH3_MMUCR_RC | SH3_MMUCR_SV);
        sh3_flush_translations(cpu);
        return true;
    case TRA_ADDRESS:    cpu->tra = value & 0x3FCu; return true;
    case EXPEVT_ADDRESS: cpu->expevt = value & 0xFFFu; return true;
    case INTEVT_ADDRESS: cpu->intevt = value & 0xFFFu; return true;
    default: return false;
    }
}

static bool physical_read(sh3_cpu_t *cpu, uint32_t pa, int size, uint32_t *value) {
    if (pa - cpu->bus.dram_base < cpu->bus.dram_size) {
        *value = read_host(cpu->bus.dram + (pa - cpu->bus.dram_base), size);
        return true;
    }
    if (pa >= CONTROL_SPACE && sh3_control_read(cpu, pa, value)) {
        if (size == 2) *value = (pa & 2) ? *value >> 16 : *value & 0xFFFFu;
        else if (size == 1) *value = (*value >> ((pa & 3) * 8)) & 0xFFu;
        return true;
    }
    if (!cpu->bus.read(cpu->bus.context, pa, size, value)) *value = 0;
    return true;
}

static void physical_write(sh3_cpu_t *cpu, uint32_t pa, int size, uint32_t value) {
    if (pa - cpu->bus.dram_base < cpu->bus.dram_size) {
        write_host(cpu->bus.dram + (pa - cpu->bus.dram_base), size, value);
        return;
    }
    if (pa >= CONTROL_SPACE && (size == 4 || pa < TLB_ADDRESS_ARRAY) && sh3_control_write(cpu, pa, value)) return;
    cpu->bus.write(cpu->bus.context, pa, size, value);
}

static bool debug_access(sh3_cpu_t *cpu, uint32_t va, int size, bool write) {
    if (!cpu->debug || !cpu->debug->data) return false;
    if (!cpu->debug->access(cpu->debug->context, va, size, write)) return false;
    cpu->debug->stop = true;
    cpu->debug->undo = true;
    cpu->fault = true;
    return true;
}

static bool load(sh3_cpu_t *cpu, uint32_t va, int size, uint32_t *value) {
    if (va & (uint32_t)(size - 1)) {
        memory_fault(cpu, TRANSLATE_ADDRESS, va, false);
        return false;
    }
    uint32_t pa;
    if (!translate_cached(cpu, cpu->read_cache, va, false, &pa)) return false;
    if (debug_access(cpu, va, size, false)) return false;
    return physical_read(cpu, pa, size, value);
}

static bool store(sh3_cpu_t *cpu, uint32_t va, int size, uint32_t value) {
    if (va & (uint32_t)(size - 1)) {
        memory_fault(cpu, TRANSLATE_ADDRESS, va, true);
        return false;
    }
    uint32_t pa;
    if (!translate_cached(cpu, cpu->write_cache, va, true, &pa)) return false;
    if (debug_access(cpu, va, size, true)) return false;
    physical_write(cpu, pa, size, value);
    return true;
}

static bool fetch(sh3_cpu_t *cpu, uint32_t va, uint16_t *op) {
    if (va & 1) {
        memory_fault(cpu, TRANSLATE_ADDRESS, va, false);
        return false;
    }
    uint32_t tag = cache_tag(cpu, va);
    sh3_fetch_cache_t *slot = &cpu->fetch_cache[(va >> 10) & (SH3_FETCH_CACHE - 1)];
    if (slot->tag != tag) {
        uint32_t pa;
        translate_result_t result = translate(cpu, va, false, &pa);
        if (result != TRANSLATE_OK) {
            memory_fault(cpu, result, va, false);
            return false;
        }
        uint8_t *block = NULL;
        if (pa - cpu->bus.dram_base < cpu->bus.dram_size) block = cpu->bus.dram + ((pa - cpu->bus.dram_base) & ~0x3FFu);
        else if (pa < CONTROL_SPACE && cpu->bus.fetch_page) {
            uint8_t *page = cpu->bus.fetch_page(cpu->bus.context, pa & ~0xFFFu);
            if (page) block = page + (pa & 0xC00u);
        }
        if (!block) {
            uint32_t value;
            physical_read(cpu, pa, 2, &value);
            *op = (uint16_t)value;
            return true;
        }
        slot->tag = tag;
        slot->block = block;
    }
    *op = (uint16_t)read_host(slot->block + (va & 0x3FFu), 2);
    return true;
}

static bool privileged(sh3_cpu_t *cpu) {
    if (cpu->sr & SH3_SR_MD) return true;
    illegal_instruction(cpu);
    return false;
}

static bool is_branch(uint16_t op) {
    switch (op & 0xF000) {
    case 0xA000: case 0xB000: return true;
    case 0x8000: {
        uint32_t sub = (op >> 8) & 15;
        return sub == 0x9 || sub == 0xB || sub == 0xD || sub == 0xF;
    }
    case 0xC000: return ((op >> 8) & 15) == 0x3;
    case 0x0000: {
        uint32_t low = op & 0xFF;
        return low == 0x03 || low == 0x23 || op == 0x000B || op == 0x002B;
    }
    case 0x4000: {
        uint32_t low = op & 0xFF;
        return low == 0x0B || low == 0x2B;
    }
    default: return false;
    }
}

static void execute(sh3_cpu_t *cpu, uint16_t op);

static bool fetch_slot(sh3_cpu_t *cpu, uint16_t *slot_op) {
    slot_branch_pc = current_pc;
    cpu->in_slot = true;
    if (!fetch(cpu, current_pc + 2, slot_op)) {
        cpu->in_slot = false;
        return false;
    }
    if (is_branch(*slot_op)) {
        illegal_instruction(cpu);
        cpu->in_slot = false;
        return false;
    }
    return true;
}

static void run_slot(sh3_cpu_t *cpu, uint16_t slot_op, uint32_t target) {
    uint32_t branch_pc = current_pc;
    current_pc = branch_pc + 2;
    cpu->pc = branch_pc + 4;
    cpu->cycles++;
    execute(cpu, slot_op);
    current_pc = branch_pc;
    cpu->in_slot = false;
    if (!cpu->fault) cpu->pc = target;
}

static void delayed_branch(sh3_cpu_t *cpu, uint32_t target) {
    uint16_t slot_op;
    if (fetch_slot(cpu, &slot_op)) run_slot(cpu, slot_op, target);
}

static void div1(sh3_cpu_t *cpu, uint32_t n, uint32_t m) {
    uint32_t *r = cpu->r;
    bool old_q = (cpu->sr & SH3_SR_Q) != 0;
    bool mbit = (cpu->sr & SH3_SR_M) != 0;
    bool q = (r[n] >> 31) != 0;
    uint32_t divisor = r[m];
    r[n] = (r[n] << 1) | (cpu->sr & SH3_SR_T);
    uint32_t before = r[n];
    bool carry;
    if (old_q == mbit) {
        r[n] -= divisor;
        carry = r[n] > before;
    } else {
        r[n] += divisor;
        carry = r[n] < before;
    }
    if (!mbit) q = q ? !carry : carry;
    else q = q ? carry : !carry;
    cpu->sr = (cpu->sr & ~(SH3_SR_Q | SH3_SR_T)) | (q ? SH3_SR_Q : 0) | (q == mbit ? SH3_SR_T : 0);
}

static void set_t(sh3_cpu_t *cpu, bool value) {
    cpu->sr = (cpu->sr & ~SH3_SR_T) | (value ? SH3_SR_T : 0);
}

static uint32_t t_bit(const sh3_cpu_t *cpu) {
    return cpu->sr & SH3_SR_T;
}

static void mac_long(sh3_cpu_t *cpu, uint32_t n, uint32_t m) {
    uint32_t *r = cpu->r;
    uint32_t a, b;
    if (!load(cpu, r[n], 4, &a)) return;
    r[n] += 4;
    if (!load(cpu, r[m], 4, &b)) { r[n] -= 4; return; }
    r[m] += 4;
    int64_t product = (int64_t)(int32_t)a * (int32_t)b;
    int64_t accumulator = (int64_t)(((uint64_t)cpu->mach << 32) | cpu->macl);
    int64_t sum = (int64_t)((uint64_t)accumulator + (uint64_t)product);
    if (cpu->sr & SH3_SR_S) {
        const int64_t high = 0x00007FFFFFFFFFFFll, low = -0x0000800000000000ll;
        if (sum > high) sum = high;
        if (sum < low) sum = low;
    }
    cpu->mach = (uint32_t)((uint64_t)sum >> 32);
    cpu->macl = (uint32_t)sum;
}

static void mac_word(sh3_cpu_t *cpu, uint32_t n, uint32_t m) {
    uint32_t *r = cpu->r;
    uint32_t a, b;
    if (!load(cpu, r[n], 2, &a)) return;
    r[n] += 2;
    if (!load(cpu, r[m], 2, &b)) { r[n] -= 2; return; }
    r[m] += 2;
    int64_t product = (int64_t)(int16_t)a * (int16_t)b;
    if (cpu->sr & SH3_SR_S) {
        int64_t sum = (int64_t)(int32_t)cpu->macl + product;
        if (sum > INT32_MAX) { sum = INT32_MAX; cpu->mach |= 1; }
        else if (sum < INT32_MIN) { sum = INT32_MIN; cpu->mach |= 1; }
        cpu->macl = (uint32_t)sum;
    } else {
        int64_t accumulator = (int64_t)(((uint64_t)cpu->mach << 32) | cpu->macl);
        uint64_t sum = (uint64_t)accumulator + (uint64_t)product;
        cpu->mach = (uint32_t)(sum >> 32);
        cpu->macl = (uint32_t)sum;
    }
}

static uint32_t shift_arithmetic(uint32_t value, uint32_t amount) {
    if ((int32_t)amount >= 0) return value << (amount & 31);
    if ((amount & 31) == 0) return (int32_t)value < 0 ? 0xFFFFFFFFu : 0;
    return (uint32_t)((int32_t)value >> ((~amount & 31) + 1));
}

static uint32_t shift_logical(uint32_t value, uint32_t amount) {
    if ((int32_t)amount >= 0) return value << (amount & 31);
    if ((amount & 31) == 0) return 0;
    return value >> ((~amount & 31) + 1);
}

static void sleep_instruction(sh3_cpu_t *cpu) {
    cpu->sleeping = true;
    cpu->yield = true;
}

static void return_from_exception(sh3_cpu_t *cpu) {
    uint16_t slot_op;
    if (!fetch_slot(cpu, &slot_op)) return;
    uint32_t target = cpu->spc;
    sh3_set_sr(cpu, cpu->ssr);
    run_slot(cpu, slot_op, target);
}

static void execute_0(sh3_cpu_t *cpu, uint16_t op) {
    uint32_t *r = cpu->r;
    uint32_t n = (op >> 8) & 15, m = (op >> 4) & 15;
    uint32_t value;
    switch (op & 15) {
    case 0x2:
        if (m & 8) {
            if (privileged(cpu)) r[n] = cpu->bank[m & 7];
            return;
        }
        switch (m) {
        case 0: if (privileged(cpu)) r[n] = cpu->sr; return;
        case 1: r[n] = cpu->gbr; return;
        case 2: if (privileged(cpu)) r[n] = cpu->vbr; return;
        case 3: if (privileged(cpu)) r[n] = cpu->ssr; return;
        case 4: if (privileged(cpu)) r[n] = cpu->spc; return;
        default: illegal_instruction(cpu); return;
        }
    case 0x3:
        switch (m) {
        case 0x0: { uint32_t target = current_pc + 4 + r[n]; cpu->pr = current_pc + 4; delayed_branch(cpu, target); return; }
        case 0x2: delayed_branch(cpu, current_pc + 4 + r[n]); return;
        case 0x8: case 0x9: case 0xA: case 0xB: case 0xC: return;
        default: illegal_instruction(cpu); return;
        }
    case 0x4: store(cpu, r[0] + r[n], 1, r[m]); return;
    case 0x5: store(cpu, r[0] + r[n], 2, r[m]); return;
    case 0x6: store(cpu, r[0] + r[n], 4, r[m]); return;
    case 0x7: cpu->macl = r[n] * r[m]; return;
    case 0x8:
        if (n) { illegal_instruction(cpu); return; }
        switch (m) {
        case 0: cpu->sr &= ~SH3_SR_T; return;
        case 1: cpu->sr |= SH3_SR_T; return;
        case 2: cpu->mach = cpu->macl = 0; return;
        case 3: if (privileged(cpu)) load_tlb(cpu); return;
        case 4: cpu->sr &= ~SH3_SR_S; return;
        case 5: cpu->sr |= SH3_SR_S; return;
        default: illegal_instruction(cpu); return;
        }
    case 0x9:
        switch (m) {
        case 0: if (n) illegal_instruction(cpu); return;
        case 1: if (n) illegal_instruction(cpu); else cpu->sr &= ~(SH3_SR_M | SH3_SR_Q | SH3_SR_T); return;
        case 2: r[n] = t_bit(cpu); return;
        default: illegal_instruction(cpu); return;
        }
    case 0xA:
        switch (m) {
        case 0: r[n] = cpu->mach; return;
        case 1: r[n] = cpu->macl; return;
        case 2: r[n] = cpu->pr; return;
        default: illegal_instruction(cpu); return;
        }
    case 0xB:
        if (n) { illegal_instruction(cpu); return; }
        switch (m) {
        case 0: delayed_branch(cpu, cpu->pr); return;
        case 1: if (privileged(cpu)) sleep_instruction(cpu); return;
        case 2: if (privileged(cpu)) return_from_exception(cpu); return;
        default: illegal_instruction(cpu); return;
        }
    case 0xC: if (load(cpu, r[0] + r[m], 1, &value)) r[n] = (uint32_t)(int8_t)value; return;
    case 0xD: if (load(cpu, r[0] + r[m], 2, &value)) r[n] = (uint32_t)(int16_t)value; return;
    case 0xE: if (load(cpu, r[0] + r[m], 4, &value)) r[n] = value; return;
    case 0xF: mac_long(cpu, n, m); return;
    default: illegal_instruction(cpu); return;
    }
}

static void execute_2(sh3_cpu_t *cpu, uint16_t op) {
    uint32_t *r = cpu->r;
    uint32_t n = (op >> 8) & 15, m = (op >> 4) & 15;
    switch (op & 15) {
    case 0x0: store(cpu, r[n], 1, r[m]); return;
    case 0x1: store(cpu, r[n], 2, r[m]); return;
    case 0x2: store(cpu, r[n], 4, r[m]); return;
    case 0x4: if (store(cpu, r[n] - 1, 1, r[m])) r[n] -= 1; return;
    case 0x5: if (store(cpu, r[n] - 2, 2, r[m])) r[n] -= 2; return;
    case 0x6: if (store(cpu, r[n] - 4, 4, r[m])) r[n] -= 4; return;
    case 0x7: {
        bool q = (r[n] >> 31) != 0, mbit = (r[m] >> 31) != 0;
        cpu->sr = (cpu->sr & ~(SH3_SR_Q | SH3_SR_M | SH3_SR_T)) | (q ? SH3_SR_Q : 0) | (mbit ? SH3_SR_M : 0) | (q != mbit ? SH3_SR_T : 0);
        return;
    }
    case 0x8: set_t(cpu, (r[n] & r[m]) == 0); return;
    case 0x9: r[n] &= r[m]; return;
    case 0xA: r[n] ^= r[m]; return;
    case 0xB: r[n] |= r[m]; return;
    case 0xC: {
        uint32_t bytes = r[n] ^ r[m];
        set_t(cpu, !(bytes & 0xFF000000u) || !(bytes & 0x00FF0000u) || !(bytes & 0x0000FF00u) || !(bytes & 0x000000FFu));
        return;
    }
    case 0xD: r[n] = (r[m] << 16) | (r[n] >> 16); return;
    case 0xE: cpu->macl = (r[n] & 0xFFFFu) * (r[m] & 0xFFFFu); return;
    case 0xF: cpu->macl = (uint32_t)((int32_t)(int16_t)r[n] * (int32_t)(int16_t)r[m]); return;
    default: illegal_instruction(cpu); return;
    }
}

static void execute_3(sh3_cpu_t *cpu, uint16_t op) {
    uint32_t *r = cpu->r;
    uint32_t n = (op >> 8) & 15, m = (op >> 4) & 15;
    switch (op & 15) {
    case 0x0: set_t(cpu, r[n] == r[m]); return;
    case 0x2: set_t(cpu, r[n] >= r[m]); return;
    case 0x3: set_t(cpu, (int32_t)r[n] >= (int32_t)r[m]); return;
    case 0x4: div1(cpu, n, m); return;
    case 0x5: { uint64_t product = (uint64_t)r[n] * r[m]; cpu->mach = (uint32_t)(product >> 32); cpu->macl = (uint32_t)product; return; }
    case 0x6: set_t(cpu, r[n] > r[m]); return;
    case 0x7: set_t(cpu, (int32_t)r[n] > (int32_t)r[m]); return;
    case 0x8: r[n] -= r[m]; return;
    case 0xA: {
        uint32_t before = r[n], difference = before - r[m];
        r[n] = difference - t_bit(cpu);
        set_t(cpu, before < difference || difference < r[n]);
        return;
    }
    case 0xB: {
        uint32_t difference = r[n] - r[m];
        set_t(cpu, ((r[n] ^ r[m]) & (r[n] ^ difference)) >> 31);
        r[n] = difference;
        return;
    }
    case 0xC: r[n] += r[m]; return;
    case 0xD: { int64_t product = (int64_t)(int32_t)r[n] * (int32_t)r[m]; cpu->mach = (uint32_t)((uint64_t)product >> 32); cpu->macl = (uint32_t)product; return; }
    case 0xE: {
        uint32_t before = r[n], sum = before + r[m];
        r[n] = sum + t_bit(cpu);
        set_t(cpu, before > sum || sum > r[n]);
        return;
    }
    case 0xF: {
        uint32_t sum = r[n] + r[m];
        set_t(cpu, (~(r[n] ^ r[m]) & (r[n] ^ sum)) >> 31);
        r[n] = sum;
        return;
    }
    default: illegal_instruction(cpu); return;
    }
}

static bool push(sh3_cpu_t *cpu, uint32_t n, uint32_t value) {
    if (!store(cpu, cpu->r[n] - 4, 4, value)) return false;
    cpu->r[n] -= 4;
    return true;
}

static bool pop(sh3_cpu_t *cpu, uint32_t m, uint32_t *value) {
    if (!load(cpu, cpu->r[m], 4, value)) return false;
    cpu->r[m] += 4;
    return true;
}

static void execute_4(sh3_cpu_t *cpu, uint16_t op) {
    uint32_t *r = cpu->r;
    uint32_t n = (op >> 8) & 15, m = (op >> 4) & 15;
    uint32_t value;
    switch (op & 15) {
    case 0xC: r[n] = shift_arithmetic(r[n], r[m]); return;
    case 0xD: r[n] = shift_logical(r[n], r[m]); return;
    case 0xF: mac_word(cpu, n, m); return;
    case 0x3:
        if (m & 8) { if (privileged(cpu)) push(cpu, n, cpu->bank[m & 7]); return; }
        break;
    case 0x7:
        if (m & 8) { if (privileged(cpu) && pop(cpu, n, &value)) cpu->bank[m & 7] = value; return; }
        break;
    case 0xE:
        if (m & 8) { if (privileged(cpu)) cpu->bank[m & 7] = r[n]; return; }
        break;
    default:
        break;
    }
    switch (op & 0xFF) {
    case 0x00: set_t(cpu, r[n] >> 31); r[n] <<= 1; return;
    case 0x01: set_t(cpu, r[n] & 1); r[n] >>= 1; return;
    case 0x02: push(cpu, n, cpu->mach); return;
    case 0x03: if (privileged(cpu)) push(cpu, n, cpu->sr); return;
    case 0x04: set_t(cpu, r[n] >> 31); r[n] = (r[n] << 1) | (r[n] >> 31); return;
    case 0x05: set_t(cpu, r[n] & 1); r[n] = (r[n] >> 1) | (r[n] << 31); return;
    case 0x06: if (pop(cpu, n, &value)) cpu->mach = value; return;
    case 0x07: if (privileged(cpu) && pop(cpu, n, &value)) sh3_set_sr(cpu, value); return;
    case 0x08: r[n] <<= 2; return;
    case 0x09: r[n] >>= 2; return;
    case 0x0A: cpu->mach = r[n]; return;
    case 0x0B: { uint32_t target = r[n]; cpu->pr = current_pc + 4; delayed_branch(cpu, target); return; }
    case 0x0E: if (privileged(cpu)) sh3_set_sr(cpu, r[n]); return;
    case 0x10: r[n] -= 1; set_t(cpu, r[n] == 0); return;
    case 0x11: set_t(cpu, (int32_t)r[n] >= 0); return;
    case 0x12: push(cpu, n, cpu->macl); return;
    case 0x13: push(cpu, n, cpu->gbr); return;
    case 0x15: set_t(cpu, (int32_t)r[n] > 0); return;
    case 0x16: if (pop(cpu, n, &value)) cpu->macl = value; return;
    case 0x17: if (pop(cpu, n, &value)) cpu->gbr = value; return;
    case 0x18: r[n] <<= 8; return;
    case 0x19: r[n] >>= 8; return;
    case 0x1A: cpu->macl = r[n]; return;
    case 0x1B:
        if (load(cpu, r[n], 1, &value)) {
            set_t(cpu, (value & 0xFF) == 0);
            store(cpu, r[n], 1, value | 0x80);
        }
        return;
    case 0x1E: cpu->gbr = r[n]; return;
    case 0x20: set_t(cpu, r[n] >> 31); r[n] <<= 1; return;
    case 0x21: set_t(cpu, r[n] & 1); r[n] = (uint32_t)((int32_t)r[n] >> 1); return;
    case 0x22: push(cpu, n, cpu->pr); return;
    case 0x23: if (privileged(cpu)) push(cpu, n, cpu->vbr); return;
    case 0x24: { uint32_t carry = r[n] >> 31; r[n] = (r[n] << 1) | t_bit(cpu); set_t(cpu, carry); return; }
    case 0x25: { uint32_t carry = r[n] & 1; r[n] = (r[n] >> 1) | (t_bit(cpu) << 31); set_t(cpu, carry); return; }
    case 0x26: if (pop(cpu, n, &value)) cpu->pr = value; return;
    case 0x27: if (privileged(cpu) && pop(cpu, n, &value)) cpu->vbr = value; return;
    case 0x28: r[n] <<= 16; return;
    case 0x29: r[n] >>= 16; return;
    case 0x2A: cpu->pr = r[n]; return;
    case 0x2B: delayed_branch(cpu, r[n]); return;
    case 0x2E: if (privileged(cpu)) cpu->vbr = r[n]; return;
    case 0x33: if (privileged(cpu)) push(cpu, n, cpu->ssr); return;
    case 0x37: if (privileged(cpu) && pop(cpu, n, &value)) cpu->ssr = value; return;
    case 0x3E: if (privileged(cpu)) cpu->ssr = r[n]; return;
    case 0x43: if (privileged(cpu)) push(cpu, n, cpu->spc); return;
    case 0x47: if (privileged(cpu) && pop(cpu, n, &value)) cpu->spc = value; return;
    case 0x4E: if (privileged(cpu)) cpu->spc = r[n]; return;
    default: illegal_instruction(cpu); return;
    }
}

static void execute_6(sh3_cpu_t *cpu, uint16_t op) {
    uint32_t *r = cpu->r;
    uint32_t n = (op >> 8) & 15, m = (op >> 4) & 15;
    uint32_t value;
    switch (op & 15) {
    case 0x0: if (load(cpu, r[m], 1, &value)) r[n] = (uint32_t)(int8_t)value; return;
    case 0x1: if (load(cpu, r[m], 2, &value)) r[n] = (uint32_t)(int16_t)value; return;
    case 0x2: if (load(cpu, r[m], 4, &value)) r[n] = value; return;
    case 0x3: r[n] = r[m]; return;
    case 0x4: if (load(cpu, r[m], 1, &value)) { if (n != m) r[m] += 1; r[n] = (uint32_t)(int8_t)value; } return;
    case 0x5: if (load(cpu, r[m], 2, &value)) { if (n != m) r[m] += 2; r[n] = (uint32_t)(int16_t)value; } return;
    case 0x6: if (load(cpu, r[m], 4, &value)) { if (n != m) r[m] += 4; r[n] = value; } return;
    case 0x7: r[n] = ~r[m]; return;
    case 0x8: r[n] = (r[m] & 0xFFFF0000u) | ((r[m] & 0xFF) << 8) | ((r[m] >> 8) & 0xFF); return;
    case 0x9: r[n] = (r[m] << 16) | (r[m] >> 16); return;
    case 0xA: {
        uint32_t negated = 0 - r[m];
        r[n] = negated - t_bit(cpu);
        set_t(cpu, 0 < negated || negated < r[n]);
        return;
    }
    case 0xB: r[n] = 0 - r[m]; return;
    case 0xC: r[n] = r[m] & 0xFF; return;
    case 0xD: r[n] = r[m] & 0xFFFF; return;
    case 0xE: r[n] = (uint32_t)(int8_t)r[m]; return;
    case 0xF: r[n] = (uint32_t)(int16_t)r[m]; return;
    }
}

static void conditional_branch(sh3_cpu_t *cpu, uint16_t op, bool when, bool delayed) {
    uint32_t target = current_pc + 4 + (uint32_t)((int32_t)(int8_t)(op & 0xFF) * 2);
    bool taken = (t_bit(cpu) != 0) == when;
    if (cpu->in_slot) { illegal_instruction(cpu); return; }
    if (delayed) delayed_branch(cpu, taken ? target : current_pc + 4);
    else if (taken) cpu->pc = target;
}

static void execute_8(sh3_cpu_t *cpu, uint16_t op) {
    uint32_t *r = cpu->r;
    uint32_t disp = op & 15, rn = (op >> 4) & 15;
    uint32_t value;
    switch ((op >> 8) & 15) {
    case 0x0: store(cpu, r[rn] + disp, 1, r[0]); return;
    case 0x1: store(cpu, r[rn] + disp * 2, 2, r[0]); return;
    case 0x4: if (load(cpu, r[rn] + disp, 1, &value)) r[0] = (uint32_t)(int8_t)value; return;
    case 0x5: if (load(cpu, r[rn] + disp * 2, 2, &value)) r[0] = (uint32_t)(int16_t)value; return;
    case 0x8: set_t(cpu, r[0] == (uint32_t)(int8_t)(op & 0xFF)); return;
    case 0x9: conditional_branch(cpu, op, true, false); return;
    case 0xB: conditional_branch(cpu, op, false, false); return;
    case 0xD: conditional_branch(cpu, op, true, true); return;
    case 0xF: conditional_branch(cpu, op, false, true); return;
    default: illegal_instruction(cpu); return;
    }
}

static void trapa(sh3_cpu_t *cpu, uint32_t number) {
    if (cpu->on_trapa && cpu->on_trapa(cpu->bus.context, number)) return;
    cpu->tra = number << 2;
    take_exception(cpu, SH3_EXP_TRAPA, VECTOR_GENERAL, current_pc + 2);
}

static void execute_c(sh3_cpu_t *cpu, uint16_t op) {
    uint32_t *r = cpu->r;
    uint32_t imm = op & 0xFF;
    uint32_t value;
    switch ((op >> 8) & 15) {
    case 0x0: store(cpu, cpu->gbr + imm, 1, r[0]); return;
    case 0x1: store(cpu, cpu->gbr + imm * 2, 2, r[0]); return;
    case 0x2: store(cpu, cpu->gbr + imm * 4, 4, r[0]); return;
    case 0x3: trapa(cpu, imm); return;
    case 0x4: if (load(cpu, cpu->gbr + imm, 1, &value)) r[0] = (uint32_t)(int8_t)value; return;
    case 0x5: if (load(cpu, cpu->gbr + imm * 2, 2, &value)) r[0] = (uint32_t)(int16_t)value; return;
    case 0x6: if (load(cpu, cpu->gbr + imm * 4, 4, &value)) r[0] = value; return;
    case 0x7: r[0] = ((current_pc + 4) & ~3u) + imm * 4; return;
    case 0x8: set_t(cpu, (r[0] & imm) == 0); return;
    case 0x9: r[0] &= imm; return;
    case 0xA: r[0] ^= imm; return;
    case 0xB: r[0] |= imm; return;
    case 0xC: if (load(cpu, cpu->gbr + r[0], 1, &value)) set_t(cpu, (value & imm) == 0); return;
    case 0xD: if (load(cpu, cpu->gbr + r[0], 1, &value)) store(cpu, cpu->gbr + r[0], 1, value & imm); return;
    case 0xE: if (load(cpu, cpu->gbr + r[0], 1, &value)) store(cpu, cpu->gbr + r[0], 1, value ^ imm); return;
    case 0xF: if (load(cpu, cpu->gbr + r[0], 1, &value)) store(cpu, cpu->gbr + r[0], 1, value | imm); return;
    }
}

static void execute(sh3_cpu_t *cpu, uint16_t op) {
    uint32_t *r = cpu->r;
    uint32_t n = (op >> 8) & 15, m = (op >> 4) & 15;
    uint32_t value;
    switch (op >> 12) {
    case 0x0: execute_0(cpu, op); return;
    case 0x1: store(cpu, r[n] + (op & 15) * 4, 4, r[m]); return;
    case 0x2: execute_2(cpu, op); return;
    case 0x3: execute_3(cpu, op); return;
    case 0x4: execute_4(cpu, op); return;
    case 0x5: if (load(cpu, r[m] + (op & 15) * 4, 4, &value)) r[n] = value; return;
    case 0x6: execute_6(cpu, op); return;
    case 0x7: r[n] += (uint32_t)(int8_t)(op & 0xFF); return;
    case 0x8: execute_8(cpu, op); return;
    case 0x9: if (load(cpu, current_pc + 4 + (op & 0xFF) * 2, 2, &value)) r[n] = (uint32_t)(int16_t)value; return;
    case 0xA: {
        int32_t disp = ((int32_t)((uint32_t)op << 20) >> 20) * 2;
        delayed_branch(cpu, current_pc + 4 + (uint32_t)disp);
        return;
    }
    case 0xB: {
        int32_t disp = ((int32_t)((uint32_t)op << 20) >> 20) * 2;
        cpu->pr = current_pc + 4;
        delayed_branch(cpu, current_pc + 4 + (uint32_t)disp);
        return;
    }
    case 0xC: execute_c(cpu, op); return;
    case 0xD: if (load(cpu, ((current_pc + 4) & ~3u) + (op & 0xFF) * 4, 4, &value)) r[n] = value; return;
    case 0xE: r[n] = (uint32_t)(int8_t)(op & 0xFF); return;
    default: illegal_instruction(cpu); return;
    }
}

static uint32_t watch_bit(uint32_t va) {
    return (va >> 1) & 4095;
}

void sh3_debug_filter_add(sh3_debug_t *debug, uint32_t va) {
    uint32_t bit = watch_bit(va);
    debug->filter[bit >> 5] |= 1u << (bit & 31);
}

static bool debug_stops_before(sh3_cpu_t *cpu, uint32_t pc) {
    sh3_debug_t *debug = cpu->debug;
    if (!debug->every) {
        uint32_t bit = watch_bit(pc);
        if (!(debug->filter[bit >> 5] >> (bit & 31) & 1)) return false;
    }
    return debug->before(debug->context, pc);
}

static bool interrupt_wakes(const sh3_cpu_t *cpu) {
    return cpu->interrupt_level > ((cpu->sr & SH3_SR_IMASK) >> 4);
}

static bool interrupt_acceptable(const sh3_cpu_t *cpu) {
    return !(cpu->sr & SH3_SR_BL) && interrupt_wakes(cpu);
}

static void take_interrupt(sh3_cpu_t *cpu) {
    uint32_t code = cpu->interrupt_code;
    cpu->intevt = code;
    cpu->intevt2 = cpu->interrupt_source ? cpu->interrupt_source : code;
    if (cpu->on_interrupt) cpu->on_interrupt(cpu->bus.context, code);
    cpu->exceptions[63]++;
    cpu->sleeping = false;
    cpu->spc = cpu->pc;
    cpu->ssr = cpu->sr;
    sh3_set_sr(cpu, cpu->sr | SH3_SR_MD | SH3_SR_RB | SH3_SR_BL);
    cpu->pc = cpu->vbr + VECTOR_INTERRUPT;
}

void sh3_run(sh3_cpu_t *cpu, uint64_t until_cycle) {
    cpu->yield = false;
    memset(cpu->watch_filter, 0, sizeof cpu->watch_filter);
    for (int w = 0; w < cpu->watch_count; w++) cpu->watch_filter[watch_bit(cpu->watch[w]) >> 5] |= 1u << (watch_bit(cpu->watch[w]) & 31);
    uint32_t speed = cpu->speed ? cpu->speed : 1;
    while (cpu->cycles < until_cycle && !cpu->yield) {
        if (cpu->sleeping) {
            if (!interrupt_wakes(cpu)) {
                cpu->cycles = until_cycle;
                break;
            }
            cpu->sleeping = false;
            if (cpu->standby_wakes_blocked) {
                take_interrupt(cpu);
                continue;
            }
        }
        if (++cpu->speed_count >= speed) {
            cpu->speed_count = 0;
            cpu->cycles++;
        }
        if (interrupt_acceptable(cpu)) {
            take_interrupt(cpu);
            continue;
        }
        cpu->fault = false;
        current_pc = cpu->pc;
        uint16_t op;
        if (!fetch(cpu, current_pc, &op)) continue;
        uint32_t bit = watch_bit(current_pc);
        bool handled = false;
        for (int w = 0; (cpu->watch_filter[bit >> 5] >> (bit & 31) & 1) && w < cpu->watch_count && !handled; w++)
            if (current_pc == cpu->watch[w]) handled = cpu->on_watch(cpu->bus.context, current_pc);
        if (cpu->fault || handled) continue;
        if (cpu->debug) {
            if (debug_stops_before(cpu, current_pc)) {
                cpu->debug->stop = true;
                break;
            }
            cpu->debug->pc = current_pc;
        }
        uint32_t saved[16];
        bool undo_possible = cpu->debug && cpu->debug->data;
        if (undo_possible) memcpy(saved, cpu->r, sizeof saved);
        cpu->pc = current_pc + 2;
        execute(cpu, op);
        if (cpu->debug && cpu->debug->stop) {
            if (cpu->debug->undo) {
                cpu->debug->undo = false;
                memcpy(cpu->r, saved, sizeof saved);
                cpu->pc = current_pc;
            }
            break;
        }
    }
}
