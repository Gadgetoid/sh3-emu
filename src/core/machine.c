#include "core/machine.h"

#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <zlib.h>

#include "core/autopc.h"
#include "core/casio.h"
#include "core/cfcard.h"
#include "core/mailbox.h"
#include "core/ppfs.h"
#include "core/sh7709.h"

#define DRAM_PA            0x0C000000u
#define DRAM_DEFAULT_SIZE  (16u << 20)
#define DRAM_AREA_SIZE     (64u << 20)
#define AREA_MASK          0x1FFFFFFFu
#define FLASH_SIZE         (16u << 20)
#define RESET_VECTOR       0xA0000000u
#define P4_ROUTINES_END    0xE0010000u
#define OP_RTS             0x000Bu
#define OP_NOP             0x0009u
#define ROM_HEADER_SIZE    0x54u

#define HKEEP_PA           0x04000000u
#define HKEEP_END          0x08000000u
#define LED_DISCRETE_PA    0x04040000u
#define LED_ALPHA_PA       0x04060000u
#define PARALLEL_PA        0x04020000u

#define ASIC_PA            0x10000000u
#define ASIC_SIZE          0x00100000u
#define ASIC_CPU_STATUS    0x0400u
#define ASIC_PCMCIA_CONTROL0 0x0410u
#define ASIC_PCMCIA_INTR0  0x0414u
#define ASIC_PCMCIA_CONTROL1 0x0418u
#define ASIC_PCMCIA_INTR1  0x041Cu
#define ASIC_CPU_ISR       0x0800u
#define ASIC_CPU_MR        0x0804u
#define ASIC_DISPLAY       0x1000u
#define ASIC_DEBUG_SERIAL  0x2000u
#define ASIC_PRODUCT_SERIAL 0x4000u
#define ASIC_IR_SERIAL     0x8000u
#define ASIC_KEYBOARD      0xC000u
#define ASIC_TOUCH_SOUND   0xA000u
#define ASIC_DMA_BASE      0x0810u
#define DMA_SLOT_STRIDE    0x20000u

#define SLOT_DEBUG_SERIAL  0
#define SLOT_DISPLAY       1
#define SLOT_IR            3
#define SLOT_PRODUCT_SERIAL 4

#define INTR_KEYBOARD      0x0040u
#define INTR_TOUCH_AUDIO   0x0020u

#define TOUCH_ADC_CNTR     0x00u
#define TOUCH_ADC_STR      0x04u
#define TOUCH_UCB_CNTR     0x08u
#define TOUCH_UCB_STR      0x0Cu
#define TOUCH_UCB_REGISTER 0x10u
#define TOUCH_SOUND_CNTR   0x14u
#define TOUCH_SOUND_STR    0x18u
#define TOUCH_INTR_MASK    0x1Cu

#define ADC_PEN_INTR       0x0010u
#define ADC_UCB_INTR       0x0008u
#define ADC_TIMING_INTR    0x0004u
#define ADC_DO_SAMPLE      0x4000u
#define ADC_SELECT_Y       0x0800u
#define ADC_TIMING_ENABLE  0x0400u
#define ADC_SAMPLE_MASK    0x0FFFu
#define UCB_REG_INTR       0x0001u
#define UCB_WRITE          0x0010u
#define UCB_PEN_STATE      0x1000u
#define SOUND_STR_MASK     0xF000u
#define MASK_REG           0x0001u
#define MASK_SOUND         0x0002u
#define MASK_TIMING        0x0004u
#define MASK_UCB           0x0008u
#define MASK_PEN           0x0010u
#define TOUCH_SCALE        4
#define PEN_TIMER_CYCLES   (MACHINE_CLOCK_HZ / 200)
#define BOARD_IRL_LEVEL    4u
#define AUTOPC_IRL_LEVEL   8u

#define DISP_LCD_ON        0x0004u

#define PCMCIA_PA          0x14000000u
#define PCMCIA_AREA6_PA    0x18000000u
#define PCMCIA_END         0x1C000000u
#define PCMCIA_NO_CARD     0x000Cu
#define PCMCIA_CARD_INTR   0x0002u
#define PCMCIA_STATE_INTR  0x0001u
#define PCMCIA_RESET       0x0020u
#define PCMCIA_SLOT_SHIFT  23
#define PCMCIA_OFFSET_MASK 0x007FFFFFu
#define PCMCIA_STATUS      0xFF00u
#define INTR_SYSTEM        0x0001u
#define BCR2_INDEX         1
#define CARD_PATH_MAX      1024

#define SERA_RX_CHARACTER_INTR 0x8000u
#define SERA_RX_CHANGED_INTR 0x0400u
#define SERA_RI            0x0080u
#define SERA_DSR           0x0040u
#define SERA_TX_INTR       0x0010u
#define SERA_CTS           0x0004u
#define SERA_CD            0x0002u
#define SERA_W1C_MASK      0xF618u
#define SERA_RW_MASK       0x0901u
#define SERA_LINES         (SERA_RI | SERA_DSR | SERA_CTS | SERA_CD)
#define SERB_RX_EN         0x4000u
#define SERB_TX_EN         0x2000u
#define SERB_TX_STOP_AT_PAGE 0x1000u
#define SERB_DTR           0x0100u
#define INTR_PRODUCT_SERIAL 0x0008u
#define SERIAL_TX_PAGE     0x800u
#define SERIAL_RX_RING     0x1000u
#define SERIAL_FIFO        16384
#define SERIAL_BYTES_PER_TICK 12
#define SERIAL_TICK_CYCLES (MACHINE_CLOCK_HZ / 1000)

#define KB_RDRF            0x0001u
#define KB_CLK_EN          0x8000u
#define KB_CSR_READ_ONLY   0x07FFu
#define KEY_FIFO           64

#define UNKNOWN_SEEN       4096

#define STATE_MAGIC        "SH3ODO01"

#define MAILBOX_FAULT_TRIES 4
#define MAILBOX_PAGES      ((MAILBOX_MESSAGE_MAX >> 10) + 2)

typedef struct {
    uint32_t page, pa;
    bool     write;
} mailbox_page_t;

typedef struct {
    uint16_t csr_a, csr_b;
    char     line[256];
    int      length;
} p2_serial_t;

struct machine {
    sh3_cpu_t cpu;
    sh7709_t  chip;
    uint8_t  *dram;
    uint32_t  dram_size;
    uint32_t  dram_size_next;
    uint8_t  *image;
    size_t    image_size;
    uint32_t  entry;
    uint64_t  rom_hash;
    bool      autopc;
    bool      casio;
    uint8_t  *flash;
    casio_t   casio_board;
    casio_host_t casio_host;
    p2_serial_t sci_line;
    autopc_t  board;
    autopc_host_t board_host;

    uint16_t  asic[ASIC_SIZE / 2];
    cfcard_t  card;
    cfcard_slot_t card_slot;
    char      card_path[CARD_PATH_MAX];
    char      pending_card[CARD_PATH_MAX];
    uint64_t  pending_card_at;
    uint16_t  pcmcia_state;
    uint32_t  cpu_isr, cpu_mr;
    uint16_t  display_csr, display_xsize, display_ysize;
    p2_serial_t serial[3];
    bool      serial_connected;
    uint8_t   serial_rx[SERIAL_FIFO], serial_tx[SERIAL_FIFO];
    uint32_t  serial_rx_head, serial_rx_count, serial_tx_count;
    uint64_t  serial_tick_at;
    uint16_t  keyboard_csr, keyboard_isr;
    uint8_t   key_fifo[KEY_FIFO];
    uint32_t  key_head, key_count;
    uint32_t  led_discrete, led_alpha;
    ppfs_t    ppfs;

    uint16_t  adc_cntr, adc_str, ucb_cntr, ucb_str, ucb_register, sound_cntr, sound_str, touch_mask;
    uint16_t  ucb_regs[16];
    uint16_t  adc_x, adc_y;
    bool      pen_down;
    uint64_t  pen_timer_at;

    mailbox_t mailbox;
    uint32_t  mailbox_fault_va;
    int       mailbox_fault_tries;
    uint32_t  mailbox_pc;
    mailbox_page_t mailbox_pages[MAILBOX_PAGES];
    int       mailbox_page_count;

    machine_log_fn log;
    sh3_debug_t exception_debug;
    machine_debug_fn debug_sink;
    void     *debug_context;
    uint32_t  unknown_logged;
    uint64_t  unknown_seen[UNKNOWN_SEEN];
    bool      host_clock;
    uint32_t  watch[MACHINE_WATCH_MAX];
    int       watch_count;
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
    if (m->autopc || m->casio ? unknown_seen(m, what, pa) : m->unknown_logged >= 200) return;
    m->unknown_logged++;
    machine_logf(m, "%s %08X (%d) = %08X at pc %08X\n", what, pa, size, value, m->cpu.pc);
}

static void autopc_debug_line(void *context, const char *line) {
    machine_t *m = context;
    if (m->debug_sink) m->debug_sink(m->debug_context, line);
}

static void autopc_trace(void *context, bool write, uint32_t pa, int size, uint32_t value) {
    note_unknown(context, write ? "write faceplate" : "read  faceplate", pa, size, value);
}

static void casio_trace(void *context, bool write, uint32_t pa, int size, uint32_t value) {
    note_unknown(context, write ? "write board" : "read  board", pa, size, value);
}

static uint64_t casio_cycles(void *context) {
    return ((machine_t *)context)->cpu.cycles;
}

static void autopc_irl(void *context, bool asserted) {
    machine_t *m = context;
    sh7709_set_irl(&m->chip, asserted ? AUTOPC_IRL_LEVEL : 0);
}

static uint32_t read_le32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint16_t asic_get(const machine_t *m, uint32_t offset) {
    return m->asic[(offset & (ASIC_SIZE - 1)) >> 1];
}

static uint32_t dma_pointer(const machine_t *m, int slot, bool transmit) {
    uint32_t base = ASIC_DMA_BASE + (uint32_t)slot * DMA_SLOT_STRIDE + (transmit ? 0x10000u : 0);
    return DRAM_PA + (((uint32_t)asic_get(m, base + 4) & 0xFFu) << 16 | asic_get(m, base));
}

static void update_board_interrupt(machine_t *m) {
    sh7709_set_irl(&m->chip, (m->cpu_isr & m->cpu_mr) ? BOARD_IRL_LEVEL : 0);
}

static void set_board_source(machine_t *m, uint32_t bit, bool asserted) {
    if (asserted) m->cpu_isr |= bit;
    else m->cpu_isr &= ~bit;
    update_board_interrupt(m);
}

static uint8_t read_dram_byte(const machine_t *m, uint32_t pa) {
    if (pa - DRAM_PA < m->dram_size) return m->dram[pa - DRAM_PA];
    return 0;
}

static void debug_character(machine_t *m, p2_serial_t *serial, uint8_t ch) {
    if (ch == '\r') return;
    if (ch == '\n' || serial->length >= (int)sizeof serial->line - 1) {
        serial->line[serial->length] = 0;
        if (m->debug_sink) m->debug_sink(m->debug_context, serial->line);
        serial->length = 0;
        if (ch == '\n') return;
    }
    serial->line[serial->length++] = (char)(ch >= 0x20 && ch < 0x7F ? ch : '?');
}

static p2_serial_t *serial_for(machine_t *m, uint32_t offset, int *slot) {
    uint32_t block = offset & ~0xFFFu;
    if (block == ASIC_DEBUG_SERIAL) { *slot = SLOT_DEBUG_SERIAL; return &m->serial[0]; }
    if (block == ASIC_PRODUCT_SERIAL) { *slot = SLOT_PRODUCT_SERIAL; return &m->serial[1]; }
    if (block == ASIC_IR_SERIAL) { *slot = SLOT_IR; return &m->serial[2]; }
    return NULL;
}

static void update_product_serial_interrupt(machine_t *m) {
    set_board_source(m, INTR_PRODUCT_SERIAL, (m->serial[1].csr_a & SERA_W1C_MASK) != 0);
}

static void set_serial_lines(machine_t *m) {
    p2_serial_t *serial = &m->serial[1];
    uint16_t lines = (uint16_t)(SERA_RI | (m->serial_connected ? 0 : SERA_DSR | SERA_CTS | SERA_CD));
    if ((serial->csr_a & SERA_LINES) == lines) return;
    serial->csr_a = (uint16_t)((serial->csr_a & ~SERA_LINES) | lines | SERA_RX_CHANGED_INTR);
    update_product_serial_interrupt(m);
}

static void set_dma_low(machine_t *m, int slot, bool transmit, uint16_t value) {
    uint32_t offset = ASIC_DMA_BASE + (uint32_t)slot * DMA_SLOT_STRIDE + (transmit ? 0x10000u : 0);
    m->asic[(offset & (ASIC_SIZE - 1)) >> 1] = value;
}

static void product_serial_transmit(machine_t *m, p2_serial_t *serial) {
    uint32_t pa = dma_pointer(m, SLOT_PRODUCT_SERIAL, true);
    uint32_t end = (serial->csr_b & SERB_TX_STOP_AT_PAGE) ? (pa | (SERIAL_TX_PAGE - 1)) + 1 : pa + 1;
    for (; pa < end; pa++) {
        if (m->serial_connected && m->serial_tx_count < SERIAL_FIFO) m->serial_tx[m->serial_tx_count++] = read_dram_byte(m, pa);
    }
    uint32_t low = asic_get(m, ASIC_DMA_BASE + SLOT_PRODUCT_SERIAL * DMA_SLOT_STRIDE + 0x10000u);
    set_dma_low(m, SLOT_PRODUCT_SERIAL, true, (uint16_t)(low + (end - dma_pointer(m, SLOT_PRODUCT_SERIAL, true))));
    serial->csr_a |= SERA_TX_INTR;
    update_product_serial_interrupt(m);
}

static void product_serial_receive(machine_t *m) {
    p2_serial_t *serial = &m->serial[1];
    if (!m->serial_rx_count || !(serial->csr_b & SERB_RX_EN)) return;
    uint32_t base = ASIC_DMA_BASE + SLOT_PRODUCT_SERIAL * DMA_SLOT_STRIDE;
    for (int i = 0; i < SERIAL_BYTES_PER_TICK && m->serial_rx_count; i++) {
        uint32_t pa = dma_pointer(m, SLOT_PRODUCT_SERIAL, false);
        if (pa - DRAM_PA < m->dram_size) m->dram[pa - DRAM_PA] = m->serial_rx[m->serial_rx_head];
        m->serial_rx_head = (m->serial_rx_head + 1) % SERIAL_FIFO;
        m->serial_rx_count--;
        uint16_t low = asic_get(m, base);
        set_dma_low(m, SLOT_PRODUCT_SERIAL, false, (uint16_t)((low & ~(SERIAL_RX_RING - 1)) | ((low + 1) & (SERIAL_RX_RING - 1))));
    }
    serial->csr_a |= SERA_RX_CHARACTER_INTR;
    update_product_serial_interrupt(m);
}

static void serial_tick_event(machine_t *m) {
    if (m->cpu.cycles < m->serial_tick_at) return;
    m->serial_tick_at = m->cpu.cycles + SERIAL_TICK_CYCLES;
    product_serial_receive(m);
}

static void serial_write(machine_t *m, p2_serial_t *serial, int slot, uint32_t offset, uint16_t value) {
    if ((offset & 0xF) == 0) {
        serial->csr_a &= (uint16_t)~(value & SERA_W1C_MASK);
        serial->csr_a = (uint16_t)((serial->csr_a & ~SERA_RW_MASK) | (value & SERA_RW_MASK));
        if (slot == SLOT_PRODUCT_SERIAL) update_product_serial_interrupt(m);
        return;
    }
    uint16_t old = serial->csr_b;
    serial->csr_b = value;
    if (!(old & SERB_TX_EN) && (value & SERB_TX_EN)) {
        if (slot == SLOT_PRODUCT_SERIAL) {
            product_serial_transmit(m, serial);
            return;
        }
        uint8_t ch = read_dram_byte(m, dma_pointer(m, slot, true));
        if (slot == SLOT_DEBUG_SERIAL) debug_character(m, serial, ch);
        serial->csr_a |= SERA_TX_INTR;
    }
}

static bool keyboard_deliver(machine_t *m) {
    if (!m->key_count || (m->keyboard_isr & KB_RDRF)) return false;
    uint8_t byte = m->key_fifo[m->key_head];
    m->key_head = (m->key_head + 1) % KEY_FIFO;
    m->key_count--;
    m->keyboard_csr = (uint16_t)((m->keyboard_csr & ~KB_CSR_READ_ONLY) | byte);
    m->keyboard_isr |= KB_RDRF;
    set_board_source(m, INTR_KEYBOARD, true);
    return true;
}

static void keyboard_push(machine_t *m, uint8_t byte) {
    if (m->key_count == KEY_FIFO) {
        m->key_head = (m->key_head + 1) % KEY_FIFO;
        m->key_count--;
    }
    m->key_fifo[(m->key_head + m->key_count) % KEY_FIFO] = byte;
    m->key_count++;
}

static void update_touch_interrupt(machine_t *m) {
    uint16_t pending = (uint16_t)((m->adc_str & (ADC_PEN_INTR | ADC_TIMING_INTR | ADC_UCB_INTR) & m->touch_mask) |
                                  (m->ucb_str & UCB_REG_INTR & m->touch_mask));
    if ((m->touch_mask & MASK_SOUND) && (m->sound_str & SOUND_STR_MASK)) pending |= MASK_SOUND;
    set_board_source(m, INTR_TOUCH_AUDIO, pending != 0);
}

static uint16_t touch_read(machine_t *m, uint32_t offset) {
    switch (offset) {
        case TOUCH_ADC_CNTR: return m->adc_cntr;
        case TOUCH_ADC_STR: return m->adc_str;
        case TOUCH_UCB_CNTR: return m->ucb_cntr;
        case TOUCH_UCB_STR: return m->ucb_str;
        case TOUCH_UCB_REGISTER: return m->ucb_register;
        case TOUCH_SOUND_CNTR: return m->sound_cntr;
        case TOUCH_SOUND_STR: return m->sound_str;
        case TOUCH_INTR_MASK: return m->touch_mask;
        default: return 0;
    }
}

static void touch_write(machine_t *m, uint32_t offset, uint16_t value) {
    switch (offset) {
        case TOUCH_ADC_CNTR: {
            bool was_timing = (m->adc_cntr & ADC_TIMING_ENABLE) != 0;
            m->adc_cntr = value;
            if (value & ADC_DO_SAMPLE) {
                uint16_t sample = (value & ADC_SELECT_Y) ? m->adc_y : m->adc_x;
                m->adc_cntr = (uint16_t)((value & ~ADC_SAMPLE_MASK) | (sample & ADC_SAMPLE_MASK));
                m->adc_str |= ADC_PEN_INTR;
            }
            if ((value & ADC_TIMING_ENABLE) && !was_timing) m->pen_timer_at = m->cpu.cycles + PEN_TIMER_CYCLES;
            if (!(value & ADC_TIMING_ENABLE)) m->pen_timer_at = 0;
            break;
        }
        case TOUCH_ADC_STR: m->adc_str &= (uint16_t)~(value & (ADC_PEN_INTR | ADC_UCB_INTR | ADC_TIMING_INTR)); break;
        case TOUCH_UCB_CNTR: m->ucb_cntr = value; break;
        case TOUCH_UCB_STR: m->ucb_str &= (uint16_t)~(value & UCB_REG_INTR); break;
        case TOUCH_UCB_REGISTER: {
            uint32_t reg = m->ucb_cntr & 0xF;
            if (m->ucb_cntr & UCB_WRITE) {
                m->ucb_regs[reg] = value;
                m->ucb_register = value;
            } else {
                uint16_t read = m->ucb_regs[reg];
                if (reg == 0x0A) read |= 0x0C00u;
                if (reg == 0x09) read = m->pen_down ? (uint16_t)(read & ~UCB_PEN_STATE) : (uint16_t)(read | UCB_PEN_STATE);
                m->ucb_register = read;
            }
            m->ucb_str |= UCB_REG_INTR;
            break;
        }
        case TOUCH_SOUND_CNTR: m->sound_cntr = value; break;
        case TOUCH_SOUND_STR: m->sound_str &= (uint16_t)~(value & SOUND_STR_MASK); break;
        case TOUCH_INTR_MASK: m->touch_mask = value; break;
        default: break;
    }
    update_touch_interrupt(m);
}

static void pen_timer_event(machine_t *m) {
    if (!m->pen_timer_at || m->cpu.cycles < m->pen_timer_at) return;
    m->pen_timer_at += PEN_TIMER_CYCLES;
    m->adc_str |= ADC_TIMING_INTR;
    update_touch_interrupt(m);
}

static uint16_t pcmcia_interrupt_register(const machine_t *m, int socket) {
    if (socket) return asic_get(m, ASIC_PCMCIA_INTR1) | PCMCIA_NO_CARD;
    uint16_t value = m->pcmcia_state;
    if (!m->card.inserted) return value | PCMCIA_NO_CARD;
    bool line = cfcard_io_mode(&m->card) ? cfcard_interrupt(&m->card) : !cfcard_ready(&m->card);
    return line ? (uint16_t)(value | PCMCIA_CARD_INTR) : value;
}

static void update_pcmcia_interrupt(machine_t *m) {
    bool pending = false;
    for (int socket = 0; socket < 2; socket++) {
        uint16_t control = asic_get(m, socket ? ASIC_PCMCIA_CONTROL1 : ASIC_PCMCIA_CONTROL0);
        if (pcmcia_interrupt_register(m, socket) & (control >> 3) & (PCMCIA_CARD_INTR | PCMCIA_STATE_INTR)) pending = true;
    }
    set_board_source(m, INTR_SYSTEM, pending);
}

static uint32_t asic_read(machine_t *m, uint32_t offset, int size) {
    int slot;
    p2_serial_t *serial = serial_for(m, offset, &slot);
    if (serial) return (offset & 0xF) == 0 ? serial->csr_a : serial->csr_b;
    if (offset - ASIC_TOUCH_SOUND < 0x20) return touch_read(m, offset - ASIC_TOUCH_SOUND);
    switch (offset) {
        case ASIC_CPU_ISR: return m->cpu_isr;
        case ASIC_CPU_MR: return m->cpu_mr;
        case ASIC_CPU_STATUS: return 0;
        case ASIC_DISPLAY + 4: return m->display_csr;
        case ASIC_DISPLAY + 8: return m->display_xsize;
        case ASIC_DISPLAY + 12: return m->display_ysize;
        case ASIC_KEYBOARD: return m->keyboard_csr;
        case ASIC_KEYBOARD + 4: return m->keyboard_isr;
        case ASIC_PCMCIA_INTR0: return pcmcia_interrupt_register(m, 0);
        case ASIC_PCMCIA_INTR1: return pcmcia_interrupt_register(m, 1);
        default: break;
    }
    uint32_t value = asic_get(m, offset);
    if (size == 4) value |= (uint32_t)asic_get(m, offset + 2) << 16;
    else if (size == 1) value = (offset & 1) ? value >> 8 : value & 0xFF;
    return value;
}

static void asic_write(machine_t *m, uint32_t offset, int size, uint32_t value) {
    int slot;
    p2_serial_t *serial = serial_for(m, offset, &slot);
    if (serial) { serial_write(m, serial, slot, offset, (uint16_t)value); return; }
    if (offset - ASIC_TOUCH_SOUND < 0x20) { touch_write(m, offset - ASIC_TOUCH_SOUND, (uint16_t)value); return; }
    switch (offset) {
        case ASIC_CPU_MR: m->cpu_mr = value & 0xFFFFu; update_board_interrupt(m); return;
        case ASIC_CPU_ISR: return;
        case ASIC_PCMCIA_INTR0:
            m->pcmcia_state &= (uint16_t)~(value & PCMCIA_STATE_INTR);
            update_pcmcia_interrupt(m);
            return;
        case ASIC_PCMCIA_INTR1:
            return;
        case ASIC_PCMCIA_CONTROL0:
        case ASIC_PCMCIA_CONTROL1: {
            uint16_t old = asic_get(m, offset);
            m->asic[offset >> 1] = (uint16_t)value;
            if (offset == ASIC_PCMCIA_CONTROL0 && (old & PCMCIA_RESET) && !(value & PCMCIA_RESET)) cfcard_reset(&m->card_slot);
            update_pcmcia_interrupt(m);
            return;
        }
        case ASIC_DISPLAY + 4: m->display_csr = (uint16_t)value; return;
        case ASIC_DISPLAY + 8: m->display_xsize = (uint16_t)value; return;
        case ASIC_DISPLAY + 12: m->display_ysize = (uint16_t)value; return;
        case ASIC_KEYBOARD:
            m->keyboard_csr = (uint16_t)((m->keyboard_csr & KB_CSR_READ_ONLY) | (value & ~KB_CSR_READ_ONLY));
            return;
        case ASIC_KEYBOARD + 4:
            if ((value & KB_RDRF) && (m->keyboard_isr & KB_RDRF)) {
                m->keyboard_isr &= (uint16_t)~KB_RDRF;
                set_board_source(m, INTR_KEYBOARD, false);
                keyboard_deliver(m);
            }
            return;
        default: break;
    }
    uint32_t index = (offset & (ASIC_SIZE - 1)) >> 1;
    if (size == 1) {
        uint16_t old = m->asic[index];
        m->asic[index] = (offset & 1) ? (uint16_t)((old & 0x00FF) | (value & 0xFF) << 8) : (uint16_t)((old & 0xFF00) | (value & 0xFF));
        return;
    }
    m->asic[index] = (uint16_t)value;
    if (size == 4) m->asic[(index + 1) & (ASIC_SIZE / 2 - 1)] = (uint16_t)(value >> 16);
}

static uint32_t pcmcia_access(machine_t *m, uint32_t pa, int size, bool write, uint32_t value) {
    uint32_t offset = pa & PCMCIA_OFFSET_MASK;
    bool slot0 = ((pa >> PCMCIA_SLOT_SHIFT) & 1) == 0;
    uint32_t window = pa & 0xFF000000u;
    if (window == 0x1B000000u) {
        if (write) return 0;
        uint32_t status = PCMCIA_STATUS;
        return size == 1 ? (pa & 1 ? status >> 8 : status & 0xFF) : status;
    }
    if (!slot0) return write ? 0 : (size == 1 ? 0xFFu : 0xFFFFu);
    cfcard_slot_t *slot = &m->card_slot;
    uint32_t result = 0;
    switch (window) {
    case 0x14000000u:
    case 0x18000000u:
        if (write) cfcard_attribute_write(slot, offset, size, value);
        else result = cfcard_attribute_read(slot, offset, size);
        break;
    case 0x15000000u:
    case 0x19000000u:
        if (write) cfcard_common_write(slot, offset, size, value);
        else result = cfcard_common_read(slot, offset, size);
        break;
    case 0x1A000000u:
        if (write) cfcard_io_write(slot, offset, size, value);
        else result = cfcard_io_read(slot, offset, size);
        break;
    default:
        result = size == 1 ? 0xFFu : 0xFFFFu;
        break;
    }
    update_pcmcia_interrupt(m);
    return result;
}

static int pcmcia_bus_width(const machine_t *m, uint32_t pa) {
    uint32_t field = (m->chip.bsc[BCR2_INDEX] >> (pa >= PCMCIA_AREA6_PA ? 12 : 10)) & 3;
    return field == 1 ? 1 : field == 2 ? 2 : 4;
}

static uint32_t pcmcia_read(machine_t *m, uint32_t pa, int size) {
    int width = pcmcia_bus_width(m, pa);
    if (size <= width) return pcmcia_access(m, pa, size, false, 0);
    uint32_t value = 0;
    for (int part = 0; part < size; part += width) value |= pcmcia_access(m, pa + (uint32_t)part, width, false, 0) << (8 * part);
    return value;
}

static void pcmcia_write(machine_t *m, uint32_t pa, int size, uint32_t value) {
    int width = pcmcia_bus_width(m, pa);
    if (size <= width) {
        pcmcia_access(m, pa, size, true, value);
        return;
    }
    uint32_t mask = width == 1 ? 0xFFu : 0xFFFFu;
    for (int part = 0; part < size; part += width) pcmcia_access(m, pa + (uint32_t)part, width, true, (value >> (8 * part)) & mask);
}

static bool bus_read(void *context, uint32_t pa, int size, uint32_t *value) {
    machine_t *m = context;
    if (pa >= 0xE0000000u) {
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
    if (m->casio && pa - DRAM_PA >= DRAM_AREA_SIZE && pa >= FLASH_SIZE) {
        if (casio_read(&m->casio_board, &m->casio_host, pa, size, value)) return true;
        note_unknown(m, "read ", pa, size, 0);
        *value = 0;
        return true;
    }
    if (m->autopc && pa - DRAM_PA >= DRAM_AREA_SIZE) {
        if (autopc_read(&m->board, &m->board_host, pa, size, value)) return true;
        note_unknown(m, "read ", pa, size, 0);
        *value = 0;
        return true;
    }
    if (pa >= ASIC_PA && pa < ASIC_PA + ASIC_SIZE) { *value = asic_read(m, pa - ASIC_PA, size); return true; }
    if (pa >= HKEEP_PA && pa < HKEEP_END) {
        if ((pa & ~3u) == PARALLEL_PA) *value = ppfs_read_register(&m->ppfs);
        else *value = pa >= LED_ALPHA_PA ? m->led_alpha : pa >= LED_DISCRETE_PA ? m->led_discrete : 0;
        return true;
    }
    if (pa >= PCMCIA_PA && pa < PCMCIA_END) {
        *value = pcmcia_read(m, pa, size);
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
        if (m->casio && casio_write(&m->casio_board, &m->casio_host, pa, size, value)) return true;
        note_unknown(m, "write P4", pa, size, value);
        return true;
    }
    pa &= AREA_MASK;
    if (sh7709_write(&m->chip, pa, size, value)) return true;
    if (m->casio && pa - DRAM_PA >= DRAM_AREA_SIZE && pa >= FLASH_SIZE) {
        if (!casio_write(&m->casio_board, &m->casio_host, pa, size, value)) note_unknown(m, "write", pa, size, value);
        return true;
    }
    if (m->autopc && pa - DRAM_PA >= DRAM_AREA_SIZE) {
        if (!autopc_write(&m->board, &m->board_host, pa, size, value)) note_unknown(m, "write", pa, size, value);
        return true;
    }
    if (pa >= ASIC_PA && pa < ASIC_PA + ASIC_SIZE) { asic_write(m, pa - ASIC_PA, size, value); return true; }
    if (pa >= HKEEP_PA && pa < HKEEP_END) {
        if ((pa & ~3u) == PARALLEL_PA) ppfs_write_register(&m->ppfs, value);
        else if (pa >= LED_ALPHA_PA) m->led_alpha = value;
        else if (pa >= LED_DISCRETE_PA) m->led_discrete = value;
        return true;
    }
    if (pa >= PCMCIA_PA && pa < PCMCIA_END) {
        pcmcia_write(m, pa, size, value);
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

static void casio_transmit(void *context, int port, uint8_t byte) {
    machine_t *m = context;
    (void)port;
    debug_character(m, &m->sci_line, byte);
}

static bool find_rom_header(const uint8_t *image, size_t size, uint32_t *physfirst) {
    for (size_t offset = 0; offset + ROM_HEADER_SIZE <= size; offset += 4) {
        uint32_t first = read_le32(image + offset + 8), last = read_le32(image + offset + 12);
        if (last - first == size && !(first & 0xFFFu)) {
            *physfirst = first;
            return true;
        }
    }
    return false;
}

static bool load_flash(machine_t *m, char *error, size_t error_size) {
    uint32_t physfirst;
    if (m->image_size > FLASH_SIZE || !find_rom_header(m->image, m->image_size, &physfirst) || (physfirst & AREA_MASK) + m->image_size > FLASH_SIZE) {
        snprintf(error, error_size, "not a B000FF (nk.bin) image or a ROM image");
        return false;
    }
    m->flash = malloc(FLASH_SIZE);
    if (!m->flash) {
        snprintf(error, error_size, "out of memory");
        return false;
    }
    memset(m->flash, 0xFF, FLASH_SIZE);
    memcpy(m->flash + (physfirst & AREA_MASK), m->image, m->image_size);
    m->entry = RESET_VECTOR;
    return true;
}

static bool load_b000ff(machine_t *m, char *error, size_t error_size) {
    const uint8_t *file = m->image;
    size_t size = m->image_size;
    if (size < 15 || memcmp(file, "B000FF\n", 7)) {
        snprintf(error, error_size, "not a B000FF (nk.bin) image");
        return false;
    }
    size_t p = 15;
    while (p + 12 <= size) {
        uint32_t address = read_le32(file + p), length = read_le32(file + p + 4), checksum = read_le32(file + p + 8);
        if (address == 0 && checksum == 0) {
            m->entry = length;
            return true;
        }
        if (p + 12 + length > size) break;
        const uint8_t *data = file + p + 12;
        uint32_t sum = 0;
        for (uint32_t i = 0; i < length; i++) sum += data[i];
        if (sum != checksum) {
            snprintf(error, error_size, "B000FF record at %08X has a bad checksum", address);
            return false;
        }
        uint32_t pa = address & AREA_MASK;
        if (pa < DRAM_PA || pa + length > DRAM_PA + m->dram_size) {
            snprintf(error, error_size, "B000FF record at %08X is outside RAM", address);
            return false;
        }
        memcpy(m->dram + (pa - DRAM_PA), data, length);
        p += 12 + length;
    }
    snprintf(error, error_size, "B000FF image has no entry record");
    return false;
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

mailbox_t *machine_mailbox(machine_t *m) { return &m->mailbox; }

static void on_watch(void *context, uint32_t pc) {
    machine_t *m = context;
    machine_logf(m, "watch: pc %08X r4=%08X r5=%08X r6=%08X r7=%08X pr=%08X\n", pc, m->cpu.r[4], m->cpu.r[5], m->cpu.r[6], m->cpu.r[7], m->cpu.pr);
}

static void trace_exception(void *context, uint32_t code, uint32_t pc, bool user) {
    machine_t *m = context;
    bool tlb = code == SH3_EXP_TLB_MISS_READ || code == SH3_EXP_TLB_MISS_WRITE || code == SH3_EXP_INITIAL_WRITE;
    bool api_call = code == SH3_EXP_ADDRESS_READ && m->cpu.tea >= 0xFFFF0000u && (m->cpu.tea & 1);
    if (tlb || api_call) return;
    machine_logf(m, "exception %03X at %08X user=%d tea=%08X pr=%08X r15=%08X\n", code, pc, user, m->cpu.tea, m->cpu.pr, m->cpu.r[15]);
}

static bool never_stop(void *context, uint32_t pc) { (void)context; (void)pc; return false; }

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

static bool reset_machine(machine_t *m, bool keep_ram, char *error, size_t error_size) {
    if (m->dram_size != m->dram_size_next || !m->dram) {
        free(m->dram);
        m->dram_size = m->dram_size_next;
        m->dram = calloc(1, m->dram_size);
        keep_ram = false;
    }
    if (!keep_ram) {
        memset(m->dram, 0, m->dram_size);
        if (!m->flash && !load_b000ff(m, error, error_size)) return false;
        if (m->autopc) autopc_prepare_ram(m->dram, m->dram_size);
    }
    m->cpu.bus = (sh3_bus_t){ m, bus_read, bus_write, bus_fetch_page, m->dram, DRAM_PA, m->dram_size };
    m->cpu.on_watch = on_watch;
    m->cpu.on_trapa = on_trapa;
    sh3_reset(&m->cpu);
    mailbox_clear(&m->mailbox);
    m->mailbox_pc = 0;
    m->mailbox_page_count = 0;
    m->cpu.pc = m->entry;
    m->cpu.watch_count = m->watch_count;
    memcpy(m->cpu.watch, m->watch, sizeof m->watch);
    sh7709_init(&m->chip, &m->cpu, SH7708, MACHINE_CLOCK_HZ, MACHINE_PERIPHERAL_HZ);
    sh7709_set_time(&m->chip, 2000 - 1970, 1, 1, 6, 0, 0, 0);
    if (m->host_clock) apply_host_time(m);
    autopc_reset(&m->board);
    casio_reset(&m->casio_board);
    if (m->casio) {
        m->chip.transmit = casio_transmit;
        m->chip.transmit_context = m;
    }
    memset(m->asic, 0, sizeof m->asic);
    m->pcmcia_state = 0;
    cfcard_reset(&m->card_slot);
    ppfs_close_all(&m->ppfs);
    ppfs_init(&m->ppfs);
    m->cpu_isr = m->cpu_mr = 0;
    m->display_csr = 0;
    m->display_xsize = SCREEN_STOCK_WIDTH - 1;
    m->display_ysize = SCREEN_STOCK_HEIGHT - 1;
    memset(m->serial, 0, sizeof m->serial);
    m->serial[1].csr_a = SERA_LINES;
    set_serial_lines(m);
    m->serial[1].csr_a &= (uint16_t)~SERA_RX_CHANGED_INTR;
    m->serial_rx_count = m->serial_tx_count = 0;
    m->keyboard_csr = m->keyboard_isr = 0;
    m->key_head = m->key_count = 0;
    m->adc_cntr = m->adc_str = m->ucb_cntr = m->ucb_str = m->ucb_register = m->sound_cntr = m->sound_str = m->touch_mask = 0;
    memset(m->ucb_regs, 0, sizeof m->ucb_regs);
    m->pen_down = false;
    m->pen_timer_at = 0;
    return true;
}

machine_t *machine_create(const uint8_t *rom, size_t rom_size, char *error, size_t error_size) {
    machine_t *m = calloc(1, sizeof *m);
    if (!m) return NULL;
    m->image = malloc(rom_size);
    memcpy(m->image, rom, rom_size);
    m->image_size = rom_size;
    m->rom_hash = hash_bytes(rom, rom_size);
    m->autopc = autopc_detect(rom, rom_size);
    m->casio = rom_size < 7 || memcmp(rom, "B000FF\n", 7);
    m->board_host = (autopc_host_t){ autopc_debug_line, autopc_trace, autopc_irl, m, &m->card_slot };
    m->casio_host = (casio_host_t){ casio_trace, casio_cycles, MACHINE_CLOCK_HZ, m };
    m->card_slot.state = &m->card;
    if (m->casio && !load_flash(m, error, error_size)) {
        machine_destroy(m);
        return NULL;
    }
    m->dram_size_next = DRAM_DEFAULT_SIZE;
    if (!reset_machine(m, false, error, error_size)) {
        machine_destroy(m);
        return NULL;
    }
    return m;
}

void machine_destroy(machine_t *m) {
    if (!m) return;
    mailbox_clear(&m->mailbox);
    cfcard_eject(&m->card_slot);
    ppfs_close_all(&m->ppfs);
    free(m->dram);
    free(m->flash);
    free(m->image);
    free(m);
}

void machine_set_log(machine_t *m, machine_log_fn log) { m->log = log; }

static void pending_card_event(machine_t *m);

void machine_run(machine_t *m, uint64_t cycles) {
    uint64_t target = m->cpu.cycles + cycles;
    while (m->cpu.cycles < target) {
        sh7709_advance(&m->chip);
        pen_timer_event(m);
        pending_card_event(m);
        serial_tick_event(m);
        uint64_t next = sh7709_next_event(&m->chip);
        if (m->serial_rx_count && m->serial_tick_at < next) next = m->serial_tick_at;
        if (m->pending_card_at && m->pending_card_at < next) next = m->pending_card_at;
        if (m->pen_timer_at && m->pen_timer_at < next) next = m->pen_timer_at;
        uint64_t until = next < target ? next : target;
        if (until <= m->cpu.cycles) until = m->cpu.cycles + 1;
        sh3_run(&m->cpu, until);
        if (m->cpu.debug && m->cpu.debug->stop) break;
    }
    sh7709_advance(&m->chip);
}

sh3_cpu_t *machine_cpu(machine_t *m) { return &m->cpu; }

bool machine_read_physical(machine_t *m, uint32_t pa, uint8_t *data, uint32_t length) {
    pa &= AREA_MASK;
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

uint64_t machine_cycles(machine_t *m) { return m->cpu.cycles; }
uint32_t machine_pc(machine_t *m) { return m->cpu.pc; }

bool machine_lcd_enabled(machine_t *m) { return m->autopc || m->casio || (m->display_csr & DISP_LCD_ON) != 0; }
bool machine_backlight(machine_t *m) { return machine_lcd_enabled(m); }

screen_size_t machine_screen_size(machine_t *m) {
    if (m->autopc) return (screen_size_t){ AUTOPC_SCREEN_WIDTH, AUTOPC_SCREEN_HEIGHT };
    if (m->casio) return (screen_size_t){ CASIO_SCREEN_WIDTH, CASIO_SCREEN_HEIGHT };
    return (screen_size_t){ SCREEN_STOCK_WIDTH, SCREEN_STOCK_HEIGHT };
}

screen_size_t machine_screen_next(machine_t *m) { return machine_screen_size(m); }

bool machine_screen_supported(machine_t *m, screen_size_t size) {
    screen_size_t stock = machine_screen_size(m);
    return size.width == stock.width && size.height == stock.height;
}

bool machine_set_screen(machine_t *m, screen_size_t size) { return machine_screen_supported(m, size); }

int machine_screen_palette(machine_t *m, uint32_t *palette) {
    return m->autopc ? autopc_palette(palette) : 0;
}

bool machine_screen(machine_t *m, uint8_t *levels) {
    if (m->autopc) {
        autopc_screen(&m->board, levels);
        return true;
    }
    if (m->casio) {
        casio_screen(&m->casio_board, levels);
        return true;
    }
    screen_size_t size = machine_screen_size(m);
    uint32_t width = size.width, height = size.height;
    if (!machine_lcd_enabled(m)) {
        memset(levels, 0, width * height);
        return false;
    }
    uint32_t base = dma_pointer(m, SLOT_DISPLAY, true);
    uint32_t stride = ((uint32_t)m->display_xsize + 1) / 4;
    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            uint8_t byte = read_dram_byte(m, base + y * stride + x / 4);
            uint32_t pixel = (byte >> ((3 - (x & 3)) * 2)) & 3;
            levels[y * width + x] = (uint8_t)((3 - pixel) * 5);
        }
    }
    return true;
}

void machine_key(machine_t *m, uint8_t scancode, bool up) {
    if (m->autopc) {
        autopc_key(&m->board, &m->board_host, scancode, up);
        return;
    }
    if (!(m->keyboard_csr & KB_CLK_EN)) return;
    bool extended = scancode >= 0x80 && scancode != 0x83;
    if (extended) keyboard_push(m, 0xE0);
    if (up) keyboard_push(m, 0xF0);
    keyboard_push(m, extended ? scancode & 0x7F : scancode);
    keyboard_deliver(m);
}

static uint16_t touch_raw(int pixel) {
    int raw = (pixel < 0 ? 0 : pixel) * TOUCH_SCALE;
    return raw > (int)ADC_SAMPLE_MASK ? (uint16_t)ADC_SAMPLE_MASK : (uint16_t)raw;
}

void machine_touch(machine_t *m, bool down, int x, int y) {
    m->adc_x = touch_raw(x);
    m->adc_y = touch_raw(y);
    if (down && !m->pen_down) m->adc_str |= ADC_UCB_INTR;
    m->pen_down = down;
    update_touch_interrupt(m);
}
bool machine_suspended(machine_t *m) { (void)m; return false; }

static bool insert_card_now(machine_t *m, const char *path) {
    FILE *image = fopen(path, "r+b");
    if (!image) return false;
    cfcard_insert(&m->card_slot, image);
    snprintf(m->card_path, sizeof m->card_path, "%s", path);
    m->pcmcia_state |= PCMCIA_STATE_INTR;
    update_pcmcia_interrupt(m);
    if (m->autopc) autopc_card_changed(&m->board, &m->board_host);
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
    m->pcmcia_state |= PCMCIA_STATE_INTR;
    update_pcmcia_interrupt(m);
    if (m->autopc) autopc_card_changed(&m->board, &m->board_host);
}

bool machine_card_inserted(machine_t *m) { return m->card.inserted; }

void machine_serial_connect(machine_t *m, bool connected) {
    m->serial_connected = connected;
    if (!connected) m->serial_rx_count = m->serial_tx_count = 0;
    set_serial_lines(m);
}

bool machine_serial_connected(machine_t *m) { return m->serial_connected; }

bool machine_serial_dtr(machine_t *m) { return (m->serial[1].csr_b & SERB_DTR) != 0; }

size_t machine_serial_space(machine_t *m) { return SERIAL_FIFO - m->serial_rx_count; }

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

static void ppfs_log(void *context, const char *message) {
    machine_logf(context, "%s", message);
}

bool machine_set_host_folder(machine_t *m, const char *path) {
    char absolute[PATH_MAX];
    struct stat info;
    bool usable = !path || !path[0] || (realpath(path, absolute) && !stat(absolute, &info) && S_ISDIR(info.st_mode));
    if (!usable) return false;
    ppfs_set_root(&m->ppfs, path && path[0] ? absolute : NULL);
    m->ppfs.log = ppfs_log;
    m->ppfs.log_context = m;
    return true;
}

void machine_reset(machine_t *m) {
    char error[256];
    if (!reset_machine(m, false, error, sizeof error)) machine_logf(m, "reset: %s\n", error);
}

void machine_soft_reset(machine_t *m) {
    char error[256];
    reset_machine(m, true, error, sizeof error);
}

bool machine_watch_pc(machine_t *m, uint32_t va) {
    if (m->watch_count >= MACHINE_WATCH_MAX) return false;
    m->watch[m->watch_count++] = va;
    m->cpu.watch_count = m->watch_count;
    memcpy(m->cpu.watch, m->watch, sizeof m->watch);
    return true;
}

void machine_set_memory(machine_t *m, uint32_t megabytes) {
    if (megabytes != 16 && megabytes != 32 && megabytes != 64) return;
    m->dram_size_next = megabytes << 20;
    if (m->dram_size_next != m->dram_size) machine_reset(m);
}

uint32_t machine_memory(machine_t *m) { return m->dram_size >> 20; }
uint32_t machine_memory_next(machine_t *m) { return m->dram_size_next >> 20; }
void machine_set_speed(machine_t *m, uint32_t multiplier) { m->cpu.speed = multiplier ? multiplier : 1; }
uint32_t machine_speed(machine_t *m) { return m->cpu.speed ? m->cpu.speed : 1; }
uint64_t machine_rom_hash(machine_t *m) { return m->rom_hash; }
int machine_rom_system(machine_t *m) { (void)m; return 2; }
key_layout_t machine_key_layout(machine_t *m) { (void)m; return KEY_LAYOUT_ROM; }

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
    machine_logf(m, "cycles=%llu sleeping=%d irl=%u isr=%04X mr=%04X led=%08X alpha=%08X lcd=%04X\n",
                 (unsigned long long)cpu->cycles, cpu->sleeping, m->chip.irl_level, m->cpu_isr, m->cpu_mr, m->led_discrete, m->led_alpha, m->display_csr);
}

#define STATE_FIELDS(X) \
    X(cpu, m->cpu) X(chip, m->chip) X(asic, m->asic) X(cpu_isr, m->cpu_isr) X(cpu_mr, m->cpu_mr) \
    X(display_csr, m->display_csr) X(display_xsize, m->display_xsize) X(display_ysize, m->display_ysize) \
    X(serial, m->serial) X(keyboard_csr, m->keyboard_csr) X(keyboard_isr, m->keyboard_isr) \
    X(key_fifo, m->key_fifo) X(key_head, m->key_head) X(key_count, m->key_count) \
    X(led_discrete, m->led_discrete) X(led_alpha, m->led_alpha) \
    X(adc_cntr, m->adc_cntr) X(adc_str, m->adc_str) X(ucb_cntr, m->ucb_cntr) X(ucb_str, m->ucb_str) \
    X(ucb_register, m->ucb_register) X(sound_cntr, m->sound_cntr) X(sound_str, m->sound_str) \
    X(touch_mask, m->touch_mask) X(ucb_regs, m->ucb_regs) X(pen_timer_at, m->pen_timer_at) \
    X(card, m->card) X(card_path, m->card_path) X(pcmcia_state, m->pcmcia_state) X(board, m->board) X(casio_board, m->casio_board)

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

bool machine_state_matches(machine_t *m, const char *path) {
    size_t length;
    uint8_t *data = read_state(path, &length);
    bool matches = data && state_header_matches(m, data, length);
    free(data);
    return matches;
}

bool machine_load(machine_t *m, const char *path, int64_t *host_time) {
    size_t length;
    uint8_t *data = read_state(path, &length);
    if (!data) return false;
    if (!state_header_matches(m, data, length)) {
        free(data);
        return false;
    }
    if (host_time) memcpy(host_time, data + sizeof STATE_MAGIC + 8, 8);
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
    m->cpu.on_interrupt = NULL;
    m->cpu.watch_count = m->watch_count;
    memcpy(m->cpu.watch, m->watch, sizeof m->watch);
    m->chip.cpu = &m->cpu;
    ppfs_close_all(&m->ppfs);
    ppfs_init(&m->ppfs);
    cfcard_sanitize(&m->card);
    FILE *image = m->card.inserted && m->card_path[0] ? fopen(m->card_path, "r+b") : NULL;
    cfcard_rebind(&m->card_slot, image);
    if (!image) m->card_path[0] = 0;
    m->chip.transmit = m->casio ? casio_transmit : NULL;
    m->chip.transmit_context = m->casio ? m : NULL;
    sh3_flush_translations(&m->cpu);
    return true;
}
