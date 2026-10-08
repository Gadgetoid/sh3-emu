#include "core/machine.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <zlib.h>

#include "core/accel.h"
#include "core/casio.h"
#include "core/ce.h"
#include "core/hp320lx.h"
#include "core/cfcard.h"
#include "core/mailbox.h"
#include "core/sh7709.h"
#include "util/file.h"

#define DRAM_PA            0x0C000000u
#define NMI_CODE           0x1C0u
#define DRAM_DEFAULT_SIZE  (16u << 20)
#define DRAM_AREA_SIZE     (64u << 20)
#define AREA_MASK          0x1FFFFFFFu
#define FLASH_SIZE         (16u << 20)
#define RESET_VECTOR       0xA0000000u
#define P4_ROUTINES_END    0xE0010000u
#define STBCR_STANDBY      0x80u
#define SCANCODE_BACKLIGHT 0x5E
#define CASIO_TIMER_HZ     (CASIO_PERIPHERAL_HZ / 16)
#define OP_RTS             0x000Bu
#define OP_NOP             0x0009u
#define ROM_HEADER_SIZE    0x54u
#define ROM_DUMP_MIN_SPAN  0x100000u
#define ROM_DUMP_MAX_MODULES 1024u
#define KSEG0_BASE         0x80000000u
#define KSEG1_BASE         0xA0000000u

#define SERIAL_FIFO        16384
#define CARD_PATH_MAX      1024
#define DICTIONARY_PA      0x04000000u
#define AUDIO_RATE         22050u
#define AUDIO_RING         65536u
#define AUDIO_GAP_CYCLES   (MACHINE_CLOCK_HZ / 20)
#define AUDIO_CHANNEL      1
#define TICK_COUNTER_PA    0xFFFFFE98u
#define CASIO_LINK_STATUS_PA 0x10000122u
#define SPIN_READS         16u
#define SPIN_WINDOW        (MACHINE_CLOCK_HZ / 1000)
#define DICTIONARY_MAX     (8u << 20)
#define SERIAL_TICKS_PER_SECOND 1000u
#define SERIAL_TICK_CYCLES (MACHINE_CLOCK_HZ / SERIAL_TICKS_PER_SECOND)
#define SERIAL_BITS_PER_BYTE 10u

#define UNKNOWN_SEEN       4096

#define STATE_MAGIC        "SH3ODO01"

#define MAILBOX_FAULT_TRIES 4
#define MAILBOX_PAGES      ((MAILBOX_MESSAGE_MAX >> 10) + 2)

typedef struct {
    uint32_t page, pa;
    bool write;
} mailbox_page_t;

typedef struct {
    char line[256];
    int length;
} debug_line_t;

typedef struct {
    uint64_t window;
    uint32_t reads;
} spin_t;

struct machine {
    sh3_cpu_t cpu;
    sh7709_t chip;
    uint8_t  *dram;
    uint32_t dram_size;
    uint32_t dram_size_next;
    uint8_t  *image;
    size_t image_size;
    uint32_t entry;
    uint64_t rom_hash;
    bool casio;
    bool hp;
    hp320lx_t hp_board;
    bool hp_on_key;
    hp320lx_host_t hp_host;
    uint8_t  *flash;
    casio_t casio_board;
    casio_host_t casio_host;
    casio_audio_t casio_audio;
    debug_line_t sci_line;

    cfcard_t card;
    cfcard_slot_t card_slot;
    char card_path[CARD_PATH_MAX];
    uint8_t  *dictionary;
    int16_t audio[AUDIO_RING];
    uint32_t audio_head, audio_count;
    uint64_t audio_clock, audio_last;
    int16_t audio_level;
    uint32_t audio_phase;
    size_t dictionary_size;
    char pending_card[CARD_PATH_MAX];
    uint64_t pending_card_at;
    bool serial_connected;
    uint8_t serial_rx[SERIAL_FIFO], serial_tx[SERIAL_FIFO];
    uint32_t serial_rx_head, serial_rx_count, serial_tx_count;
    uint64_t serial_tick_at;
    uint64_t run_target;
    spin_t tick_spin, link_spin;
    bool optimisations;
    accel_hooks_t accel;
    ce_t accel_ce;

    mailbox_t mailbox;
    uint32_t mailbox_fault_va;
    int mailbox_fault_tries;
    uint32_t mailbox_pc;
    uint64_t agent_poll_at;
    mailbox_page_t mailbox_pages[MAILBOX_PAGES];
    int mailbox_page_count;

    machine_log_fn log;
    sh3_debug_t exception_debug;
    machine_debug_fn debug_sink;
    void     *debug_context;
    uint32_t unknown_logged;
    uint64_t unknown_seen[UNKNOWN_SEEN];
    bool host_clock;
    uint32_t watch[MACHINE_WATCH_MAX];
    int watch_count;
};

static void machine_logf(machine_t *m, const char *format, ...) {
    if (!m->log) return;
    char text[512];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof text, format, args);
    va_end(args);
    m->log(text);
}

static bool unknown_seen(machine_t *m, const char *what, uint32_t pa) {
    uint64_t key = ((uint64_t)pa << 32 | m->cpu.pc) ^ (uint64_t)(what[0] == 'w');
    if (!key) key = 1;
    uint32_t index = (uint32_t)((key * 11400714819323198485ull) >> 52);
    for (uint32_t probe = 0; probe < UNKNOWN_SEEN; probe++) {
        uint64_t *slot = &m->unknown_seen[(index + probe) % UNKNOWN_SEEN];
        if (*slot == key) return true;
        if (!*slot) {
            *slot = key;
            return false;
        }
    }
    return true;
}

static void note_unknown(machine_t *m, const char *what, uint32_t pa, int size, uint32_t value) {
    if (unknown_seen(m, what, pa)) return;
    m->unknown_logged++;
    machine_logf(m, "%s %08X (%d) = %08X at pc %08X\n", what, pa, size, value, m->cpu.pc);
}

static void board_debug_line(void *context, const char *line) {
    machine_t *m = context;
    if (m->debug_sink) m->debug_sink(m->debug_context, line);
}

static void casio_trace(void *context, bool write, uint32_t pa, int size, uint32_t value) {
    note_unknown(context, write ? "write board" : "read  board", pa, size, value);
}

static void hp_trace(void *context, bool write, uint32_t pa, int size, uint32_t value) {
    note_unknown(context, write ? "write board" : "read  board", pa, size, value);
}

static bool casio_read_memory(void *context, uint32_t pa, uint8_t *data, uint32_t length) {
    machine_t *m = context;
    if (pa - DRAM_PA >= DRAM_AREA_SIZE) return false;
    for (uint32_t i = 0; i < length; i++) data[i] = m->dram[(pa - DRAM_PA + i) % m->dram_size];
    return true;
}

static void casio_samples(void *context, const int16_t *samples, uint32_t count, uint32_t rate);

static uint64_t casio_cycles(void *context) {
    return ((machine_t *)context)->cpu.cycles;
}

static void casio_irl(void *context, uint32_t level, uint32_t code) {
    machine_t *m = context;
    (void)code;
    if (m->chip.irl_level != level) sh7709_set_irl(&m->chip, level);
}

static void casio_onchip(void *context, uint32_t level, uint32_t code) {
    machine_t *m = context;
    if (m->chip.extra_level != level || m->chip.extra_code != code) sh7709_set_extra(&m->chip, level, code);
}

static uint32_t read_le32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void debug_character(machine_t *m, debug_line_t *serial, uint8_t ch) {
    if (ch == '\r') return;
    if (ch == '\n' || serial->length >= (int)sizeof serial->line - 1) {
        serial->line[serial->length] = 0;
        if (m->debug_sink) m->debug_sink(m->debug_context, serial->line);
        serial->length = 0;
        if (ch == '\n') return;
    }
    serial->line[serial->length++] = (char)(ch >= 0x20 && ch < 0x7F ? ch : '?');
}

static void set_serial_lines(machine_t *m) {
    if (m->casio) {
        casio_serial_line(&m->casio_board, &m->casio_host, m->serial_connected);
        return;
    }
    sh7709_set_port_input(&m->chip, HP320LX_SERIAL_PORT, HP320LX_SERIAL_NO_CABLE, m->serial_connected ? 0 : HP320LX_SERIAL_NO_CABLE);
    sh7709_set_irq(&m->chip, HP320LX_SERIAL_IRQ, m->serial_connected);
}

static int onchip_serial_port(machine_t *m) {
    return m->hp ? HP320LX_SERIAL_SCIF : m->casio ? CASIO_SCIF_PORT : -1;
}

static void onchip_serial_receive(machine_t *m) {
    int port = onchip_serial_port(m);
    uint32_t per_tick = machine_serial_baud(m) / SERIAL_BITS_PER_BYTE / SERIAL_TICKS_PER_SECOND;
    if (!per_tick) per_tick = 1;
    while (per_tick-- && m->serial_rx_count && sh7709_receive_room(&m->chip, port)) {
        sh7709_receive(&m->chip, port, m->serial_rx[m->serial_rx_head]);
        m->serial_rx_head = (m->serial_rx_head + 1) % SERIAL_FIFO;
        m->serial_rx_count--;
    }
}

static void serial_tick_event(machine_t *m) {
    if (m->cpu.cycles < m->serial_tick_at) return;
    m->serial_tick_at = m->cpu.cycles + SERIAL_TICK_CYCLES;
    onchip_serial_receive(m);
}

static uint64_t next_event(machine_t *m);

static void stall_spin(machine_t *m, spin_t *spin) {
    uint64_t window = m->cpu.cycles / SPIN_WINDOW;
    if (window != spin->window) {
        spin->window = window;
        spin->reads = 0;
        return;
    }
    if (++spin->reads < SPIN_READS) return;
    spin->reads = 0;
    uint64_t when = (window + 1) * SPIN_WINDOW;
    uint64_t limit = next_event(m);
    if (limit < when) when = limit;
    if (m->run_target < when) when = m->run_target;
    if (when > m->cpu.cycles) m->cpu.cycles = when;
}

static bool bus_read(void *context, uint32_t pa, int size, uint32_t *value) {
    machine_t *m = context;
    if (pa >= 0xE0000000u) {
        if (pa == TICK_COUNTER_PA && m->optimisations) stall_spin(m, &m->tick_spin);
        if (sh7709_read(&m->chip, pa, size, value)) return true;
        if (m->casio && casio_read(&m->casio_board, &m->casio_host, pa, size, value)) return true;
        if (m->casio && pa < P4_ROUTINES_END) {
            *value = (pa & 2) ? OP_RTS : OP_NOP;
            return true;
        }
        note_unknown(m, "read  P4", pa, size, 0);
        *value = 0;
        return true;
    }
    pa &= AREA_MASK;
    if (sh7709_read(&m->chip, pa, size, value)) return true;
    if (m->hp && pa - DRAM_PA >= DRAM_AREA_SIZE && pa >= FLASH_SIZE) {
        return hp320lx_read(&m->hp_board, &m->hp_host, pa, size, value);
    }
    if (m->casio && m->dictionary && pa - DICTIONARY_PA < m->dictionary_size) {
        const uint8_t *base = m->dictionary + (pa - DICTIONARY_PA);
        *value = size == 4 ? read_le32(base) : size == 2 ? (uint32_t)(base[0] | base[1] << 8) : base[0];
        return true;
    }
    if (m->casio && pa - DRAM_PA >= DRAM_AREA_SIZE && pa >= FLASH_SIZE) {
        if (pa == CASIO_LINK_STATUS_PA && m->optimisations) stall_spin(m, &m->link_spin);
        if (casio_read(&m->casio_board, &m->casio_host, pa, size, value)) return true;
        note_unknown(m, "read ", pa, size, 0);
        *value = 0;
        return true;
    }
    if (m->flash && pa < FLASH_SIZE) {
        const uint8_t *base = m->flash + pa;
        *value = size == 4 ? read_le32(base) : size == 2 ? (uint32_t)(base[0] | base[1] << 8) : base[0];
        return true;
    }
    if (pa - DRAM_PA < DRAM_AREA_SIZE) {
        const uint8_t *base = m->dram + (pa - DRAM_PA) % m->dram_size;
        *value = size == 4 ? read_le32(base) : size == 2 ? (uint32_t)(base[0] | base[1] << 8) : base[0];
        return true;
    }
    note_unknown(m, "read ", pa, size, 0);
    *value = size == 4 ? 0xFFFFFFFFu : size == 2 ? 0xFFFFu : 0xFFu;
    return true;
}

static bool bus_write(void *context, uint32_t pa, int size, uint32_t value) {
    machine_t *m = context;
    if (pa >= 0xE0000000u) {
        if (sh7709_write(&m->chip, pa, size, value)) return true;
        if (m->casio && casio_write(&m->casio_board, &m->casio_host, pa, size, value)) {
            sh7709_set_scif_alias(&m->chip, CASIO_SCIF_PA, casio_scif_priority(&m->casio_board));
            return true;
        }
        note_unknown(m, "write P4", pa, size, value);
        return true;
    }
    pa &= AREA_MASK;
    if (sh7709_write(&m->chip, pa, size, value)) return true;
    if (m->hp && pa - DRAM_PA >= DRAM_AREA_SIZE && pa >= FLASH_SIZE) {
        return hp320lx_write(&m->hp_board, &m->hp_host, pa, size, value);
    }
    if (m->casio && m->dictionary && pa - DICTIONARY_PA < m->dictionary_size) {
        note_unknown(m, "write dictionary", pa, size, value);
        return true;
    }
    if (m->casio && pa - DRAM_PA >= DRAM_AREA_SIZE && pa >= FLASH_SIZE) {
        if (!casio_write(&m->casio_board, &m->casio_host, pa, size, value)) note_unknown(m, "write", pa, size, value);
        return true;
    }
    if (m->flash && pa < FLASH_SIZE) {
        note_unknown(m, "write flash", pa, size, value);
        return true;
    }
    if (pa - DRAM_PA < DRAM_AREA_SIZE) {
        uint32_t offset = (pa - DRAM_PA) % m->dram_size;
        for (int i = 0; i < size; i++) m->dram[offset + (uint32_t)i] = (uint8_t)(value >> (8 * i));
        return true;
    }
    note_unknown(m, "write", pa, size, value);
    return true;
}

static uint8_t *bus_fetch_page(void *context, uint32_t pa) {
    machine_t *m = context;
    if (pa >= 0xE0000000u) return NULL;
    pa &= AREA_MASK;
    if (pa - DRAM_PA < m->dram_size) return m->dram + (pa - DRAM_PA);
    if (m->flash && pa < FLASH_SIZE) return m->flash + pa;
    return NULL;
}

static void onchip_transmit(void *context, int port, uint8_t byte) {
    machine_t *m = context;
    if (port == onchip_serial_port(m)) {
        if (m->serial_connected && m->serial_tx_count < SERIAL_FIFO) m->serial_tx[m->serial_tx_count++] = byte;
        return;
    }
    debug_character(m, &m->sci_line, byte);
}

static void hp_ports_written(void *context) {
    machine_t *m = context;
    uint16_t columns = hp320lx_key_columns(&m->hp_board, m->chip.ports);
    sh7709_set_port_input(&m->chip, HP320LX_KEY_COLUMNS_LOW, 0x00FFu, columns & 0xFFu);
    sh7709_set_port_input(&m->chip, HP320LX_KEY_COLUMNS_HIGH, 0x0007u, columns >> 8);
    hp320lx_touch_t touch = hp320lx_touch_inputs(&m->hp_board, m->chip.ports);
    sh7709_set_adc(&m->chip, 0, touch.channel_a);
    sh7709_set_adc(&m->chip, 1, touch.channel_b);
    if (touch.pen_interrupt != (((m->chip.irq_lines >> HP320LX_PEN_IRQ) & 1) != 0)) sh7709_set_irq(&m->chip, HP320LX_PEN_IRQ, touch.pen_interrupt);
}

static void audio_push(machine_t *m, int16_t sample) {
    if (m->audio_count == AUDIO_RING) {
        m->audio_head = (m->audio_head + 1) % AUDIO_RING;
        m->audio_count--;
    }
    m->audio[(m->audio_head + m->audio_count) % AUDIO_RING] = sample;
    m->audio_count++;
}

static void hp_dac_written(void *context, int channel, uint8_t value, uint64_t cycle) {
    machine_t *m = context;
    if (channel != AUDIO_CHANNEL) return;
    uint64_t scaled = cycle * AUDIO_RATE;
    bool resumed = !m->audio_last || cycle - m->audio_last > AUDIO_GAP_CYCLES || scaled < m->audio_clock;
    if (resumed) m->audio_clock = scaled;
    while (m->audio_clock < scaled) {
        audio_push(m, m->audio_level);
        m->audio_clock += MACHINE_CLOCK_HZ;
    }
    m->audio_level = (int16_t)(((int)value - 128) * 256);
    m->audio_last = cycle;
}

static void casio_samples(void *context, const int16_t *samples, uint32_t count, uint32_t rate) {
    machine_t *m = context;
    for (uint32_t i = 0; i < count; i++) {
        for (m->audio_phase += AUDIO_RATE; m->audio_phase >= rate; m->audio_phase -= rate) audio_push(m, samples[i]);
    }
}

size_t machine_audio(machine_t *m, int16_t *samples, size_t max, uint32_t *rate) {
    size_t count = m->audio_count < max ? m->audio_count : max;
    for (size_t i = 0; i < count; i++) samples[i] = m->audio[(m->audio_head + i) % AUDIO_RING];
    m->audio_head = (uint32_t)((m->audio_head + count) % AUDIO_RING);
    m->audio_count -= (uint32_t)count;
    *rate = AUDIO_RATE;
    return count;
}

static void casio_nmi_taken(void *context, uint32_t code) {
    machine_t *m = context;
    if (code != NMI_CODE) return;
    m->chip.nmi = false;
    sh7709_update_interrupts(&m->chip);
}

static void casio_first_wake(machine_t *m) {
    if (m->casio_board.powered_on || !m->cpu.sleeping || !(m->chip.stbcr & STBCR_STANDBY)) return;
    m->casio_board.powered_on = true;
    m->cpu.sleeping = false;
}

static bool kernel_address(uint32_t address) {
    uint32_t segment = address & ~AREA_MASK;
    return segment == KSEG0_BASE || segment == KSEG1_BASE;
}

static bool rom_header_at(const uint8_t *image, size_t offset, size_t size, bool exact) {
    const uint8_t *header = image + offset;
    uint32_t first = read_le32(header + 8), last = read_le32(header + 12);
    if (exact && last - first != size) return false;
    uint32_t dll_first = read_le32(header), dll_last = read_le32(header + 4), modules = read_le32(header + 16);
    uint32_t ram_start = read_le32(header + 20), ram_free = read_le32(header + 24), ram_end = read_le32(header + 28);
    uint32_t span = last - first;
    return kernel_address(first) && !(first & AREA_MASK) && span >= ROM_DUMP_MIN_SPAN && span <= size && offset + ROM_HEADER_SIZE <= span &&
           dll_first < dll_last && modules && modules <= ROM_DUMP_MAX_MODULES &&
           kernel_address(ram_start) && ram_start <= ram_free && ram_free < ram_end;
}

static bool find_rom_header(const uint8_t *image, size_t size, uint32_t *physfirst, size_t *span) {
    for (int exact = 1; exact >= 0; exact--) {
        for (size_t offset = 0; offset + ROM_HEADER_SIZE <= size; offset += 4) {
            if (!rom_header_at(image, offset, size, exact)) continue;
            *physfirst = read_le32(image + offset + 8);
            *span = read_le32(image + offset + 12) - *physfirst;
            return true;
        }
    }
    return false;
}

static bool load_flash(machine_t *m, char *error, size_t error_size) {
    uint32_t physfirst;
    size_t span;
    if (!find_rom_header(m->image, m->image_size, &physfirst, &span) || (physfirst & AREA_MASK) + span > FLASH_SIZE) {
        snprintf(error, error_size, "not a Windows CE ROM image");
        return false;
    }
    m->flash = malloc(FLASH_SIZE);
    if (!m->flash) {
        snprintf(error, error_size, "out of memory");
        return false;
    }
    memset(m->flash, 0xFF, FLASH_SIZE);
    memcpy(m->flash + (physfirst & AREA_MASK), m->image, span);
    m->entry = RESET_VECTOR;
    return true;
}

static uint64_t hash_bytes(const uint8_t *data, size_t length) {
    uint64_t hash = 1469598103934665603ull;
    for (size_t i = 0; i < length; i++) hash = (hash ^ data[i]) * 1099511628211ull;
    return hash;
}

static bool mailbox_page(machine_t *m, uint32_t va, bool write, uint32_t *pa) {
    uint32_t page = va & ~0x3FFu;
    for (int i = 0; i < m->mailbox_page_count; i++) {
        if (m->mailbox_pages[i].page == page && (m->mailbox_pages[i].write || !write)) {
            *pa = m->mailbox_pages[i].pa | (va & 0x3FFu);
            return true;
        }
    }
    if (va >= 0x80000000u || !sh3_translate(&m->cpu, va, write, pa)) return false;
    if (m->mailbox_page_count < (int)MAILBOX_PAGES)
        m->mailbox_pages[m->mailbox_page_count++] = (mailbox_page_t){ page, *pa & ~0x3FFu, write };
    return true;
}

static bool mailbox_copy(void *context, uint32_t va, uint8_t *data, uint32_t length, bool write) {
    machine_t *m = context;
    uint32_t pa;
    if (!mailbox_page(m, va, write, &pa)) return false;
    pa &= AREA_MASK;
    if (pa - DRAM_PA >= m->dram_size || length > m->dram_size - (pa - DRAM_PA)) return false;
    if (write) memcpy(m->dram + (pa - DRAM_PA), data, length);
    else memcpy(data, m->dram + (pa - DRAM_PA), length);
    return true;
}

static bool on_trapa(void *context, uint32_t number) {
    machine_t *m = context;
    sh3_cpu_t *cpu = &m->cpu;
    if (number != MAILBOX_TRAPA || !sh3_user_mode(cpu)) return false;
    uint32_t pc = cpu->pc - 2;
    if (pc != m->mailbox_pc) {
        m->mailbox_pc = pc;
        m->mailbox_page_count = 0;
        m->mailbox_fault_tries = 0;
    }
    mailbox_call_t call = { .operation = cpu->r[4], .buffer = cpu->r[5], .length = cpu->r[6], .extra = cpu->r[1] };
    uint32_t fault_va;
    if (mailbox_trap(&m->mailbox, &call, mailbox_copy, m, &fault_va)) {
        if (call.operation == MAILBOX_RECV) m->agent_poll_at = cpu->cycles + 1;
        cpu->r[0] = call.result;
        cpu->r[1] = call.extra;
        m->mailbox_pc = 0;
        m->mailbox_page_count = 0;
        m->mailbox_fault_tries = 0;
        return true;
    }
    if (fault_va != m->mailbox_fault_va) m->mailbox_fault_tries = 0;
    m->mailbox_fault_va = fault_va;
    if (++m->mailbox_fault_tries > MAILBOX_FAULT_TRIES) {
        m->mailbox_fault_tries = 0;
        m->mailbox_pc = 0;
        cpu->r[0] = (uint32_t)-1;
        return true;
    }
    sh3_raise_memory_fault(cpu, fault_va, call.operation == MAILBOX_RECV);
    return true;
}

mailbox_t *machine_mailbox(machine_t *m) {
    return &m->mailbox;
}

static uint8_t *accel_map(void *context, uint32_t va, bool write) {
    machine_t *m = context;
    uint32_t pa;
    if (!ce_translate_current(&m->accel_ce, va, write, &pa)) return NULL;
    if (pa - DRAM_PA < DRAM_AREA_SIZE) return m->dram + (pa - DRAM_PA) % m->dram_size;
    if (!write && pa < FLASH_SIZE) return m->flash + pa;
    return NULL;
}

static const uint8_t *accel_rom(void *context, uint32_t pa, uint32_t length) {
    machine_t *m = context;
    return pa < FLASH_SIZE && length <= FLASH_SIZE - pa ? m->flash + pa : NULL;
}

static bool on_watch(void *context, uint32_t pc) {
    machine_t *m = context;
    if (accel_hooked(&m->accel, pc)) {
        accel_memory_t memory = { m, accel_map };
        return m->optimisations && accel_call(&m->accel, &m->cpu, &memory, pc);
    }
    machine_logf(m, "watch: pc %08X r4=%08X r5=%08X r6=%08X r7=%08X pr=%08X\n", pc, m->cpu.r[4], m->cpu.r[5], m->cpu.r[6], m->cpu.r[7], m->cpu.pr);
    return false;
}

static void sync_watches(machine_t *m) {
    memcpy(m->cpu.watch, m->watch, sizeof m->watch);
    m->cpu.watch_count = m->watch_count;
    for (int i = 0; i < m->accel.count && m->cpu.watch_count < SH3_WATCH_MAX; i++) m->cpu.watch[m->cpu.watch_count++] = m->accel.hooks[i].va;
}

static void trace_exception(void *context, uint32_t code, uint32_t pc, bool user) {
    machine_t *m = context;
    bool tlb = code == SH3_EXP_TLB_MISS_READ || code == SH3_EXP_TLB_MISS_WRITE || code == SH3_EXP_INITIAL_WRITE;
    bool api_call = code == SH3_EXP_ADDRESS_READ && m->cpu.tea >= 0xFFFF0000u && (m->cpu.tea & 1);
    if (tlb || api_call) return;
    machine_logf(m, "exception %03X at %08X user=%d tea=%08X pr=%08X r15=%08X\n", code, pc, user, m->cpu.tea, m->cpu.pr, m->cpu.r[15]);
}

static bool never_stop(void *context, uint32_t pc) {
    (void)context; (void)pc; return false;
}

void machine_trace_exceptions(machine_t *m, bool enabled) {
    m->exception_debug = (sh3_debug_t){ .context = m, .before = never_stop, .exception = trace_exception };
    m->cpu.debug = enabled ? &m->exception_debug : NULL;
}

static void apply_host_time(machine_t *m) {
    time_t now = time(NULL);
    struct tm local;
    localtime_r(&now, &local);
    int year = local.tm_year + 1900;
    sh7709_set_time(&m->chip, (year - 1970) % 100, local.tm_mon + 1, local.tm_mday, local.tm_wday, local.tm_hour, local.tm_min, local.tm_sec);
}

static void reset_machine(machine_t *m, bool keep_ram) {
    if (m->dram_size != m->dram_size_next || !m->dram) {
        free(m->dram);
        m->dram_size = m->dram_size_next;
        m->dram = calloc(1, m->dram_size);
        keep_ram = false;
    }
    if (!keep_ram) memset(m->dram, 0, m->dram_size);
    m->cpu.bus = (sh3_bus_t){ m, bus_read, bus_write, bus_fetch_page, m->dram, DRAM_PA, m->dram_size };
    m->cpu.on_watch = on_watch;
    m->cpu.on_trapa = on_trapa;
    m->cpu.on_interrupt = m->casio ? casio_nmi_taken : NULL;
    uint64_t cycles = m->cpu.cycles;
    sh3_reset(&m->cpu);
    if (keep_ram) {
        m->cpu.cycles = cycles;
        m->cpu.expevt = SH3_EXP_MANUAL_RESET;
    }
    mailbox_clear(&m->mailbox);
    m->mailbox_pc = 0;
    m->mailbox_page_count = 0;
    m->agent_poll_at = 0;
    m->cpu.pc = m->entry;
    sync_watches(m);
    if (keep_ram) {
        sh7709_reset(&m->chip);
    } else {
        sh7709_init(&m->chip, &m->cpu, m->hp ? SH7709 : SH7708, MACHINE_CLOCK_HZ, m->hp ? HP320LX_PERIPHERAL_HZ : CASIO_PERIPHERAL_HZ);
        sh7709_set_time(&m->chip, 2000 - 1970, 1, 1, 6, 0, 0, 0);
        if (m->host_clock) apply_host_time(m);
    }
    if (m->casio) sh7709_set_scif_alias(&m->chip, CASIO_SCIF_PA, 0);
    casio_reset(&m->casio_board);
    memset(&m->casio_audio, 0, sizeof m->casio_audio);
    hp320lx_reset(&m->hp_board);
    if (m->hp) {
        for (int channel = 0; channel < 4; channel++) sh7709_set_adc(&m->chip, channel, HP320LX_ADC_HEALTHY);
        sh7709_set_port_input(&m->chip, HP320LX_MODEL_PORT, HP320LX_MODEL_PINS, HP320LX_MODEL_PINS);
        sh7709_set_port_input(&m->chip, HP320LX_POWER_PORT, HP320LX_POWER_AC, HP320LX_POWER_AC);
        sh7709_set_irq_active_high(&m->chip, 1u << HP320LX_PEN_IRQ);
    }
    m->chip.transmit = onchip_transmit;
    m->chip.transmit_context = m;
    m->audio_count = 0;
    m->audio_last = 0;
    m->audio_phase = 0;
    if (m->hp) {
        m->chip.dac_written = hp_dac_written;
        m->chip.ports_written = hp_ports_written;
        hp_ports_written(m);
    }
    cfcard_reset(&m->card_slot);
    set_serial_lines(m);
    m->serial_rx_count = m->serial_tx_count = 0;
}

machine_t *machine_create(const uint8_t *rom, size_t rom_size, char *error, size_t error_size) {
    machine_t *m = calloc(1, sizeof *m);
    if (!m) return NULL;
    m->image = malloc(rom_size);
    memcpy(m->image, rom, rom_size);
    m->image_size = rom_size;
    m->rom_hash = hash_bytes(rom, rom_size);
    m->hp = hp320lx_detect(rom, rom_size);
    m->casio = !m->hp;
    m->hp_host = (hp320lx_host_t){ hp_trace, board_debug_line, m };
    m->casio_host = (casio_host_t){ casio_trace, casio_cycles, casio_irl, casio_onchip, MACHINE_CLOCK_HZ, CASIO_TIMER_HZ, &m->card_slot, &m->casio_audio, casio_read_memory, casio_samples, m };
    m->card_slot.state = &m->card;
    if (!load_flash(m, error, error_size)) {
        machine_destroy(m);
        return NULL;
    }
    accel_find(accel_rom, m, &m->accel);
    ce_init(&m->accel_ce, m);
    m->dram_size_next = DRAM_DEFAULT_SIZE;
    reset_machine(m, false);
    return m;
}

void machine_destroy(machine_t *m) {
    if (!m) return;
    mailbox_clear(&m->mailbox);
    cfcard_eject(&m->card_slot);
    free(m->dram);
    free(m->dictionary);
    free(m->flash);
    free(m->image);
    free(m);
}

void machine_set_log(machine_t *m, machine_log_fn log) {
    m->log = log;
}

static void pending_card_event(machine_t *m);

static uint64_t next_event(machine_t *m) {
    uint64_t next = sh7709_next_event(&m->chip);
    if (m->casio) {
        uint64_t board_next = casio_next_event(&m->casio_board, &m->casio_host);
        if (board_next < next) next = board_next;
    }
    if (m->serial_rx_count && m->serial_tick_at < next) next = m->serial_tick_at;
    if (m->pending_card_at && m->pending_card_at < next) next = m->pending_card_at;
    return next;
}

void machine_run(machine_t *m, uint64_t cycles) {
    uint64_t target = m->cpu.cycles + cycles;
    m->run_target = target;
    while (m->cpu.cycles < target) {
        sh7709_advance(&m->chip);
        pending_card_event(m);
        serial_tick_event(m);
        if (m->casio) {
            casio_update(&m->casio_board, &m->casio_host);
            casio_first_wake(m);
        }
        uint64_t next = next_event(m);
        uint64_t until = next < target ? next : target;
        if (until <= m->cpu.cycles) until = m->cpu.cycles + 1;
        sh3_run(&m->cpu, until);
        if (m->cpu.debug && m->cpu.debug->stop) break;
    }
    sh7709_advance(&m->chip);
}

sh3_cpu_t *machine_cpu(machine_t *m) {
    return &m->cpu;
}

bool machine_read_physical(machine_t *m, uint32_t pa, uint8_t *data, uint32_t length) {
    pa &= AREA_MASK;
    if (pa < FLASH_SIZE && length <= FLASH_SIZE - pa) {
        memcpy(data, m->flash + pa, length);
        return true;
    }
    if (pa - DRAM_PA >= m->dram_size || length > m->dram_size - (pa - DRAM_PA)) return false;
    memcpy(data, m->dram + (pa - DRAM_PA), length);
    return true;
}

bool machine_write_physical(machine_t *m, uint32_t pa, const uint8_t *data, uint32_t length) {
    pa &= AREA_MASK;
    if (pa - DRAM_PA >= m->dram_size || length > m->dram_size - (pa - DRAM_PA)) return false;
    memcpy(m->dram + (pa - DRAM_PA), data, length);
    return true;
}

uint64_t machine_cycles(machine_t *m) {
    return m->cpu.cycles;
}

bool machine_agent_running(machine_t *m) {
    return m->agent_poll_at && m->cpu.cycles < m->agent_poll_at + MACHINE_CLOCK_HZ / 2;
}
uint32_t machine_pc(machine_t *m) {
    return m->cpu.pc;
}

bool machine_lcd_enabled(machine_t *m) {
    return !machine_suspended(m);
}
bool machine_backlight(machine_t *m) {
    if (m->casio) return machine_lcd_enabled(m) && casio_backlight(&m->casio_board);
    return machine_lcd_enabled(m) && !(m->chip.ports[HP320LX_BACKLIGHT_PORT] & HP320LX_BACKLIGHT_OFF);
}

uint32_t machine_backlight_colour(machine_t *m) {
    return m->hp ? HP320LX_BACKLIGHT_COLOUR : CASIO_BACKLIGHT_COLOUR;
}

void machine_backlight_button(machine_t *m, bool down) {
    machine_key(m, SCANCODE_BACKLIGHT, !down);
}

screen_size_t machine_screen_size(machine_t *m) {
    if (m->casio) return (screen_size_t){ CASIO_SCREEN_WIDTH, CASIO_SCREEN_HEIGHT };
    return (screen_size_t){ HP320LX_SCREEN_WIDTH, HP320LX_SCREEN_HEIGHT };
}

screen_size_t machine_screen_next(machine_t *m) {
    return machine_screen_size(m);
}

bool machine_screen_supported(machine_t *m, screen_size_t size) {
    screen_size_t stock = machine_screen_size(m);
    return size.width == stock.width && size.height == stock.height;
}

bool machine_set_screen(machine_t *m, screen_size_t size) {
    return machine_screen_supported(m, size);
}

int machine_screen_palette(machine_t *m, uint32_t *palette) {
    (void)m;
    (void)palette;
    return 0;
}

bool machine_screen(machine_t *m, uint8_t *levels) {
    if (m->casio) {
        casio_screen(&m->casio_board, levels);
        return true;
    }
    uint32_t offset = HP320LX_FRAMEBUFFER - DRAM_PA;
    if (offset + HP320LX_SCREEN_WIDTH / 4 * HP320LX_SCREEN_HEIGHT > m->dram_size) return false;
    hp320lx_screen(m->dram + offset, levels);
    return true;
}

void machine_key(machine_t *m, uint8_t scancode, bool up) {
    if (m->casio) {
        casio_key(&m->casio_board, &m->casio_host, scancode, up);
        return;
    }
    if (m->hp) {
        if (!up && machine_suspended(m)) {
            m->hp_on_key = true;
            hp320lx_woken(&m->hp_board);
        }
        if (m->hp_on_key) {
            sh7709_set_irq(&m->chip, HP320LX_ON_IRQ, !up);
            if (up) m->hp_on_key = false;
            return;
        }
        if (hp320lx_key(&m->hp_board, scancode, up)) hp_ports_written(m);
    }
}

void machine_touch(machine_t *m, bool down, int x, int y) {
    if (m->casio) {
        casio_touch(&m->casio_board, &m->casio_host, down, x, y);
        return;
    }
    if (m->hp) {
        hp320lx_touch(&m->hp_board, down, x, y);
        hp_ports_written(m);
    }
}
bool machine_suspended(machine_t *m) {
    if (m->casio && (m->cpu.sr & SH3_SR_IMASK) != SH3_SR_IMASK) return false;
    return m->cpu.sleeping && (m->chip.stbcr & STBCR_STANDBY);
}

static bool insert_card_now(machine_t *m, const char *path) {
    FILE *image = fopen(path, "r+b");
    if (!image) return false;
    cfcard_insert(&m->card_slot, image);
    snprintf(m->card_path, sizeof m->card_path, "%s", path);
    if (m->casio) casio_card_changed(&m->casio_board, &m->casio_host);
    return true;
}

bool machine_insert_card(machine_t *m, const char *path) {
    if (m->card.inserted && !strcmp(path, m->card_path)) return true;
    if (!m->card.inserted) return insert_card_now(m, path);
    FILE *probe = fopen(path, "r+b");
    if (!probe) return false;
    fclose(probe);
    machine_eject_card(m);
    snprintf(m->pending_card, sizeof m->pending_card, "%s", path);
    m->pending_card_at = m->cpu.cycles + MACHINE_CLOCK_HZ;
    return true;
}

static void pending_card_event(machine_t *m) {
    if (!m->pending_card_at || m->cpu.cycles < m->pending_card_at) return;
    m->pending_card_at = 0;
    if (!insert_card_now(m, m->pending_card)) machine_logf(m, "card: cannot open %s\n", m->pending_card);
}

void machine_eject_card(machine_t *m) {
    m->pending_card_at = 0;
    if (!m->card.inserted) return;
    cfcard_eject(&m->card_slot);
    m->card_path[0] = 0;
    if (m->casio) casio_card_changed(&m->casio_board, &m->casio_host);
}

bool machine_card_inserted(machine_t *m) {
    return m->card.inserted;
}

void machine_serial_connect(machine_t *m, bool connected) {
    m->serial_connected = connected;
    if (!connected) m->serial_rx_count = m->serial_tx_count = 0;
    set_serial_lines(m);
}

bool machine_serial_connected(machine_t *m) {
    return m->serial_connected;
}

bool machine_serial_dtr(machine_t *m) {
    if (m->hp) return !(m->chip.ports[HP320LX_SERIAL_CONTROL_PORT] & HP320LX_SERIAL_DTR);
    return m->serial_connected;
}

uint32_t machine_serial_baud(machine_t *m) {
    if (m->casio) return casio_serial_baud(&m->casio_board);
    return sh7709_baud(&m->chip, HP320LX_SERIAL_SCIF);
}

size_t machine_serial_space(machine_t *m) {
    return SERIAL_FIFO - m->serial_rx_count;
}

size_t machine_serial_send(machine_t *m, const uint8_t *data, size_t length) {
    size_t accepted = 0;
    while (m->serial_connected && accepted < length && m->serial_rx_count < SERIAL_FIFO) {
        m->serial_rx[(m->serial_rx_head + m->serial_rx_count) % SERIAL_FIFO] = data[accepted++];
        m->serial_rx_count++;
    }
    return accepted;
}

size_t machine_serial_take(machine_t *m, uint8_t *out, size_t max) {
    size_t count = m->serial_tx_count < max ? m->serial_tx_count : max;
    memcpy(out, m->serial_tx, count);
    memmove(m->serial_tx, m->serial_tx + count, m->serial_tx_count - count);
    m->serial_tx_count -= (uint32_t)count;
    return count;
}

void machine_reset(machine_t *m) {
    reset_machine(m, false);
}

bool machine_has_dictionary_slot(machine_t *m) {
    return m->casio;
}

bool machine_mount_dictionary(machine_t *m, const char *path) {
    if (!m->casio) return false;
    size_t size;
    uint8_t *image = file_read(path, &size);
    if (!image || !size || size > DICTIONARY_MAX) {
        free(image);
        return false;
    }
    free(m->dictionary);
    m->dictionary = image;
    m->dictionary_size = size;
    return true;
}

void machine_unmount_dictionary(machine_t *m) {
    free(m->dictionary);
    m->dictionary = NULL;
    m->dictionary_size = 0;
}

bool machine_dictionary_mounted(machine_t *m) {
    return m->dictionary != NULL;
}

void machine_soft_reset(machine_t *m) {
    reset_machine(m, true);
}

bool machine_watch_pc(machine_t *m, uint32_t va) {
    if (m->watch_count >= MACHINE_WATCH_MAX) return false;
    m->watch[m->watch_count++] = va;
    sync_watches(m);
    return true;
}

void machine_set_memory(machine_t *m, uint32_t megabytes) {
    if (megabytes != 16 && megabytes != 32 && megabytes != 64) return;
    m->dram_size_next = megabytes << 20;
    if (m->dram_size_next != m->dram_size) machine_reset(m);
}

uint32_t machine_memory(machine_t *m) {
    return m->dram_size >> 20;
}
uint32_t machine_memory_next(machine_t *m) {
    return m->dram_size_next >> 20;
}
void machine_set_speed(machine_t *m, uint32_t multiplier) {
    m->cpu.speed = multiplier ? multiplier : 1;
}
uint32_t machine_speed(machine_t *m) {
    return m->cpu.speed ? m->cpu.speed : 1;
}
void machine_set_optimisations(machine_t *m, bool optimisations) {
    m->optimisations = optimisations;
    m->cpu.fast_divide = optimisations;
}
bool machine_optimisations(machine_t *m) {
    return m->optimisations;
}
uint64_t machine_rom_hash(machine_t *m) {
    return m->rom_hash;
}
void machine_power_button(machine_t *m, bool down) {
    if (m->hp && down && machine_suspended(m)) hp320lx_woken(&m->hp_board);
    if (m->hp) sh7709_set_irq(&m->chip, HP320LX_ON_IRQ, down);
    if (!m->casio) return;
    casio_power_key(&m->casio_board, down);
    if (!down) return;
    if (!m->casio_board.powered_on) {
        casio_first_wake(m);
        return;
    }
    m->chip.nmi = true;
    sh7709_update_interrupts(&m->chip);
}

int machine_rom_system(machine_t *m) {
    return m->casio ? MACHINE_BOARD_CASIO : MACHINE_BOARD_HP;
}

const char *machine_board_name(int board) {
    switch (board) {
    case MACHINE_BOARD_CASIO: return "Casio A-51";
    case MACHINE_BOARD_HP: return "HP 320LX";
    default: return "Unknown";
    }
}
key_layout_t machine_key_layout(machine_t *m) {
    (void)m; return KEY_LAYOUT_ROM;
}

void machine_set_host_clock(machine_t *m, bool enabled) {
    m->host_clock = enabled;
    if (enabled) apply_host_time(m);
}

void machine_advance_clock(machine_t *m, int64_t seconds) {
    if (seconds > 0) sh7709_add_seconds(&m->chip, (uint32_t)(seconds > 86400 * 366 ? 86400 * 366 : seconds));
}

void machine_set_debug_output(machine_t *m, machine_debug_fn sink, void *context) {
    m->debug_sink = sink;
    m->debug_context = context;
}

void machine_dump_state(machine_t *m) {
    sh3_cpu_t *cpu = &m->cpu;
    machine_logf(m, "pc=%08X pr=%08X sr=%08X spc=%08X ssr=%08X vbr=%08X expevt=%03X intevt=%03X tea=%08X mmucr=%08X\n",
                 cpu->pc, cpu->pr, cpu->sr, cpu->spc, cpu->ssr, cpu->vbr, cpu->expevt, cpu->intevt, cpu->tea, cpu->mmucr);
    for (int i = 0; i < 16; i += 4)
        machine_logf(m, "r%-2d=%08X r%-2d=%08X r%-2d=%08X r%-2d=%08X\n", i, cpu->r[i], i + 1, cpu->r[i + 1], i + 2, cpu->r[i + 2], i + 3, cpu->r[i + 3]);
    machine_logf(m, "cycles=%llu sleeping=%d irl=%u\n", (unsigned long long)cpu->cycles, cpu->sleeping, m->chip.irl_level);
}

#define STATE_FIELDS(X) \
    X(cpu, m->cpu) X(chip, m->chip) X(card, m->card) X(card_path, m->card_path) X(casio_board, m->casio_board) X(casio_audio, m->casio_audio) X(hp_board, m->hp_board)

static bool write_bytes(gzFile file, const void *data, uint32_t size) {
    return size == 0 || gzwrite(file, data, size) == (int)size;
}

static bool write_record(gzFile file, const char *name, const void *data, uint32_t size) {
    uint8_t length = (uint8_t)strlen(name);
    return write_bytes(file, &length, 1) && write_bytes(file, name, length) && write_bytes(file, &size, sizeof size) && write_bytes(file, data, size);
}

bool machine_save(machine_t *m, const char *path, int64_t host_time) {
    char temporary[1100];
    int written = snprintf(temporary, sizeof temporary, "%s.tmp", path);
    if (written < 0 || (size_t)written >= sizeof temporary) return false;
    gzFile file = gzopen(temporary, "wb1");
    if (!file) return false;
    bool ok = write_bytes(file, STATE_MAGIC, sizeof STATE_MAGIC) && write_bytes(file, &m->rom_hash, sizeof m->rom_hash) &&
              write_bytes(file, &host_time, sizeof host_time);
#define SAVE_FIELD(key, field) ok = ok && write_record(file, #key, &(field), (uint32_t)sizeof(field));
    STATE_FIELDS(SAVE_FIELD)
#undef SAVE_FIELD
    ok = ok && write_record(file, "dram", m->dram, m->dram_size);
    uint8_t end = 0;
    ok = ok && write_bytes(file, &end, 1);
    ok = gzclose(file) == Z_OK && ok;
    if (ok) ok = rename(temporary, path) == 0;
    else remove(temporary);
    return ok;
}

static uint8_t *read_state(const char *path, size_t *length) {
    gzFile file = gzopen(path, "rb");
    if (!file) return NULL;
    size_t capacity = 1 << 20, used = 0;
    uint8_t *data = malloc(capacity);
    while (data) {
        if (used == capacity) {
            uint8_t *grown = realloc(data, capacity * 2);
            if (!grown) { free(data); data = NULL; break; }
            data = grown;
            capacity *= 2;
        }
        int got = gzread(file, data + used, (unsigned)(capacity - used));
        if (got <= 0) break;
        used += (size_t)got;
    }
    gzclose(file);
    *length = used;
    return data;
}

static bool state_header_matches(const machine_t *m, const uint8_t *data, size_t length) {
    return length >= sizeof STATE_MAGIC + 16 && !memcmp(data, STATE_MAGIC, sizeof STATE_MAGIC) &&
           !memcmp(data + sizeof STATE_MAGIC, &m->rom_hash, 8);
}

static bool state_fields_match(machine_t *m, const uint8_t *data, size_t length) {
    const uint8_t *cursor = data + sizeof STATE_MAGIC + 16, *end = data + length;
    while (cursor < end) {
        uint8_t name_length = *cursor++;
        if (!name_length || end - cursor < name_length + 4) break;
        char name[256];
        memcpy(name, cursor, name_length);
        name[name_length] = 0;
        cursor += name_length;
        uint32_t size;
        memcpy(&size, cursor, 4);
        cursor += 4;
        if ((size_t)(end - cursor) < size) return false;
#define CHECK_FIELD(key, field) if (!strcmp(name, #key) && size != sizeof(field)) return false;
        STATE_FIELDS(CHECK_FIELD)
#undef CHECK_FIELD
        cursor += size;
    }
    return true;
}

static bool state_usable(machine_t *m, const uint8_t *data, size_t length) {
    return state_header_matches(m, data, length) && state_fields_match(m, data, length);
}

bool machine_state_matches(machine_t *m, const char *path) {
    size_t length;
    uint8_t *data = read_state(path, &length);
    bool matches = data && state_usable(m, data, length);
    free(data);
    return matches;
}

bool machine_load(machine_t *m, const char *path, int64_t *host_time) {
    size_t length;
    uint8_t *data = read_state(path, &length);
    if (!data) return false;
    if (!state_usable(m, data, length)) {
        free(data);
        return false;
    }
    if (host_time) memcpy(host_time, data + sizeof STATE_MAGIC + 8, 8);
    m->agent_poll_at = 0;
    sh3_debug_t *debug = m->cpu.debug;
    const uint8_t *cursor = data + sizeof STATE_MAGIC + 16, *end = data + length;
    while (cursor < end) {
        uint8_t name_length = *cursor++;
        if (!name_length || end - cursor < name_length + 4) break;
        char name[256];
        memcpy(name, cursor, name_length);
        name[name_length] = 0;
        cursor += name_length;
        uint32_t size;
        memcpy(&size, cursor, 4);
        cursor += 4;
        if ((size_t)(end - cursor) < size) break;
#define LOAD_FIELD(key, field) if (!strcmp(name, #key) && size == sizeof(field)) memcpy(&(field), cursor, size);
        STATE_FIELDS(LOAD_FIELD)
#undef LOAD_FIELD
        if (!strcmp(name, "dram")) {
            if (size != m->dram_size) {
                free(m->dram);
                m->dram = calloc(1, size);
                m->dram_size = m->dram_size_next = size;
            }
            memcpy(m->dram, cursor, size);
        }
        cursor += size;
    }
    free(data);
    m->cpu.bus = (sh3_bus_t){ m, bus_read, bus_write, bus_fetch_page, m->dram, DRAM_PA, m->dram_size };
    m->cpu.debug = debug;
    m->cpu.on_watch = on_watch;
    m->cpu.on_trapa = on_trapa;
    mailbox_clear(&m->mailbox);
    m->cpu.on_interrupt = m->casio ? casio_nmi_taken : NULL;
    sync_watches(m);
    m->chip.cpu = &m->cpu;
    cfcard_sanitize(&m->card);
    FILE *image = m->card.inserted && m->card_path[0] ? fopen(m->card_path, "r+b") : NULL;
    cfcard_rebind(&m->card_slot, image);
    if (!image) m->card_path[0] = 0;
    m->chip.transmit = onchip_transmit;
    m->chip.transmit_context = m;
    m->chip.ports_written = m->hp ? hp_ports_written : NULL;
    m->chip.dac_written = m->hp ? hp_dac_written : NULL;
    m->audio_count = 0;
    m->audio_last = 0;
    sh3_flush_translations(&m->cpu);
    return true;
}
