#include "machine.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mips.h"
#include "pccard.h"
#include "uart.h"

#define DRAM_SIZE        0x00400000u
#define DRAM_MAX         0x01000000u
#define DRAM_DECODE_END  0x02000000u
#define BANK1_DECODE_END 0x04000000u
#define ROM_PA           0x1F400000u
#define ROM_WINDOW_PA    0x1F000000u
#define ROM_WINDOW_END   0x20000000u
#define REGS_PA          0x10C00000u
#define REGS_END         0x10E00000u
#define CS2_PA           0x10400000u
#define CS2_END          0x10800000u
#define DEBUG_PROBE_PA   0x10401024u
#define ENTRY_VA         0x9F400000u
#define ROM_CARD_PA      0x10000000u
#define ROM_CARD_END     0x10400000u
#define KSEG_PA_MASK     0x1FFFFFFFu
#define BOOT_BLOCK_PA    0x1FC00000u
#define BOOT_BLOCK_SIZE  0x1000u

static const char STATE_MAGIC[16] = "VELO1 STATE v2";

#define REG_COUNT 128

#define INTC_SETS 5

#define IODIN_MMODULE_ATTACHED (1u << 0)
#define IODIN_SERIAL_DCD       (1u << 4)
#define MFIO_SERIAL_CTS        (1u << 30)
#define STATUS5_IOPOS_DCD      (1u << 11)
#define STATUS5_IONEG_DCD      (1u << 4)
#define IODIN_MINICARD1_ABSENT (1u << 5)
#define MFIO_EEPROM_SCL        (1u << 18)
#define MFIO_EEPROM_SDA        (1u << 20)
#define EEPROM_ADDRESS         0x50
#define CARD_DRAM_MAX          0x01000000u
#define IODIN_MINICARD2_ABSENT (1u << 6)

#define POWER_ONBUTN   (1u << 31)
#define POWER_PWRINT   (1u << 30)
#define POWER_PWROK    (1u << 29)
#define POWER_VIDRF_SHIFT 27
#define POWER_STPTIMERVAL_SHIFT 12
#define POWER_ENSTPTIMER (1u << 11)
#define POWER_FORCESHUTDWN (1u << 9)
#define POWER_STOPCPU  (1u << 4)
#define POWER_COLDSTART (1u << 2)
#define POWER_PWRCS    (1u << 1)
#define POWER_VCCON    (1u << 0)
#define POWER_WRITABLE 0x1E00FFBFu

#define STATUS5_SET        4
#define STATUS5_STPTIMER   (1u << 28)
#define STATUS5_PERINT     (1u << 29)
#define STATUS5_ALARMINT   (1u << 30)
#define STATUS5_SPIBUFAVAIL (1u << 21)
#define STATUS5_POSCARINT  (1u << 15)
#define STATUS5_NEGCARINT  (1u << 14)
#define IR_CARDET          (1u << 24)
#define STATUS5_POSONBUTN  (1u << 23)
#define STATUS5_NEGONBUTN  (1u << 22)
#define STATUS5_SPIRCV     (1u << 19)
#define STATUS1_LCDINT     (1u << 31)
#define STATUS1_SIBSF0     (1u << 8)
#define STATUS1_SIBSF1     (1u << 7)
#define STATUS1_SIBIRQPOS  (1u << 6)

#define ENABLE6_GLOBALEN   (1u << 18)

#define TIMER_RTCCLR       (1u << 3)
#define TIMER_ENPERTIMER   (1u << 4)

#define LCD_ENVID  (1u << 0)
#define LCD_DISPON (1u << 1)
#define LCD_INVVID (1u << 2)
#define LCD_DISP8  (1u << 4)

#define MFIO_LCD_POWER_OFF (1u << 17)
#define MFIO_BACKLIGHT     (1u << 25)
#define SCANCODE_BACKLIGHT 0x5E

#define SPI_ENSPI  (1u << 0)
#define SPI_EMPTY  (1u << 16)
#define SPI_SPION  (1u << 17)

#define SIB_ENSIB  (1u << 0)
#define SIB_ENSF0  (1u << 1)
#define SIB_IRQ    (1u << 31)
#define SIB_ENSND  (1u << 4)
#define SIB_DMA_ENTXSND (1u << 16)
#define SIB_SCLK_HZ 9216000u
#define STATUS1_SND0_5 (1u << 22)
#define STATUS1_SND1_0 (1u << 21)
#define SOUND_MAX_BYTES 0x4000u
#define AUDIO_RING 65536

#define UCB_IO_DATA   0x00
#define UCB_IE_FAL    0x03
#define UCB_IE_STATUS 0x04
#define UCB_TS_CR     0x09
#define UCB_ADC_CR    0x0A
#define UCB_ADC_DATA  0x0B
#define UCB_ID        0x0C
#define UCB_NULL      0x0F
#define UCB_ID_1100   0x1003u
#define UCB_PEN_BITS  ((1u << 12) | (1u << 13))

#define IRQ_LOW_IP  (1u << 12)
#define IRQ_HIGH_IP (1u << 14)

#define STOP_TIMER_TICK_CYCLES (MACHINE_CLOCK_HZ / 125u)
#define NO_EVENT UINT64_MAX
#define CARD_SWAP_CYCLES MACHINE_CLOCK_HZ

#define KEY_QUEUE_SIZE 64
#define LOGGED_ADDRESSES 512

typedef struct { uint32_t set; uint32_t mask; } high_priority_term_t;

static const high_priority_term_t high_priority[16][2] = {
    { { 0, 0 }, { 0, 0 } },
    { { 4, (1u << 7) | (1u << 0) }, { 0, 0 } },
    { { 0, 1u << 27 }, { 0, 0 } },
    { { 0, 1u << 17 }, { 0, 0 } },
    { { 0, 1u << 18 }, { 0, 0 } },
    { { 1, 1u << 5 }, { 0, 0 } },
    { { 3, (1u << 1) | (1u << 0) }, { 4, (1u << 6) | (1u << 5) } },
    { { 3, 0xFu << 16 }, { 0, 0 } },
    { { 2, (1u << 1) | (1u << 0) }, { 4, (1u << 13) | (1u << 12) } },
    { { 2, 0xFu << 16 }, { 0, 0 } },
    { { 1, 1u << 21 }, { 0, 0 } },
    { { 1, 1u << 31 }, { 0, 0 } },
    { { 1, (1u << 3) | (1u << 2) }, { 0, 0 } },
    { { 4, 1u << 29 }, { 0, 0 } },
    { { 4, 1u << 30 }, { 0, 0 } },
    { { 4, (1u << 25) | (1u << 24) }, { 0, 0 } },
};

struct machine {
    mips_cpu_t cpu;
    uint8_t *dram;
    uint32_t dram_size;
    uint32_t dram_size_next;
    uint8_t *card_dram;
    uint32_t card_dram_size;
    uint32_t card_dram_size_next;
    uint8_t  eeprom_phase, eeprom_shift, eeprom_bit, eeprom_addr;
    bool     eeprom_selected, eeprom_read, eeprom_in_ack, eeprom_scl, eeprom_sda, eeprom_sda_out;
    uint8_t *rom;
    uint32_t rom_size;
    uint32_t rom_pa;
    uint8_t *rom2;
    uint32_t rom2_size;
    uint32_t rom2_pa;
    uint32_t entry_va;
    uint64_t rom_hash;
    machine_log_fn log;
    bool halted;
    char halt_reason[256];

    uint32_t regs[REG_COUNT];
    pccard_t pccard;
    pccard_socket_t card_socket;
    char     card_path[1024];
    FILE    *pending_card;
    char     pending_card_path[1024];
    uint64_t card_insert_at;
    bool     ir_cardet;
    uart_t   uart_a;
    uart_port_t uart_port;
    bool     serial_connected;
    uint32_t serial_tag;

    uint32_t intc_status[INTC_SETS];
    uint32_t intc_enable[INTC_SETS];
    uint32_t intc_free_running[INTC_SETS];
    uint32_t intc_enable6;

    uint32_t timer_ctl;
    uint32_t perval;
    uint64_t periodic_next;
    uint64_t rtc_base;
    uint64_t rtc_anchor;
    uint64_t alarm;
    bool     alarm_armed;
    uint64_t alarm_next;

    uint32_t power_ctl;
    bool     cpu_stopped;
    bool     suspended;
    bool     power_button;
    uint64_t suspended_at;
    uint64_t suspended_cycles;
    uint64_t stopped_cycles;
    uint64_t stop_timer_next;

    uint64_t lcd_next;

    uint32_t io_ctl;
    uint32_t mfio_dout, mfio_direc, mfio_sel;

    uint32_t spi_ctl;
    uint8_t  key_queue[KEY_QUEUE_SIZE];
    int      key_head, key_count;
    bool     keyboard_enabled;

    uint32_t sib_ctl;
    uint32_t sib_sf0_aux;
    uint32_t sib_sf0_stat;
    uint32_t sib_dma_ctl;
    uint32_t snd_size;
    uint32_t snd_tx_start;
    bool     sound_active;
    uint32_t sound_half;
    uint64_t sound_next;
    int16_t  audio[AUDIO_RING];
    uint32_t audio_head, audio_count;
    uint32_t audio_rate;
    uint16_t ucb_regs[16];
    uint16_t ucb_adc_data;
    uint16_t pen_irq_armed;
    uint16_t pen_irq_status;
    bool     pen_down;
    int      pen_x, pen_y;
    bool     touch_legacy;

    uint32_t logged[LOGGED_ADDRESSES];
    int      logged_count;
};

static void machine_logf(machine_t *m, const char *format, ...) {
    char message[512];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof message, format, args);
    va_end(args);
    if (m->log) m->log(message);
}

static void note_access(machine_t *m, const char *what, uint32_t pa, int size, uint32_t value) {
    for (int i = 0; i < m->logged_count; i++) if (m->logged[i] == pa) return;
    if (m->logged_count < LOGGED_ADDRESSES) m->logged[m->logged_count++] = pa;
    machine_logf(m, "%s pa=%08X size=%d value=%08X pc=%08X\n", what, pa, size, value, m->cpu.pc);
}

static uint32_t high_priority_level(const machine_t *m) {
    for (uint32_t level = 15; level >= 1; level--) {
        if (!(m->intc_enable6 & (1u << level))) continue;
        for (int term = 0; term < 2; term++) {
            const high_priority_term_t *t = &high_priority[level][term];
            if (t->mask && (m->intc_status[t->set] & t->mask)) return level;
        }
    }
    return 0;
}

static bool irq_low(const machine_t *m) {
    if (!(m->intc_enable6 & ENABLE6_GLOBALEN)) return false;
    for (int i = 0; i < INTC_SETS; i++) if (m->intc_status[i] & m->intc_enable[i]) return true;
    return false;
}

static bool irq_high(const machine_t *m) {
    return (m->intc_enable6 & ENABLE6_GLOBALEN) && high_priority_level(m) != 0;
}

static void intc_update(machine_t *m) {
    uint32_t ip = 0;
    if (irq_low(m)) ip |= IRQ_LOW_IP;
    if (irq_high(m)) ip |= IRQ_HIGH_IP;
    mips_set_external_ip(&m->cpu, ip);
}

static void intc_set_pending(machine_t *m, int set, uint32_t bits) {
    m->intc_status[set] |= bits;
    intc_update(m);
}

static void intc_free_running(machine_t *m, int set, uint32_t bits, bool active) {
    if (active) {
        m->intc_free_running[set] |= bits;
        m->intc_status[set] |= bits;
    } else {
        m->intc_free_running[set] &= ~bits;
    }
    intc_update(m);
}

static uint8_t keyboard_checksum(uint8_t byte) {
    uint8_t value = 0x80 ^ 0xA8 ^ 0x00 ^ byte;
    return (value & 0x80) ? (uint8_t)(value ^ 0xC0) : value;
}

static void key_push(machine_t *m, uint8_t byte) {
    if (m->key_count == KEY_QUEUE_SIZE) return;
    m->key_queue[(m->key_head + m->key_count) % KEY_QUEUE_SIZE] = byte;
    m->key_count++;
}

static void keyboard_stage_enable(machine_t *m) {
    if (m->keyboard_enabled) return;
    key_push(m, 0x80);
    key_push(m, 0xA8);
    key_push(m, 0x00);
    key_push(m, 0x00);
    key_push(m, keyboard_checksum(0x00));
    m->keyboard_enabled = true;
}

static void intc_enable_written(machine_t *m, int set, uint32_t value) {
    if (set == STATUS5_SET && (value & STATUS5_SPIRCV)) {
        keyboard_stage_enable(m);
        if (m->key_count) intc_set_pending(m, STATUS5_SET, STATUS5_SPIRCV);
    }
}

static uint64_t periodic_period(const machine_t *m) {
    return ((uint64_t)m->perval + 1) * 32;
}

static uint64_t rtc_count(const machine_t *m) {
    if (m->timer_ctl & TIMER_RTCCLR) return 0;
    uint64_t elapsed = m->cpu.cycles - m->rtc_anchor;
    return (m->rtc_base + elapsed * 32768u / MACHINE_CLOCK_HZ) & 0xFFFFFFFFFFull;
}

static void alarm_schedule(machine_t *m) {
    m->alarm_next = NO_EVENT;
    if (!m->alarm_armed || (m->timer_ctl & TIMER_RTCCLR)) return;
    uint64_t now = rtc_count(m);
    if (m->alarm <= now) { m->alarm_next = m->cpu.cycles; return; }
    m->alarm_next = m->cpu.cycles + (m->alarm - now) * MACHINE_CLOCK_HZ / 32768u + 1;
}

static uint64_t lcd_frame_cycles(const machine_t *m) {
    uint32_t ctl1 = m->regs[0x28 / 4], ctl2 = m->regs[0x2C / 4];
    uint64_t baudval = (ctl1 >> 16) & 0x1Fu;
    uint64_t vidrate = (ctl2 >> 22) & 0x3FFu;
    uint64_t lineval = ctl2 & 0x3FFu;
    uint32_t vidrf = (m->power_ctl >> POWER_VIDRF_SHIFT) & 3u;
    return ((baudval * 2 + 2) * (vidrate + 1) * (lineval + 1)) << vidrf;
}

static void lcd_schedule(machine_t *m) {
    if (!(m->regs[0x28 / 4] & LCD_ENVID) || lcd_frame_cycles(m) == 0) { m->lcd_next = NO_EVENT; return; }
    if (m->lcd_next == NO_EVENT) m->lcd_next = m->cpu.cycles + lcd_frame_cycles(m);
}

static uint16_t touch_adc(int pixel) {
    int value = 64 + (pixel < 0 ? 0 : pixel) * 2;
    return (uint16_t)(value > 1023 ? 1023 : value);
}

static uint16_t touch_x_adc(int x) {
    int value = 40 + (x < 0 ? 0 : x) * 1903 / 1000;
    return (uint16_t)(value > 1023 ? 1023 : value);
}

static uint16_t touch_y_adc(int y) {
    int value = 92 + (y < 0 ? 0 : y) * 3528 / 1000;
    return (uint16_t)(value > 1023 ? 1023 : value);
}

static uint16_t ucb_read(machine_t *m, uint8_t reg) {
    switch (reg & 0xF) {
        case UCB_IE_STATUS: return m->pen_irq_status;
        case UCB_TS_CR: {
            uint16_t value = m->ucb_regs[UCB_TS_CR];
            if (m->pen_down) value |= UCB_PEN_BITS;
            else value &= (uint16_t)~UCB_PEN_BITS;
            return value;
        }
        case UCB_ADC_DATA: return m->ucb_adc_data;
        case UCB_ID: return UCB_ID_1100;
        case UCB_NULL: return 0xFFFF;
        case UCB_IO_DATA: return m->ucb_regs[UCB_IO_DATA];
        default: return m->ucb_regs[reg & 0xF];
    }
}

static void ucb_convert(machine_t *m, uint16_t adc_cr) {
    uint16_t mode = m->ucb_regs[UCB_TS_CR] & (3u << 8);
    uint16_t channel = adc_cr & (7u << 2);
    uint16_t sample;
    if (channel >= (4u << 2)) {
        int aux = (channel >> 2) - 4;
        sample = aux == 2 ? 0x1F0 : aux == 3 ? 0x200 : 0;
    } else if (mode == (1u << 8)) {
        sample = m->pen_down ? 0x3FF : 0;
    } else if (channel == (2u << 2) || channel == (3u << 2)) {
        sample = m->touch_legacy ? touch_adc(m->pen_x) : touch_y_adc(m->pen_y);
    } else {
        sample = m->touch_legacy ? touch_adc(m->pen_y) : touch_x_adc(m->pen_x);
    }
    m->ucb_adc_data = (uint16_t)((1u << 15) | ((sample & 0x3FFu) << 5));
}

static void ucb_write(machine_t *m, uint8_t reg, uint16_t value) {
    m->ucb_regs[reg & 0xF] = value;
    switch (reg & 0xF) {
        case UCB_ADC_CR: if (value & (1u << 7)) ucb_convert(m, value); break;
        case UCB_IE_FAL: m->pen_irq_armed = value & UCB_PEN_BITS; break;
        case UCB_IE_STATUS: m->pen_irq_status &= (uint16_t)~value; break;
        default: break;
    }
}

static uint32_t sound_rate(const machine_t *m) {
    uint32_t fsdiv = (m->sib_ctl >> 8) & 0x7F;
    return SIB_SCLK_HZ * 2 / ((fsdiv + 1) * 64);
}

static uint32_t sound_bytes(const machine_t *m) {
    uint32_t bytes = (m->snd_size + 1) << 2;
    return bytes > SOUND_MAX_BYTES ? SOUND_MAX_BYTES : bytes;
}

static void sound_capture_half(machine_t *m) {
    uint32_t bytes = sound_bytes(m) & ~1u;
    uint32_t half = (bytes / 2) & ~1u;
    uint32_t offset = m->sound_half ? half : 0;
    uint32_t length = m->sound_half ? ((bytes - half) & ~1u) : half;
    for (uint32_t i = 0; i + 1 < length; i += 2) {
        uint32_t pa = m->snd_tx_start + offset + i;
        int16_t sample = (int16_t)(m->dram[pa & (m->dram_size - 1)] << 8 | m->dram[(pa + 1) & (m->dram_size - 1)]);
        if (m->audio_count == AUDIO_RING) {
            m->audio_head = (m->audio_head + 1) % AUDIO_RING;
            m->audio_count--;
        }
        m->audio[(m->audio_head + m->audio_count) % AUDIO_RING] = sample;
        m->audio_count++;
    }
    uint64_t samples = length / 2;
    m->sound_next = m->cpu.cycles + (samples * MACHINE_CLOCK_HZ + sound_rate(m) - 1) / sound_rate(m);
}

static void sound_update(machine_t *m) {
    bool armed = (m->sib_ctl & SIB_ENSIB) && (m->sib_ctl & SIB_ENSND) && (m->sib_dma_ctl & SIB_DMA_ENTXSND)
              && sound_bytes(m) >= 4;
    if (armed && !m->sound_active) {
        m->sound_active = true;
        m->sound_half = 0;
        m->audio_rate = sound_rate(m);
        sound_capture_half(m);
    } else if (!armed && m->sound_active) {
        m->sound_active = false;
        m->sound_next = NO_EVENT;
    }
}

static void sound_event(machine_t *m) {
    intc_set_pending(m, 0, m->sound_half ? STATUS1_SND1_0 : STATUS1_SND0_5);
    m->sound_half ^= 1;
    sound_capture_half(m);
}

enum { EEPROM_IDLE, EEPROM_CONTROL, EEPROM_WORD, EEPROM_WRITE, EEPROM_TRANSMIT };

static uint8_t eeprom_byte(const machine_t *m, uint8_t address) {
    switch (address) {
        case 16: return 0x99;
        case 59: return 1;
        case 64: return 2;
        case 67: return (uint8_t)(((m->card_dram_size >> 20) - 1) & 0x3F);
        case 96: return 12;
        case 97: return 11;
        case 98: return 1;
        case 101: return 127;
        default: return 0;
    }
}

static void eeprom_drive(machine_t *m, bool level) {
    if (m->eeprom_sda_out == level) return;
    m->eeprom_sda_out = level;
    intc_set_pending(m, level ? 2 : 3, MFIO_EEPROM_SDA);
}

static void eeprom_load_byte(machine_t *m) {
    m->eeprom_shift = eeprom_byte(m, m->eeprom_addr);
    m->eeprom_addr++;
    eeprom_drive(m, (m->eeprom_shift & 0x80) != 0);
    m->eeprom_shift <<= 1;
}

static void eeprom_pins(machine_t *m) {
    if (!m->card_dram_size) return;
    uint32_t outputs = m->mfio_direc & m->mfio_sel;
    bool scl = !(outputs & MFIO_EEPROM_SCL) || (m->mfio_dout & MFIO_EEPROM_SCL);
    bool sda = !(outputs & MFIO_EEPROM_SDA) || (m->mfio_dout & MFIO_EEPROM_SDA);
    bool previous_scl = m->eeprom_scl, previous_sda = m->eeprom_sda;
    m->eeprom_scl = scl;
    m->eeprom_sda = sda;
    if (scl && previous_scl && sda != previous_sda) {
        m->eeprom_bit = 0;
        m->eeprom_in_ack = false;
        if (sda) {
            m->eeprom_phase = EEPROM_IDLE;
        } else {
            m->eeprom_phase = EEPROM_CONTROL;
            m->eeprom_shift = 0;
            m->eeprom_selected = false;
        }
        eeprom_drive(m, true);
        return;
    }
    if (m->eeprom_phase == EEPROM_IDLE) return;
    if (scl && !previous_scl) {
        if (m->eeprom_bit < 8) {
            if (m->eeprom_phase != EEPROM_TRANSMIT) m->eeprom_shift = (uint8_t)(m->eeprom_shift << 1 | (sda ? 1 : 0));
            m->eeprom_bit++;
        } else if (m->eeprom_phase == EEPROM_TRANSMIT && sda) {
            m->eeprom_phase = EEPROM_IDLE;
            m->eeprom_bit = 0;
            m->eeprom_in_ack = false;
            eeprom_drive(m, true);
        }
    } else if (!scl && previous_scl) {
        if (m->eeprom_in_ack) {
            m->eeprom_in_ack = false;
            m->eeprom_bit = 0;
            switch (m->eeprom_phase) {
                case EEPROM_CONTROL:
                    if (!m->eeprom_selected) { m->eeprom_phase = EEPROM_IDLE; eeprom_drive(m, true); }
                    else if (m->eeprom_read) { m->eeprom_phase = EEPROM_TRANSMIT; eeprom_load_byte(m); }
                    else { m->eeprom_phase = EEPROM_WORD; eeprom_drive(m, true); }
                    break;
                case EEPROM_WORD:
                    m->eeprom_phase = EEPROM_WRITE;
                    eeprom_drive(m, true);
                    break;
                case EEPROM_TRANSMIT:
                    eeprom_load_byte(m);
                    break;
                default:
                    break;
            }
            return;
        }
        if (m->eeprom_bit == 8) {
            m->eeprom_in_ack = true;
            bool acknowledge = true;
            if (m->eeprom_phase == EEPROM_CONTROL) {
                m->eeprom_selected = (m->eeprom_shift >> 1) == EEPROM_ADDRESS;
                m->eeprom_read = (m->eeprom_shift & 1) != 0;
                acknowledge = m->eeprom_selected;
            } else if (m->eeprom_phase == EEPROM_WORD) {
                m->eeprom_addr = m->eeprom_shift;
            }
            eeprom_drive(m, m->eeprom_phase == EEPROM_TRANSMIT ? true : !acknowledge);
            return;
        }
        if (m->eeprom_phase == EEPROM_TRANSMIT) {
            eeprom_drive(m, (m->eeprom_shift & 0x80) != 0);
            m->eeprom_shift <<= 1;
        }
    }
}

static uint32_t soc_read(machine_t *m, uint32_t offset, int size) {
    uint32_t index = offset / 4;
    switch (offset & ~3u) {
        case 0x0A0: return m->regs[0x0A0 / 4] | (m->ir_cardet ? IR_CARDET : 0);
        case 0x074: return m->sib_ctl | (m->pen_irq_status ? SIB_IRQ : 0);
        case 0x080: return m->sib_sf0_aux;
        case 0x088: return m->sib_sf0_stat;
        case 0x090: return m->sib_dma_ctl;
        case 0x100: case 0x104: case 0x108: case 0x10C: case 0x110:
            return m->intc_status[(offset - 0x100) / 4];
        case 0x114: {
            uint32_t value = (high_priority_level(m) & 0xFu) << 2;
            if (irq_high(m)) value |= 1u << 31;
            if (irq_low(m)) value |= 1u << 30;
            return value;
        }
        case 0x118: case 0x11C: case 0x120: case 0x124: case 0x128:
            return m->intc_enable[(offset - 0x118) / 4];
        case 0x12C: return m->intc_enable6;
        case 0x140: return (uint32_t)(rtc_count(m) >> 32);
        case 0x144: return (uint32_t)rtc_count(m);
        case 0x148: return (uint32_t)(m->alarm >> 32);
        case 0x14C: return (uint32_t)m->alarm;
        case 0x150: return m->timer_ctl;
        case 0x154: {
            uint32_t count = m->perval;
            if ((m->timer_ctl & TIMER_ENPERTIMER) && m->periodic_next != NO_EVENT) {
                uint64_t remaining = m->periodic_next > m->cpu.cycles ? (m->periodic_next - m->cpu.cycles) / 32 : 0;
                count = remaining > m->perval ? m->perval : (uint32_t)remaining;
            }
            return (count << 16) | m->perval;
        }
        case 0x160: return m->spi_ctl | SPI_EMPTY | ((m->spi_ctl & SPI_ENSPI) ? SPI_SPION : 0);
        case 0x164: {
            if (!m->key_count) return 0;
            uint8_t byte = m->key_queue[m->key_head];
            m->key_head = (m->key_head + 1) % KEY_QUEUE_SIZE;
            m->key_count--;
            if (m->key_count) intc_set_pending(m, STATUS5_SET, STATUS5_SPIRCV);
            return byte;
        }
        case 0x180: {
            uint32_t direction = (m->io_ctl >> 16) & 0x7F;
            uint32_t board = IODIN_MMODULE_ATTACHED | (m->serial_connected ? 0 : IODIN_SERIAL_DCD) | (m->card_dram_size ? 0 : IODIN_MINICARD1_ABSENT) | IODIN_MINICARD2_ABSENT;
            uint32_t din = ((m->io_ctl >> 8) & direction) | (board & ~direction & 0x7F);
            return m->io_ctl | din;
        }
        case 0x184: return m->mfio_dout;
        case 0x188: return m->mfio_direc;
        case 0x18C: {
            uint32_t outputs = m->mfio_direc & m->mfio_sel;
            uint32_t inputs = (m->serial_connected ? 0 : MFIO_SERIAL_CTS) | ((m->card_dram_size && m->eeprom_sda_out) ? MFIO_EEPROM_SDA : 0);
            return (m->mfio_dout & outputs) | (inputs & ~outputs);
        }
        case 0x0B0: case 0x0B4: case 0x0B8: case 0x0BC: case 0x0C0: case 0x0C4:
            return uart_read(&m->uart_port, offset - 0x0B0);
        case 0x190: return m->mfio_sel;
        case 0x1C4: return m->power_ctl | POWER_PWROK | (m->power_button ? POWER_ONBUTN : 0);
        default:
            if (index < REG_COUNT) {
                note_access(m, "soc read", REGS_PA + offset, size, m->regs[index]);
                return m->regs[index];
            }
            note_access(m, "soc read (out of range)", REGS_PA + offset, size, 0);
            return 0;
    }
}

static void power_write(machine_t *m, uint32_t value) {
    uint32_t previous = m->power_ctl;
    m->power_ctl = value & POWER_WRITABLE;
    if ((previous & (POWER_VCCON | POWER_PWRCS)) && !(m->power_ctl & (POWER_VCCON | POWER_PWRCS))) {
        machine_logf(m, "suspend pc=%08X\n", m->cpu.pc);
        m->suspended = true;
        m->suspended_at = m->cpu.cycles;
        m->cpu.yield = true;
    }
    bool was_stop = (previous & POWER_ENSTPTIMER) != 0, now_stop = (m->power_ctl & POWER_ENSTPTIMER) != 0;
    if (now_stop && !was_stop) {
        uint32_t ticks = (m->power_ctl >> POWER_STPTIMERVAL_SHIFT) & 0xF;
        m->stop_timer_next = m->cpu.cycles + (uint64_t)ticks * STOP_TIMER_TICK_CYCLES;
    } else if (was_stop && !now_stop) {
        m->stop_timer_next = NO_EVENT;
    }
    if (m->power_ctl & POWER_STOPCPU) {
        m->cpu_stopped = true;
        m->cpu.yield = true;
    }
    if ((m->power_ctl ^ previous) & ~(POWER_STOPCPU | POWER_ENSTPTIMER | (0xFu << POWER_STPTIMERVAL_SHIFT))) {
        machine_logf(m, "power ctl %08X -> %08X pc=%08X\n", previous, m->power_ctl, m->cpu.pc);
    }
}

static void soc_write(machine_t *m, uint32_t offset, int size, uint32_t value) {
    uint32_t index = offset / 4;
    if (size != 4) note_access(m, "soc narrow write", REGS_PA + offset, size, value);
    switch (offset & ~3u) {
        case 0x028:
            m->regs[index] = value & 0x003FFFFFu;
            lcd_schedule(m);
            if (!(value & LCD_ENVID)) m->lcd_next = NO_EVENT;
            return;
        case 0x02C:
            m->regs[index] = value;
            m->lcd_next = NO_EVENT;
            lcd_schedule(m);
            return;
        case 0x074: {
            m->sib_ctl = value & 0x7FFFFFFFu;
            bool active = (m->sib_ctl & (SIB_ENSIB | SIB_ENSF0)) == (SIB_ENSIB | SIB_ENSF0);
            intc_free_running(m, 0, STATUS1_SIBSF0 | STATUS1_SIBSF1, active);
            if (value & 0x6000002Cu) note_access(m, "sib ctl unmodelled bits", REGS_PA + offset, size, value);
            sound_update(m);
            return;
        }
        case 0x060:
            m->snd_size = (value >> 18) & 0xFFF;
            return;
        case 0x068:
            m->snd_tx_start = value & ~3u;
            return;
        case 0x090: {
            m->sib_dma_ctl = value;
            if (value & 0x8002C003u) note_access(m, "sib dma ctl unmodelled channel", REGS_PA + offset, size, value);
            sound_update(m);
            return;
        }
        case 0x080: {
            m->sib_sf0_aux = value;
            uint8_t reg = (uint8_t)((value >> 27) & 0xF);
            uint16_t data = (uint16_t)(value & 0xFFFF);
            if (value & (1u << 26)) ucb_write(m, reg, data);
            else m->sib_sf0_stat = ucb_read(m, reg);
            return;
        }
        case 0x100: case 0x104: case 0x108: case 0x10C: case 0x110: {
            int set = (int)((offset - 0x100) / 4);
            m->intc_status[set] &= ~value;
            m->intc_status[set] |= m->intc_free_running[set];
            intc_update(m);
            return;
        }
        case 0x118: case 0x11C: case 0x120: case 0x124: case 0x128: {
            int set = (int)((offset - 0x118) / 4);
            m->intc_enable[set] = value;
            intc_update(m);
            intc_enable_written(m, set, value);
            return;
        }
        case 0x12C:
            m->intc_enable6 = value & (ENABLE6_GLOBALEN | 0xFFFFu);
            intc_update(m);
            return;
        case 0x148:
            m->alarm = ((uint64_t)(value & 0xFF) << 32) | (m->alarm & 0xFFFFFFFFull);
            m->alarm_armed = true;
            alarm_schedule(m);
            return;
        case 0x14C:
            m->alarm = (m->alarm & ~0xFFFFFFFFull) | value;
            m->alarm_armed = true;
            alarm_schedule(m);
            return;
        case 0x150: {
            bool was_clear = (m->timer_ctl & TIMER_RTCCLR) != 0;
            bool was_periodic = (m->timer_ctl & TIMER_ENPERTIMER) != 0;
            if (value & 0xE7u) note_access(m, "timer ctl unmodelled bits", REGS_PA + offset, size, value);
            m->timer_ctl = value & 0xFFu;
            if (was_clear && !(value & TIMER_RTCCLR)) {
                m->rtc_base = 0;
                m->rtc_anchor = m->cpu.cycles;
            }
            if ((value & TIMER_ENPERTIMER) && !was_periodic) m->periodic_next = m->cpu.cycles + periodic_period(m);
            if (!(value & TIMER_ENPERTIMER)) m->periodic_next = NO_EVENT;
            alarm_schedule(m);
            return;
        }
        case 0x154: m->perval = value & 0xFFFF; return;
        case 0x0B0: case 0x0B4: case 0x0B8: case 0x0BC: case 0x0C0: case 0x0C4:
            uart_write(&m->uart_port, offset - 0x0B0, value, m->cpu.cycles);
            m->cpu.yield = true;
            return;
        case 0x0A0: m->regs[0x0A0 / 4] = value & 0x00FF000Cu; return;
        case 0x160:
            m->spi_ctl = value & 0x0000FF37u;
            intc_free_running(m, STATUS5_SET, STATUS5_SPIBUFAVAIL, (m->spi_ctl & SPI_ENSPI) != 0);
            return;
        case 0x164: return;
        case 0x180: m->io_ctl = value & 0x7F7F7F00u; return;
        case 0x184: m->mfio_dout = value; eeprom_pins(m); return;
        case 0x188: m->mfio_direc = value; eeprom_pins(m); return;
        case 0x190: m->mfio_sel = value; eeprom_pins(m); return;
        case 0x1C4: power_write(m, value); return;
        default:
            if (index < REG_COUNT) {
                m->regs[index] = value;
                note_access(m, "soc write", REGS_PA + offset, size, value);
                return;
            }
            note_access(m, "soc write (out of range)", REGS_PA + offset, size, value);
            return;
    }
}

static inline uint32_t read_host(const uint8_t *base, int size) {
    switch (size) {
        case 1: return base[0];
        case 2: return (uint32_t)base[0] | (uint32_t)base[1] << 8;
        default: return (uint32_t)base[0] | (uint32_t)base[1] << 8 | (uint32_t)base[2] << 16 | (uint32_t)base[3] << 24;
    }
}

static inline void write_host(uint8_t *base, int size, uint32_t value) {
    base[0] = (uint8_t)value;
    if (size >= 2) base[1] = (uint8_t)(value >> 8);
    if (size == 4) { base[2] = (uint8_t)(value >> 16); base[3] = (uint8_t)(value >> 24); }
}

static uint8_t boot_block[BOOT_BLOCK_SIZE] = { 0x00, 0x00, 0xF0, 0x0B };

static bool bus_read(void *context, uint32_t pa, int size, uint32_t *value) {
    machine_t *m = context;
    if (pa >= BOOT_BLOCK_PA && pa < BOOT_BLOCK_PA + BOOT_BLOCK_SIZE) { *value = read_host(boot_block + (pa - BOOT_BLOCK_PA), size); return true; }
    if (pa < DRAM_DECODE_END) { *value = read_host(m->dram + (pa & (m->dram_size - 1)), size); return true; }
    if ((pa >= ROM_WINDOW_PA && pa < ROM_WINDOW_END) || (pa >= ROM_CARD_PA && pa < ROM_CARD_END)) {
        if (pa >= m->rom_pa && pa - m->rom_pa + (uint32_t)size <= m->rom_size) { *value = read_host(m->rom + (pa - m->rom_pa), size); return true; }
        if (m->rom2 && pa >= m->rom2_pa && pa - m->rom2_pa + (uint32_t)size <= m->rom2_size) { *value = read_host(m->rom2 + (pa - m->rom2_pa), size); return true; }
        *value = size == 4 ? 0xFFFFFFFFu : size == 2 ? 0xFFFFu : 0xFFu;
        return true;
    }
    if (pa >= REGS_PA && pa < REGS_END) { *value = soc_read(m, pa - REGS_PA, size); return true; }
    if (pa == DEBUG_PROBE_PA) { *value = 0xFFFF; return true; }
    if (pa >= CS2_PA && pa < CS2_PA + PCCARD_IT8368_SIZE) {
        uint32_t offset = (pa - CS2_PA) & ~1u;
        *value = pccard_it8368_read(&m->card_socket, offset);
        if (size == 4) *value |= (uint32_t)pccard_it8368_read(&m->card_socket, offset + 2) << 16;
        return true;
    }
    if ((pa >= PCCARD_CTRL_WINDOW_PA && pa < PCCARD_CTRL_WINDOW_END) || (pa >= PCCARD_MEM_WINDOW_PA && pa < PCCARD_MEM_WINDOW_END)) {
        *value = pccard_read(&m->card_socket, pa, size);
        return true;
    }
    if (pa >= DRAM_DECODE_END && pa < BANK1_DECODE_END) {
        if (m->card_dram_size) { *value = read_host(m->card_dram + (pa & (m->card_dram_size - 1)), size); return true; }
        note_access(m, "bank1 read", pa, size, 0);
        *value = 0xFFFFFFFFu >> (32 - size * 8);
        return true;
    }
    note_access(m, "unmapped read", pa, size, 0);
    *value = 0;
    return true;
}

static bool bus_write(void *context, uint32_t pa, int size, uint32_t value) {
    machine_t *m = context;
    if (pa < DRAM_DECODE_END) { write_host(m->dram + (pa & (m->dram_size - 1)), size, value); return true; }
    if ((pa >= ROM_WINDOW_PA && pa < ROM_WINDOW_END) || (pa >= ROM_CARD_PA && pa < ROM_CARD_END)) { note_access(m, "rom write", pa, size, value); return true; }
    if (pa >= REGS_PA && pa < REGS_END) { soc_write(m, pa - REGS_PA, size, value); return true; }
    if (pa >= CS2_PA && pa < CS2_PA + PCCARD_IT8368_SIZE) {
        uint32_t offset = (pa - CS2_PA) & ~1u;
        pccard_it8368_write(&m->card_socket, offset, (uint16_t)value);
        if (size == 4) pccard_it8368_write(&m->card_socket, offset + 2, (uint16_t)(value >> 16));
        return true;
    }
    if ((pa >= PCCARD_CTRL_WINDOW_PA && pa < PCCARD_CTRL_WINDOW_END) || (pa >= PCCARD_MEM_WINDOW_PA && pa < PCCARD_MEM_WINDOW_END)) {
        pccard_write(&m->card_socket, pa, size, value);
        return true;
    }
    if (pa >= DRAM_DECODE_END && pa < BANK1_DECODE_END && m->card_dram_size) {
        write_host(m->card_dram + (pa & (m->card_dram_size - 1)), size, value);
        return true;
    }
    note_access(m, "unmapped write", pa, size, value);
    return true;
}

static uint8_t *bus_fetch_page(void *context, uint32_t pa) {
    machine_t *m = context;
    if (pa >= BOOT_BLOCK_PA && pa < BOOT_BLOCK_PA + BOOT_BLOCK_SIZE) return boot_block + (pa - BOOT_BLOCK_PA);
    if (pa < DRAM_DECODE_END) return m->dram + (pa & (m->dram_size - 1));
    if (pa >= DRAM_DECODE_END && pa < BANK1_DECODE_END && m->card_dram_size) return m->card_dram + (pa & (m->card_dram_size - 1));
    if (pa >= m->rom_pa && pa - m->rom_pa + 4096 <= m->rom_size) return m->rom + (pa - m->rom_pa);
    if (m->rom2 && pa >= m->rom2_pa && pa - m->rom2_pa + 4096 <= m->rom2_size) return m->rom2 + (pa - m->rom2_pa);
    return NULL;
}

static void card_int_changed(void *context, bool asserted) {
    machine_t *m = context;
    if (m->ir_cardet == asserted) return;
    m->ir_cardet = asserted;
    intc_set_pending(m, STATUS5_SET, asserted ? STATUS5_POSCARINT : STATUS5_NEGCARINT);
}

static void uart_raise(void *context, uint32_t bits) {
    intc_set_pending(context, 1, bits);
}

static void bind_uart(machine_t *m) {
    m->uart_port.state = &m->uart_a;
    m->uart_port.dram = m->dram;
    m->uart_port.dram_mask = m->dram_size - 1;
    m->uart_port.context = m;
    m->uart_port.raise = uart_raise;
}

static void bind_card_socket(machine_t *m, FILE *image) {
    m->card_socket.state = &m->pccard;
    m->card_socket.image = image;
    m->card_socket.int_changed = card_int_changed;
    m->card_socket.context = m;
}

static void machine_power_on(machine_t *m) {
    pccard_reset(&m->card_socket);
    m->uart_a.rx_next = NO_EVENT;
    mips_reset(&m->cpu, m->entry_va);
    m->power_ctl = POWER_COLDSTART | POWER_PWRCS | POWER_VCCON;
    m->periodic_next = NO_EVENT;
    m->alarm_next = NO_EVENT;
    m->stop_timer_next = NO_EVENT;
    m->lcd_next = NO_EVENT;
    m->sound_next = NO_EVENT;
    m->regs[0x1C0 / 4] = 1u << 7;
}

typedef struct {
    uint32_t offset;
    uint32_t original;
    uint32_t replacement;
} rom_patch_t;

static const rom_patch_t rom_patches[] = {
    { 0x1B4FCCu, 0x03231023u, 0x03251023u },
};

static void apply_rom_patches(machine_t *m) {
    if (m->rom_pa != ROM_PA) return;
    for (size_t i = 0; i < sizeof rom_patches / sizeof rom_patches[0]; i++) {
        const rom_patch_t *patch = &rom_patches[i];
        if (patch->offset + 4 > m->rom_size) continue;
        uint8_t *word = m->rom + patch->offset;
        uint32_t value = (uint32_t)word[0] | (uint32_t)word[1] << 8 | (uint32_t)word[2] << 16 | (uint32_t)word[3] << 24;
        if (value != patch->original) continue;
        for (int b = 0; b < 4; b++) word[b] = (uint8_t)(patch->replacement >> (8 * b));
    }
}

static uint32_t read_le32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static bool find_image_start(const uint8_t *rom, size_t rom_size, uint32_t *start) {
    for (size_t offset = 0; offset + 16 <= rom_size; offset += 4) {
        uint32_t physfirst = read_le32(rom + offset + 8), physlast = read_le32(rom + offset + 12);
        if (physlast <= physfirst || physlast - physfirst != rom_size || (physfirst & 0xE0000000u) != 0x80000000u) continue;
        if (physfirst + offset >= physlast) continue;
        *start = physfirst;
        return true;
    }
    return false;
}

typedef struct {
    uint32_t pa, size;
    uint8_t *data;
} rom_region_t;

static bool rom_window(uint32_t pa, uint32_t *window_start, uint32_t *window_end) {
    if (pa >= ROM_CARD_PA && pa < ROM_CARD_END) { *window_start = ROM_CARD_PA; *window_end = ROM_CARD_END; return true; }
    if (pa >= ROM_WINDOW_PA && pa < ROM_WINDOW_END) { *window_start = ROM_WINDOW_PA; *window_end = ROM_WINDOW_END; return true; }
    return false;
}

static int load_b000ff(const uint8_t *file, size_t size, rom_region_t regions[2], uint32_t *entry, char *error, size_t error_size) {
    uint32_t low[2] = { UINT32_MAX, UINT32_MAX }, high[2] = { 0, 0 }, windows[2] = { 0, 0 };
    int count = 0;
    bool ended = false;
    for (int pass = 0; pass < 2; pass++) {
        size_t p = 15;
        while (p + 12 <= size) {
            uint32_t address = read_le32(file + p), length = read_le32(file + p + 4), checksum = read_le32(file + p + 8);
            if (address == 0 && checksum == 0) {
                *entry = length;
                ended = true;
                break;
            }
            if (p + 12 + length > size) break;
            const uint8_t *data = file + p + 12;
            uint32_t pa = address & KSEG_PA_MASK, window_start, window_end;
            if (!rom_window(pa, &window_start, &window_end) || pa + length > window_end) {
                snprintf(error, error_size, "B000FF record at VA %08X is outside the ROM windows", address);
                return -1;
            }
            int slot = 0;
            while (slot < count && windows[slot] != window_start) slot++;
            if (pass == 0) {
                uint32_t sum = 0;
                for (uint32_t i = 0; i < length; i++) sum += data[i];
                if (sum != checksum) {
                    snprintf(error, error_size, "B000FF record at VA %08X has a bad checksum", address);
                    return -1;
                }
                if (slot == count) {
                    if (count == 2) {
                        snprintf(error, error_size, "B000FF image uses more than two ROM windows");
                        return -1;
                    }
                    windows[count++] = window_start;
                }
                if (pa < low[slot]) low[slot] = pa;
                if (pa + length > high[slot]) high[slot] = pa + length;
            } else {
                memcpy(regions[slot].data + (pa - regions[slot].pa), data, length);
            }
            p += 12 + length;
        }
        if (!ended || count == 0) {
            snprintf(error, error_size, "B000FF image has no entry record");
            return -1;
        }
        if (pass == 0) {
            for (int i = 0; i < count; i++) {
                regions[i].pa = low[i] & ~4095u;
                regions[i].size = ((high[i] - regions[i].pa) + 4095) & ~4095u;
                regions[i].data = malloc(regions[i].size);
                memset(regions[i].data, 0xFF, regions[i].size);
            }
        }
    }
    return count;
}

machine_t *machine_create(const uint8_t *rom, size_t rom_size, char *error, size_t error_size) {
    rom_region_t regions[2] = { { 0 } };
    int region_count;
    uint32_t start = ENTRY_VA;
    if (rom_size >= 15 && !memcmp(rom, "B000FF\n", 7)) {
        region_count = load_b000ff(rom, rom_size, regions, &start, error, error_size);
        if (region_count < 0) {
            for (int i = 0; i < 2; i++) free(regions[i].data);
            return NULL;
        }
        uint32_t entry_pa = start & KSEG_PA_MASK;
        if (region_count == 2 && !(entry_pa >= regions[0].pa && entry_pa < regions[0].pa + regions[0].size)) {
            rom_region_t swap = regions[0];
            regions[0] = regions[1];
            regions[1] = swap;
        }
    } else {
        uint32_t window_end = ROM_WINDOW_END;
        if (rom_size >= 16 && find_image_start(rom, rom_size, &start)) window_end = ROM_CARD_END;
        uint32_t rom_pa = start & KSEG_PA_MASK;
        bool placeable = (rom_pa >= ROM_PA && rom_pa < ROM_WINDOW_END) || (rom_pa >= ROM_CARD_PA && rom_pa < ROM_CARD_END);
        if (rom_size < 16 || !placeable || rom_size > window_end - rom_pa) {
            snprintf(error, error_size, "ROM size %zu is not a Velo 1 nk.bin", rom_size);
            return NULL;
        }
        regions[0].pa = rom_pa;
        regions[0].size = (uint32_t)((rom_size + 4095) & ~(size_t)4095);
        regions[0].data = malloc(regions[0].size);
        memset(regions[0].data, 0xFF, regions[0].size);
        memcpy(regions[0].data, rom, rom_size);
        region_count = 1;
    }
    machine_t *m = calloc(1, sizeof *m);
    m->rom_pa = regions[0].pa;
    m->rom_size = regions[0].size;
    m->rom = regions[0].data;
    if (region_count == 2) {
        m->rom2_pa = regions[1].pa;
        m->rom2_size = regions[1].size;
        m->rom2 = regions[1].data;
    }
    m->entry_va = start;
    m->dram_size = m->dram_size_next = DRAM_SIZE;
    m->eeprom_scl = m->eeprom_sda = m->eeprom_sda_out = true;
    m->dram = calloc(1, m->dram_size);
    m->rom_hash = 0xCBF29CE484222325ull;
    for (size_t i = 0; i < rom_size; i++) m->rom_hash = (m->rom_hash ^ rom[i]) * 0x100000001B3ull;
    apply_rom_patches(m);
    m->cpu.bus.context = m;
    m->cpu.bus.read = bus_read;
    m->cpu.bus.write = bus_write;
    m->cpu.bus.fetch_page = bus_fetch_page;
    bind_card_socket(m, NULL);
    bind_uart(m);
    machine_power_on(m);
    return m;
}

void machine_destroy(machine_t *m) {
    if (!m) return;
    free(m->card_dram);
    if (m->card_socket.image) fclose(m->card_socket.image);
    if (m->pending_card) fclose(m->pending_card);
    free(m->dram);
    free(m->rom);
    free(m->rom2);
    free(m);
}

void machine_set_log(machine_t *m, machine_log_fn log) { m->log = log; }

static uint64_t next_event(const machine_t *m) {
    uint64_t next = m->periodic_next;
    if (m->alarm_next < next) next = m->alarm_next;
    if (m->stop_timer_next < next) next = m->stop_timer_next;
    if (m->lcd_next < next) next = m->lcd_next;
    if (m->sound_active && m->sound_next < next) next = m->sound_next;
    if (uart_next_event(&m->uart_port) < next) next = uart_next_event(&m->uart_port);
    if (m->pending_card && m->card_insert_at < next) next = m->card_insert_at;
    return next;
}

static void process_events(machine_t *m) {
    uint64_t now = m->cpu.cycles;
    if (m->periodic_next <= now) {
        intc_set_pending(m, STATUS5_SET, STATUS5_PERINT);
        uint64_t period = periodic_period(m);
        while (m->periodic_next <= now) m->periodic_next += period;
    }
    if (m->alarm_next <= now) {
        m->alarm_next = NO_EVENT;
        m->alarm_armed = false;
        intc_set_pending(m, STATUS5_SET, STATUS5_ALARMINT);
    }
    if (m->stop_timer_next <= now) {
        m->stop_timer_next = NO_EVENT;
        intc_set_pending(m, STATUS5_SET, STATUS5_STPTIMER);
    }
    if (m->sound_active && m->sound_next <= now) sound_event(m);
    uart_event(&m->uart_port, now);
    if (m->pending_card && m->card_insert_at <= now) {
        memcpy(m->card_path, m->pending_card_path, sizeof m->card_path);
        pccard_insert(&m->card_socket, m->pending_card);
        m->pending_card = NULL;
    }
    if (m->lcd_next <= now) {
        intc_set_pending(m, 0, STATUS1_LCDINT);
        uint64_t period = lcd_frame_cycles(m);
        while (m->lcd_next <= now) m->lcd_next += period;
    }
}

static void shift_deadline(uint64_t *deadline, uint64_t by) {
    if (*deadline != NO_EVENT) *deadline += by;
}

static void wake_from_suspend(machine_t *m) {
    uint64_t asleep = m->cpu.cycles - m->suspended_at;
    m->suspended = false;
    m->suspended_cycles += asleep;
    shift_deadline(&m->periodic_next, asleep);
    shift_deadline(&m->stop_timer_next, asleep);
    shift_deadline(&m->lcd_next, asleep);
    shift_deadline(&m->sound_next, asleep);
    shift_deadline(&m->uart_a.rx_next, asleep);
    m->power_ctl |= POWER_PWRCS | POWER_VCCON | POWER_FORCESHUTDWN;
    m->keyboard_enabled = false;
    m->key_count = 0;
    m->intc_status[STATUS5_SET] &= ~STATUS5_SPIRCV;
    intc_update(m);
    machine_logf(m, "resume after %.1fs pc=%08X\n", (double)asleep / MACHINE_CLOCK_HZ, m->cpu.pc);
}

static void run_suspended(machine_t *m, uint64_t target) {
    uint64_t until = m->alarm_next < target ? m->alarm_next : target;
    if (until > m->cpu.cycles) m->cpu.cycles = until;
    if (m->alarm_next <= m->cpu.cycles) {
        m->alarm_next = NO_EVENT;
        m->alarm_armed = false;
        intc_set_pending(m, STATUS5_SET, STATUS5_ALARMINT);
    }
    if (irq_low(m) || irq_high(m)) wake_from_suspend(m);
}

static void reset_machine(machine_t *m, bool keep_ram);

void machine_run(machine_t *m, uint64_t cycles) {
    uint64_t target = m->cpu.cycles + cycles;
    while (!m->halted && m->cpu.cycles < target) {
        if (m->suspended) {
            run_suspended(m, target);
            continue;
        }
        if (m->cpu_stopped && m->cpu.external_ip) {
            m->cpu_stopped = false;
            m->power_ctl &= ~POWER_STOPCPU;
        }
        if ((m->cpu.pc & KSEG_PA_MASK) >= BOOT_BLOCK_PA && (m->cpu.pc & KSEG_PA_MASK) < BOOT_BLOCK_PA + 8) {
            machine_logf(m, "boot block: warm reset\n");
            reset_machine(m, true);
            continue;
        }
        uint64_t until = next_event(m);
        if (until > target) until = target;
        if (m->cpu_stopped) {
            if (until > m->cpu.cycles) {
                m->stopped_cycles += until - m->cpu.cycles;
                m->cpu.cycles = until;
            }
        } else if (until > m->cpu.cycles) {
            mips_run(&m->cpu, until);
        }
        process_events(m);
    }
}

uint64_t machine_cycles(machine_t *m) { return m->cpu.cycles; }
uint32_t machine_pc(machine_t *m) { return m->cpu.pc; }
bool machine_halted(machine_t *m) { return m->halted; }
const char *machine_halt_reason(machine_t *m) { return m->halt_reason; }

static uint32_t mfio_driven(const machine_t *m) {
    return m->mfio_dout & m->mfio_direc & m->mfio_sel;
}

bool machine_lcd_enabled(machine_t *m) {
    uint32_t ctl1 = m->regs[0x28 / 4];
    return (ctl1 & LCD_ENVID) && (ctl1 & LCD_DISPON) && !(mfio_driven(m) & MFIO_LCD_POWER_OFF);
}

bool machine_backlight(machine_t *m) {
    return machine_lcd_enabled(m) && (mfio_driven(m) & MFIO_BACKLIGHT);
}

void machine_backlight_button(machine_t *m, bool down) {
    machine_key(m, SCANCODE_BACKLIGHT, !down);
}

static uint32_t lcd_shade(const machine_t *m, uint32_t raw, uint32_t bpp) {
    uint32_t vdat;
    if (bpp == 2) vdat = (m->regs[0x40 / 4] >> (4 * raw)) & 0xF;
    else if (bpp == 1) vdat = raw ? 0xF : 0;
    else vdat = raw;
    uint32_t on_duty = (m->regs[0x28 / 4] & LCD_INVVID) ? 0xF - vdat : vdat;
    return on_duty;
}

bool machine_screen(machine_t *m, uint8_t *levels) {
    uint32_t ctl1 = m->regs[0x28 / 4], ctl2 = m->regs[0x2C / 4];
    if (!(ctl1 & LCD_ENVID)) {
        memset(levels, 0, MACHINE_SCREEN_WIDTH * MACHINE_SCREEN_HEIGHT);
        return false;
    }
    uint32_t bpp = 1u << ((ctl1 >> 6) & 3);
    uint32_t width = (((ctl2 >> 12) & 0x1FF) + 1) * ((ctl1 & LCD_DISP8) ? 8 : 4);
    uint32_t height = (ctl2 & 0x3FF) + 1;
    uint32_t base = m->regs[0x30 / 4] & 0xFFFFFFF0u;
    uint32_t stride = width * bpp / 8;
    for (int y = 0; y < MACHINE_SCREEN_HEIGHT; y++) {
        for (int x = 0; x < MACHINE_SCREEN_WIDTH; x++) {
            uint8_t level = 0;
            if ((uint32_t)x < width && (uint32_t)y < height) {
                uint32_t bit = (uint32_t)x * bpp;
                uint32_t pa = base + (uint32_t)y * stride + bit / 8;
                uint32_t byte = pa < DRAM_DECODE_END ? m->dram[pa & (m->dram_size - 1)] : 0;
                uint32_t raw = (byte >> (8 - bpp - bit % 8)) & ((1u << bpp) - 1);
                uint32_t on_duty = lcd_shade(m, raw, bpp);
                level = bpp == 4 ? (uint8_t)on_duty : (uint8_t)((on_duty * 3 + 7) / 15 * 5);
            }
            levels[y * MACHINE_SCREEN_WIDTH + x] = level;
        }
    }
    return true;
}

void machine_key(machine_t *m, uint8_t scancode, bool up) {
    keyboard_stage_enable(m);
    key_push(m, up ? (uint8_t)(scancode | 0x80) : scancode);
    intc_set_pending(m, STATUS5_SET, STATUS5_SPIRCV);
}

void machine_power_button(machine_t *m, bool down) {
    if (down == m->power_button) return;
    m->power_button = down;
    intc_set_pending(m, STATUS5_SET, down ? STATUS5_POSONBUTN : STATUS5_NEGONBUTN);
    if (down && m->suspended) wake_from_suspend(m);
}

bool machine_suspended(machine_t *m) { return m->suspended; }

void machine_touch(machine_t *m, bool down, int x, int y) {
    m->pen_x = x;
    m->pen_y = y;
    bool was_down = m->pen_down;
    m->pen_down = down;
    if (down && !was_down && m->pen_irq_armed) {
        m->pen_irq_status |= m->pen_irq_armed;
        intc_set_pending(m, 0, STATUS1_SIBIRQPOS);
    }
}

void machine_dump_state(machine_t *m) {
    mips_cpu_t *cpu = &m->cpu;
    machine_logf(m, "pc=%08X cycles=%llu status=%08X cause=%08X epc=%08X badvaddr=%08X ip=%08X\n",
                 cpu->pc, (unsigned long long)cpu->cycles, cpu->cp0[CP0_STATUS], cpu->cp0[CP0_CAUSE],
                 cpu->cp0[CP0_EPC], cpu->cp0[CP0_BADVADDR], cpu->external_ip);
    for (int i = 0; i < 32; i += 4) {
        machine_logf(m, "r%-2d %08X %08X %08X %08X\n", i, cpu->gpr[i], cpu->gpr[i + 1], cpu->gpr[i + 2], cpu->gpr[i + 3]);
    }
    machine_logf(m, "intc status %08X %08X %08X %08X %08X enable %08X %08X %08X %08X %08X enable6 %08X\n",
                 m->intc_status[0], m->intc_status[1], m->intc_status[2], m->intc_status[3], m->intc_status[4],
                 m->intc_enable[0], m->intc_enable[1], m->intc_enable[2], m->intc_enable[3], m->intc_enable[4],
                 m->intc_enable6);
    machine_logf(m, "stopped %.1f%% suspended %.1fs%s\n", cpu->cycles ? 100.0 * (double)m->stopped_cycles / (double)cpu->cycles : 0.0,
                 (double)m->suspended_cycles / MACHINE_CLOCK_HZ, m->suspended ? " (now)" : "");
    machine_logf(m, "exceptions:");
    for (int i = 0; i < 16; i++) if (cpu->exceptions[i]) machine_logf(m, " %d=%llu", i, (unsigned long long)cpu->exceptions[i]);
    machine_logf(m, "\n");
}

bool machine_read_virtual(machine_t *m, uint32_t va, uint32_t *value) {
    return mips_read_virtual(&m->cpu, va, 4, value);
}

#define STATE_FIELDS(X) \
    X(cpu_gpr, m->cpu.gpr) X(cpu_hi, m->cpu.hi) X(cpu_lo, m->cpu.lo) X(cpu_pc, m->cpu.pc) \
    X(cpu_next_pc, m->cpu.next_pc) X(cpu_in_delay_slot, m->cpu.in_delay_slot) \
    X(cpu_next_in_delay_slot, m->cpu.next_in_delay_slot) X(cpu_cp0, m->cpu.cp0) \
    X(cpu_external_ip, m->cpu.external_ip) X(cpu_tlb, m->cpu.tlb) X(cpu_random_state, m->cpu.random_state) \
    X(cpu_cycles, m->cpu.cycles) X(cpu_exceptions, m->cpu.exceptions) \
    X(regs, m->regs) X(card_path, m->card_path) X(ir_cardet, m->ir_cardet) \
    X(card_gpio_dataout, m->pccard.gpio_dataout) X(card_gpio_dir, m->pccard.gpio_dir) \
    X(card_gpio_posinten, m->pccard.gpio_posinten) X(card_gpio_neginten, m->pccard.gpio_neginten) \
    X(card_gpio_posintstat, m->pccard.gpio_posintstat) X(card_gpio_negintstat, m->pccard.gpio_negintstat) \
    X(card_mfio_posintstat, m->pccard.mfio_posintstat) X(card_mfio_negintstat, m->pccard.mfio_negintstat) \
    X(card_mfio_dataout, m->pccard.mfio_dataout) X(card_mfio_dir, m->pccard.mfio_dir) \
    X(card_mfio_sel, m->pccard.mfio_sel) X(card_ctrl, m->pccard.ctrl) X(card_prev_datain, m->pccard.prev_datain) \
    X(card_int_asserted, m->pccard.int_asserted) X(card_reset_asserted, m->pccard.reset_asserted) \
    X(card_inserted, m->pccard.inserted) X(card_powered, m->pccard.powered) X(card_irq, m->pccard.card_irq) \
    X(card_feature, m->pccard.feature) X(card_error, m->pccard.error) X(card_sector_count, m->pccard.sector_count) \
    X(card_sector_number, m->pccard.sector_number) X(card_cylinder_low, m->pccard.cylinder_low) \
    X(card_cylinder_high, m->pccard.cylinder_high) X(card_drive_head, m->pccard.drive_head) \
    X(card_status, m->pccard.status) X(card_device_control, m->pccard.device_control) X(card_cor, m->pccard.cor) \
    X(card_buffer, m->pccard.buffer) X(card_buffer_position, m->pccard.buffer_position) \
    X(card_sectors_left, m->pccard.sectors_left) X(card_writing, m->pccard.writing) \
    X(card_total_sectors, m->pccard.total_sectors) \
    X(intc_status, m->intc_status) X(intc_enable, m->intc_enable) X(intc_free_running, m->intc_free_running) \
    X(intc_enable6, m->intc_enable6) X(timer_ctl, m->timer_ctl) X(perval, m->perval) \
    X(periodic_next, m->periodic_next) X(rtc_base, m->rtc_base) X(rtc_anchor, m->rtc_anchor) X(alarm, m->alarm) \
    X(alarm_armed, m->alarm_armed) X(alarm_next, m->alarm_next) X(power_ctl, m->power_ctl) \
    X(cpu_stopped, m->cpu_stopped) X(suspended, m->suspended) X(power_button, m->power_button) \
    X(suspended_at, m->suspended_at) X(suspended_cycles, m->suspended_cycles) X(stopped_cycles, m->stopped_cycles) \
    X(stop_timer_next, m->stop_timer_next) X(lcd_next, m->lcd_next) X(io_ctl, m->io_ctl) \
    X(mfio_dout, m->mfio_dout) X(mfio_direc, m->mfio_direc) X(mfio_sel, m->mfio_sel) X(spi_ctl, m->spi_ctl) \
    X(key_queue, m->key_queue) X(key_head, m->key_head) X(key_count, m->key_count) \
    X(keyboard_enabled, m->keyboard_enabled) X(sib_ctl, m->sib_ctl) X(sib_sf0_aux, m->sib_sf0_aux) \
    X(sib_sf0_stat, m->sib_sf0_stat) X(sib_dma_ctl, m->sib_dma_ctl) X(snd_size, m->snd_size) \
    X(snd_tx_start, m->snd_tx_start) X(sound_active, m->sound_active) X(sound_half, m->sound_half) \
    X(sound_next, m->sound_next) X(audio_rate, m->audio_rate) X(ucb_regs, m->ucb_regs) \
    X(ucb_adc_data, m->ucb_adc_data) X(pen_irq_armed, m->pen_irq_armed) X(pen_irq_status, m->pen_irq_status) \
    X(pen_down, m->pen_down) X(pen_x, m->pen_x) X(pen_y, m->pen_y) X(touch_legacy, m->touch_legacy) \
    X(uart_a_ctl1, m->uart_a.ctl1) X(uart_a_baud_divisor, m->uart_a.baud_divisor) \
    X(uart_a_dma_buffer, m->uart_a.dma_buffer) X(uart_a_dma_length, m->uart_a.dma_length) \
    X(uart_a_dma_count, m->uart_a.dma_count) X(uart_a_dma_armed, m->uart_a.dma_armed) \
    X(serial_connected, m->serial_connected) X(serial_tag, m->serial_tag) \
    X(eeprom_phase, m->eeprom_phase) X(eeprom_shift, m->eeprom_shift) X(eeprom_bit, m->eeprom_bit) \
    X(eeprom_addr, m->eeprom_addr) X(eeprom_selected, m->eeprom_selected) X(eeprom_read, m->eeprom_read) \
    X(eeprom_in_ack, m->eeprom_in_ack) X(eeprom_scl, m->eeprom_scl) X(eeprom_sda, m->eeprom_sda) \
    X(eeprom_sda_out, m->eeprom_sda_out)

static bool write_record(FILE *file, const char *name, const void *data, uint32_t size) {
    uint8_t length = (uint8_t)strlen(name);
    return fwrite(&length, 1, 1, file) == 1 && fwrite(name, length, 1, file) == 1 &&
           fwrite(&size, sizeof size, 1, file) == 1 && (size == 0 || fwrite(data, size, 1, file) == 1);
}

bool machine_save(machine_t *m, const char *path, int64_t host_time) {
    char temporary[1100];
    snprintf(temporary, sizeof temporary, "%s.tmp", path);
    FILE *file = fopen(temporary, "wb");
    if (!file) return false;
    bool ok = fwrite(STATE_MAGIC, sizeof STATE_MAGIC, 1, file) == 1 &&
              fwrite(&m->rom_hash, sizeof m->rom_hash, 1, file) == 1 &&
              fwrite(&host_time, sizeof host_time, 1, file) == 1;
#define SAVE_FIELD(key, field) ok = ok && write_record(file, #key, &(field), (uint32_t)sizeof(field));
    STATE_FIELDS(SAVE_FIELD)
#undef SAVE_FIELD
    ok = ok && write_record(file, "dram", m->dram, m->dram_size);
    if (m->card_dram_size) ok = ok && write_record(file, "dram_card", m->card_dram, m->card_dram_size);
    uint8_t end = 0;
    ok = ok && fwrite(&end, 1, 1, file) == 1;
    ok = fclose(file) == 0 && ok;
    if (ok) ok = rename(temporary, path) == 0;
    else remove(temporary);
    return ok;
}

typedef struct {
    char name[256];
    const uint8_t *data;
    uint32_t size;
} state_record_t;

static bool next_record(const uint8_t **cursor, const uint8_t *end, state_record_t *record) {
    if (*cursor >= end) return false;
    uint8_t length = *(*cursor)++;
    if (length == 0) return false;
    if (end - *cursor < length + 4) return false;
    memcpy(record->name, *cursor, length);
    record->name[length] = 0;
    *cursor += length;
    memcpy(&record->size, *cursor, 4);
    *cursor += 4;
    if ((uint64_t)(end - *cursor) < record->size) return false;
    record->data = *cursor;
    *cursor += record->size;
    return true;
}

static void apply_record(machine_t *m, const state_record_t *record) {
#define LOAD_FIELD(key, field) \
    if (!strcmp(record->name, #key)) { \
        if (record->size == sizeof(field)) memcpy(&(field), record->data, sizeof(field)); \
        else machine_logf(m, "state: %s has size %u, expected %zu; using default\n", #key, record->size, sizeof(field)); \
        return; \
    }
    STATE_FIELDS(LOAD_FIELD)
#undef LOAD_FIELD
    if (!strcmp(record->name, "dram")) {
        if (record->size == m->dram_size) memcpy(m->dram, record->data, m->dram_size);
        return;
    }
    if (!strcmp(record->name, "dram_card")) {
        if (record->size == m->card_dram_size) memcpy(m->card_dram, record->data, m->card_dram_size);
        return;
    }
    machine_logf(m, "state: ignoring unknown record %s\n", record->name);
}

static void cancel_pending_card(machine_t *m);

bool machine_load(machine_t *m, const char *path, int64_t *host_time) {
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    cancel_pending_card(m);
    fseek(file, 0, SEEK_END);
    long length = ftell(file);
    fseek(file, 0, SEEK_SET);
    uint8_t *contents = length > 0 ? malloc((size_t)length) : NULL;
    bool ok = contents && fread(contents, (size_t)length, 1, file) == 1;
    fclose(file);
    size_t header = sizeof STATE_MAGIC + sizeof(uint64_t) + sizeof(int64_t);
    ok = ok && (size_t)length >= header && memcmp(contents, STATE_MAGIC, sizeof STATE_MAGIC) == 0;
    uint64_t rom_hash = 0;
    int64_t saved_at = 0;
    if (ok) {
        memcpy(&rom_hash, contents + sizeof STATE_MAGIC, sizeof rom_hash);
        memcpy(&saved_at, contents + sizeof STATE_MAGIC + sizeof rom_hash, sizeof saved_at);
        ok = rom_hash == m->rom_hash;
    }
    bool has_dram = false;
    uint32_t saved_card = 0;
    if (ok) {
        const uint8_t *cursor = contents + header, *end = contents + length;
        state_record_t record;
        uint32_t saved_dram = 0;
        saved_card = 0;
        while (next_record(&cursor, end, &record)) {
            if (!strcmp(record.name, "dram") && record.size >= DRAM_SIZE && record.size <= DRAM_MAX && !(record.size & (record.size - 1))) {
                has_dram = true;
                saved_dram = record.size;
            }
            if (!strcmp(record.name, "dram_card") && record.size <= CARD_DRAM_MAX && !(record.size & (record.size - 1))) saved_card = record.size;
        }
        ok = has_dram;
        if (ok && saved_dram != m->dram_size) {
            uint8_t *dram = calloc(1, saved_dram);
            if (!dram) ok = false;
            else {
                free(m->dram);
                m->dram = dram;
                m->dram_size = saved_dram;
            }
        }
    }
    if (ok) {
        FILE *image = m->card_socket.image;
        char current_path[sizeof m->card_path];
        memcpy(current_path, m->card_path, sizeof current_path);
        m->card_socket.image = NULL;
        uint32_t dram_next = m->dram_size_next, card_next = m->card_dram_size_next;
        m->dram_size_next = m->dram_size;
        m->card_dram_size_next = saved_card;
        machine_reset(m);
        m->dram_size_next = dram_next;
        m->card_dram_size_next = card_next;
        m->pccard.inserted = false;
        m->touch_legacy = true;
        const uint8_t *cursor = contents + header, *end = contents + length;
        state_record_t record;
        bool has_uart = false, has_sound = false;
        while (next_record(&cursor, end, &record)) {
            apply_record(m, &record);
            if (!strcmp(record.name, "uart_a_ctl1")) has_uart = true;
            if (!strcmp(record.name, "snd_size")) has_sound = true;
        }
        if (!has_uart || (m->uart_a.dma_length == 0 && m->regs[0x0BC / 4] != 0)) {
            m->uart_a.ctl1 = m->regs[0x0B0 / 4] & 0x0000FFEFu;
            m->uart_a.baud_divisor = m->regs[0x0B4 / 4] & 0x7FF;
            m->uart_a.dma_buffer = m->regs[0x0B8 / 4] & ~3u;
            m->uart_a.dma_length = m->regs[0x0BC / 4] ? (m->regs[0x0BC / 4] & 0xFFFF) + 1 : 0;
            m->uart_a.dma_armed = (m->uart_a.ctl1 & (1u << 15)) != 0;
            machine_logf(m, "state: serial port rebuilt from registers\n");
        }
        if (!has_sound || (m->snd_size == 0 && m->snd_tx_start == 0 && m->regs[0x068 / 4] != 0)) {
            m->snd_size = (m->regs[0x060 / 4] >> 18) & 0xFFF;
            m->snd_tx_start = m->regs[0x068 / 4] & ~3u;
            m->sib_dma_ctl = m->regs[0x090 / 4];
            machine_logf(m, "state: sound DMA rebuilt from registers\n");
        }
        m->cpu.last_fetch_valid = false;
        m->cpu.last_fetch_page = NULL;
        bind_card_socket(m, NULL);
        if (m->pccard.inserted && strcmp(current_path, m->card_path) != 0) {
            if (image) fclose(image);
            image = fopen(m->card_path, "r+b");
        } else if (!m->pccard.inserted && image) {
            fclose(image);
            image = NULL;
        }
        pccard_rebind(&m->card_socket, image);
        if (!image) m->card_path[0] = 0;
        bind_uart(m);
        m->uart_a.rx_next = NO_EVENT;
        intc_update(m);
        if (host_time) *host_time = saved_at;
    }
    free(contents);
    return ok;
}

void machine_advance_clock(machine_t *m, int64_t seconds) {
    if (seconds <= 0) return;
    m->rtc_base = (m->rtc_base + (uint64_t)seconds * 32768u) & 0xFFFFFFFFFFull;
    alarm_schedule(m);
}

void machine_reset(machine_t *m) {
    reset_machine(m, false);
}

static void reset_machine(machine_t *m, bool keep_ram) {
    mips_bus_t bus = m->cpu.bus;
    uint32_t watch[MIPS_WATCH_MAX];
    memcpy(watch, m->cpu.watch, sizeof watch);
    int watch_count = m->cpu.watch_count;
    void (*on_watch)(void *, uint32_t) = m->cpu.on_watch;
    uint64_t cycles = m->cpu.cycles;
    bool serial_connected = m->serial_connected, touch_legacy = m->touch_legacy;
    uint32_t serial_tag = m->serial_tag;
    FILE *pending_card = m->pending_card;
    char pending_card_path[sizeof m->pending_card_path];
    memcpy(pending_card_path, m->pending_card_path, sizeof pending_card_path);
    uint64_t card_insert_at = m->card_insert_at;
    uint32_t speed = m->cpu.speed;
    uint8_t *dram = m->dram, *rom = m->rom;
    uint32_t dram_size = m->dram_size, dram_size_next = m->dram_size_next;
    uint8_t *card_dram = m->card_dram;
    uint32_t card_size = m->card_dram_size, card_size_next = m->card_dram_size_next;
    uint32_t rom_size = m->rom_size, rom_pa = m->rom_pa, entry_va = m->entry_va;
    uint8_t *rom2 = m->rom2;
    uint32_t rom2_size = m->rom2_size, rom2_pa = m->rom2_pa;
    uint64_t rom_hash = m->rom_hash;
    machine_log_fn log = m->log;
    pccard_t card = m->pccard;
    FILE *image = m->card_socket.image;
    char card_path[sizeof m->card_path];
    memcpy(card_path, m->card_path, sizeof card_path);
    if (!keep_ram && pending_card) fclose(pending_card);
    if (!keep_ram && dram_size_next != dram_size) {
        uint8_t *resized = calloc(1, dram_size_next);
        if (resized) {
            free(dram);
            dram = resized;
            dram_size = dram_size_next;
        }
    }
    if (!keep_ram && card_size_next != card_size) {
        free(card_dram);
        card_dram = card_size_next ? calloc(1, card_size_next) : NULL;
        card_size = card_dram ? card_size_next : 0;
    }
    memset(m, 0, sizeof *m);
    if (!keep_ram) {
        memset(dram, 0, dram_size);
        if (card_dram) memset(card_dram, 0, card_size);
    }
    m->card_dram = card_dram;
    m->card_dram_size = card_size;
    m->card_dram_size_next = card_size_next;
    m->eeprom_scl = m->eeprom_sda = m->eeprom_sda_out = true;
    m->dram = dram;
    m->dram_size = dram_size;
    m->dram_size_next = dram_size_next;
    m->rom = rom;
    m->rom_size = rom_size;
    m->rom_pa = rom_pa;
    m->rom2 = rom2;
    m->rom2_size = rom2_size;
    m->rom2_pa = rom2_pa;
    m->entry_va = entry_va;
    m->rom_hash = rom_hash;
    m->log = log;
    m->cpu.bus = bus;
    memcpy(m->card_path, card_path, sizeof card_path);
    m->pccard.inserted = card.inserted;
    m->pccard.total_sectors = card.total_sectors;
    bind_card_socket(m, image);
    bind_uart(m);
    machine_power_on(m);
    m->cpu.speed = speed;
    memcpy(m->cpu.watch, watch, sizeof watch);
    m->cpu.watch_count = watch_count;
    m->cpu.on_watch = on_watch;
    if (keep_ram) {
        m->cpu.cycles = cycles;
        m->power_ctl &= ~POWER_COLDSTART;
        m->serial_connected = serial_connected;
        m->serial_tag = serial_tag;
        m->touch_legacy = touch_legacy;
        m->pending_card = pending_card;
        memcpy(m->pending_card_path, pending_card_path, sizeof pending_card_path);
        m->card_insert_at = card_insert_at;
    }
}

void machine_set_memory(machine_t *m, uint32_t megabytes) {
    uint32_t bank0, card;
    switch (megabytes) {
        case 4: bank0 = 4; card = 0; break;
        case 8: bank0 = 8; card = 0; break;
        case 16: bank0 = 16; card = 0; break;
        case 20: bank0 = 4; card = 16; break;
        case 32: bank0 = 16; card = 16; break;
        default: return;
    }
    m->dram_size_next = bank0 << 20;
    m->card_dram_size_next = card << 20;
    if (m->cpu.cycles == 0 && (m->dram_size_next != m->dram_size || m->card_dram_size_next != m->card_dram_size)) machine_reset(m);
}

uint32_t machine_memory(machine_t *m) { return (m->dram_size + m->card_dram_size) >> 20; }
uint32_t machine_memory_next(machine_t *m) { return (m->dram_size_next + m->card_dram_size_next) >> 20; }

void machine_set_speed(machine_t *m, uint32_t multiplier) {
    m->cpu.speed = multiplier < 1 ? 1 : multiplier > 16 ? 16 : multiplier;
}

uint32_t machine_speed(machine_t *m) { return m->cpu.speed ? m->cpu.speed : 1; }

size_t machine_audio(machine_t *m, int16_t *samples, size_t max, uint32_t *rate) {
    size_t count = m->audio_count < max ? m->audio_count : max;
    for (size_t i = 0; i < count; i++) samples[i] = m->audio[(m->audio_head + i) % AUDIO_RING];
    m->audio_head = (uint32_t)((m->audio_head + count) % AUDIO_RING);
    m->audio_count -= (uint32_t)count;
    *rate = m->audio_rate;
    return count;
}

static void cancel_pending_card(machine_t *m) {
    if (m->pending_card) fclose(m->pending_card);
    m->pending_card = NULL;
}

bool machine_insert_card(machine_t *m, const char *path) {
    FILE *image = fopen(path, "r+b");
    if (!image) return false;
    cancel_pending_card(m);
    if (m->pccard.inserted) {
        machine_eject_card(m);
        m->pending_card = image;
        snprintf(m->pending_card_path, sizeof m->pending_card_path, "%s", path);
        m->card_insert_at = m->cpu.cycles + CARD_SWAP_CYCLES;
        return true;
    }
    snprintf(m->card_path, sizeof m->card_path, "%s", path);
    return pccard_insert(&m->card_socket, image);
}

void machine_eject_card(machine_t *m) {
    cancel_pending_card(m);
    pccard_eject(&m->card_socket);
    m->card_path[0] = 0;
}

const char *machine_card_path(machine_t *m) {
    return m->pccard.inserted ? m->card_path : NULL;
}

bool machine_card_inserted(machine_t *m) {
    return m->pccard.inserted;
}

void machine_serial_connect(machine_t *m, bool connected) {
    if (m->serial_connected == connected) return;
    m->serial_connected = connected;
    intc_set_pending(m, STATUS5_SET, connected ? STATUS5_IONEG_DCD : STATUS5_IOPOS_DCD);
    intc_set_pending(m, connected ? 3 : 2, MFIO_SERIAL_CTS);
}

bool machine_serial_connected(machine_t *m) { return m->serial_connected; }

void machine_set_serial_tag(machine_t *m, uint32_t tag) { m->serial_tag = tag; }
uint32_t machine_serial_tag(machine_t *m) { return m->serial_tag; }

void machine_serial_send(machine_t *m, const uint8_t *data, size_t length) {
    uart_receive(&m->uart_port, data, (uint32_t)length, m->cpu.cycles);
}

size_t machine_serial_take(machine_t *m, uint8_t *out, size_t max) {
    return uart_take_tx(&m->uart_port, out, (uint32_t)max);
}

uint32_t machine_serial_baud(machine_t *m) { return uart_baud(&m->uart_port); }

static void report_watch(void *context, uint32_t pc) {
    machine_t *m = context;
    mips_cpu_t *cpu = &m->cpu;
    machine_logf(m, "watch t=%.3f pc=%08X ra=%08X v0=%08X a0=%08X a1=%08X a2=%08X a3=%08X\n", (double)cpu->cycles / MACHINE_CLOCK_HZ, pc,
                 cpu->gpr[31], cpu->gpr[2], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6], cpu->gpr[7]);
}

bool machine_watch_pc(machine_t *m, uint32_t va) {
    if (m->cpu.watch_count == MIPS_WATCH_MAX) return false;
    m->cpu.watch[m->cpu.watch_count++] = va;
    m->cpu.on_watch = report_watch;
    return true;
}
