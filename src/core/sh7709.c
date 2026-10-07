#include "core/sh7709.h"

#include <string.h>

#define SCI_BASE    0xFFFFFE80u
#define TMU_BASE    0xFFFFFE90u
#define RTC_BASE    0xFFFFFEC0u
#define INTC_BASE   0xFFFFFEE0u
#define ICR0_NMIL   0x8000u
#define STBCR_STBY  0x80u
#define ICR1_FALLING 0u
#define ICR1_RISING  1u
#define ICR1_LEVEL   2u
#define PCC_NO_CARD 0x0Cu
#define BSC_BASE    0xFFFFFF60u
#define CPG_BASE    0xFFFFFF80u
#define CCR_ADDRESS 0xFFFFFFECu
#define AREA1_BASE  0x04000000u
#define AREA1_SIZE  0x00000200u

#define TCR_UNF  0x0100u
#define TCR_UNIE 0x0020u
#define TCR_TPSC 0x0007u

#define RCR1_CF  0x80u
#define RCR1_CIE 0x10u
#define RCR1_AIE 0x08u
#define RCR1_AF  0x01u
#define RCR2_PEF 0x80u
#define RCR2_PES 0x70u
#define RCR2_RTCEN 0x08u
#define RCR2_RESET 0x02u
#define RCR2_START 0x01u

#define SCI_TDRE 0x80u
#define SCI_RDRF 0x40u
#define SCI_ORER 0x20u
#define SCI_FER  0x10u
#define SCI_PER  0x08u
#define SCI_TEND 0x04u
#define SCR_TIE  0x80u
#define SCR_RIE  0x40u
#define SCR_TEIE 0x04u

#define SCIF_ER   0x80u
#define SCIF_TEND 0x40u
#define SCIF_TDFE 0x20u
#define SCIF_BRK  0x10u
#define SCIF_RDF  0x02u
#define SCIF_DR   0x01u
#define SCIF_FIFO 16u
#define SMR_CKS   0x03u
#define SCIF_SIZE 0x10u
#define SCIF_ALIAS_CODE 0x700u

#define ADCSR_ADF   0x80u
#define ADCSR_ADIE  0x40u
#define ADCSR_ADST  0x20u
#define ADCSR_MULTI 0x10u
#define ADCSR_CH    0x07u
#define ADC_CHANNEL_HZ 50000u

#define PORT_A_DATA 0x10u
#define PORT_B_DATA 0x11u

#define RTC_TICK_HZ 64u
#define RTC_OUTPUT_HZ 16384u

enum {
    RTC_SECOND, RTC_MINUTE, RTC_HOUR, RTC_WEEKDAY, RTC_DAY, RTC_MONTH, RTC_YEAR,
};

void sh7709_init(sh7709_t *chip, sh3_cpu_t *cpu, sh7709_variant_t variant, uint32_t cpu_hz, uint32_t peripheral_hz) {
    memset(chip, 0, sizeof *chip);
    chip->cpu = cpu;
    chip->variant = variant;
    chip->cpu_hz = cpu_hz;
    chip->peripheral_hz = peripheral_hz;
    chip->rtc_control2 = RCR2_RTCEN | RCR2_START;
    sh7709_reset(chip);
}

void sh7709_reset(sh7709_t *chip) {
    uint8_t counter[7], alarm[6];
    memcpy(counter, chip->rtc_counter, sizeof counter);
    memcpy(alarm, chip->rtc_alarm, sizeof alarm);
    uint8_t control2 = chip->rtc_control2;
    sh3_cpu_t *cpu = chip->cpu;
    sh7709_variant_t variant = chip->variant;
    uint32_t cpu_hz = chip->cpu_hz, peripheral_hz = chip->peripheral_hz;
    sh7709_transmit_fn transmit = chip->transmit;
    void *transmit_context = chip->transmit_context;
    sh7709_ports_fn ports_written = chip->ports_written;
    uint16_t adc_input[4], port_input_mask[64], port_input[64];
    uint32_t irq_active_high = chip->irq_active_high;
    memcpy(adc_input, chip->adc_input, sizeof adc_input);
    memcpy(port_input_mask, chip->port_input_mask, sizeof port_input_mask);
    memcpy(port_input, chip->port_input, sizeof port_input);
    memset(chip, 0, sizeof *chip);
    memcpy(chip->adc_input, adc_input, sizeof adc_input);
    memcpy(chip->port_input_mask, port_input_mask, sizeof port_input_mask);
    memcpy(chip->port_input, port_input, sizeof port_input);
    chip->irq_active_high = irq_active_high;
    chip->cpu = cpu;
    chip->variant = variant;
    chip->cpu_hz = cpu_hz;
    chip->peripheral_hz = peripheral_hz;
    chip->transmit = transmit;
    chip->transmit_context = transmit_context;
    chip->ports_written = ports_written;
    memcpy(chip->rtc_counter, counter, sizeof counter);
    memcpy(chip->rtc_alarm, alarm, sizeof alarm);
    chip->rtc_control2 = control2 & (RCR2_START | RCR2_PES | RCR2_RTCEN);
    for (int i = 0; i < 3; i++) {
        chip->timer[i].constant = 0xFFFFFFFFu;
        chip->timer[i].count = 0xFFFFFFFFu;
    }
    chip->sci.status = SCI_TDRE | SCI_TEND;
    chip->sci.bit_rate = 0xFF;
    for (int i = 0; i < 2; i++) {
        chip->scif[i].status = SCIF_TEND | SCIF_TDFE;
        chip->scif[i].bit_rate = 0xFF;
    }
    chip->last_cycle = cpu ? cpu->cycles : 0;
}

static uint32_t timer_hz(const sh7709_t *chip, const sh7709_timer_t *timer) {
    switch (timer->control & TCR_TPSC) {
        case 0: return chip->peripheral_hz / 4;
        case 1: return chip->peripheral_hz / 16;
        case 2: return chip->peripheral_hz / 64;
        case 3: return chip->peripheral_hz / 256;
        case 4: return RTC_OUTPUT_HZ;
        default: return 0;
    }
}

static void advance_timer(sh7709_t *chip, int index, uint64_t elapsed) {
    sh7709_timer_t *timer = &chip->timer[index];
    if (!(chip->timer_start & (1u << index))) return;
    uint32_t hz = timer_hz(chip, timer);
    if (!hz) return;
    timer->remainder += elapsed * hz;
    uint64_t ticks = timer->remainder / chip->cpu_hz;
    timer->remainder %= chip->cpu_hz;
    if (ticks <= timer->count) {
        timer->count -= (uint32_t)ticks;
        return;
    }
    ticks -= (uint64_t)timer->count + 1;
    uint64_t period = (uint64_t)timer->constant + 1;
    timer->count = timer->constant - (uint32_t)(ticks % period);
    timer->control |= TCR_UNF;
}

static uint8_t bcd_increment(uint8_t value) {
    value++;
    if ((value & 0x0F) > 9) value = (uint8_t)((value & 0xF0) + 0x10);
    return value;
}

static int bcd_value(uint8_t value) {
    return (value >> 4) * 10 + (value & 0x0F);
}

static uint8_t to_bcd(int value) {
    return (uint8_t)(((value / 10) << 4) | (value % 10));
}

static int days_in_month(int month, int year) {
    static const int days[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (month < 1 || month > 12) return 31;
    if (month == 2 && year % 4 == 0) return 29;
    return days[month - 1];
}

static void check_alarm(sh7709_t *chip) {
    static const int fields[] = { RTC_SECOND, RTC_MINUTE, RTC_HOUR, RTC_WEEKDAY, RTC_DAY, RTC_MONTH };
    bool any = false;
    for (int i = 0; i < 6; i++) {
        uint8_t alarm = chip->rtc_alarm[i];
        if (!(alarm & 0x80)) continue;
        any = true;
        if ((alarm & 0x7F) != chip->rtc_counter[fields[i]]) return;
    }
    if (any) chip->rtc_control1 |= RCR1_AF;
}

static void rtc_second(sh7709_t *chip) {
    uint8_t *counter = chip->rtc_counter;
    chip->rtc_control1 |= RCR1_CF;
    counter[RTC_SECOND] = bcd_increment(counter[RTC_SECOND]);
    if (counter[RTC_SECOND] >= 0x60) {
        counter[RTC_SECOND] = 0;
        counter[RTC_MINUTE] = bcd_increment(counter[RTC_MINUTE]);
        if (counter[RTC_MINUTE] >= 0x60) {
            counter[RTC_MINUTE] = 0;
            counter[RTC_HOUR] = bcd_increment(counter[RTC_HOUR]);
            if (counter[RTC_HOUR] >= 0x24) {
                counter[RTC_HOUR] = 0;
                counter[RTC_WEEKDAY] = (uint8_t)((counter[RTC_WEEKDAY] + 1) % 7);
                int month = bcd_value(counter[RTC_MONTH]);
                if (bcd_value(counter[RTC_DAY]) >= days_in_month(month, bcd_value(counter[RTC_YEAR]))) {
                    counter[RTC_DAY] = 1;
                    counter[RTC_MONTH] = bcd_increment(counter[RTC_MONTH]);
                    if (counter[RTC_MONTH] > 0x12) {
                        counter[RTC_MONTH] = 1;
                        counter[RTC_YEAR] = bcd_increment(counter[RTC_YEAR]);
                        if (counter[RTC_YEAR] >= 0xA0) counter[RTC_YEAR] = 0;
                    }
                } else {
                    counter[RTC_DAY] = bcd_increment(counter[RTC_DAY]);
                }
            }
        }
    }
    check_alarm(chip);
}

static uint32_t periodic_ticks(const sh7709_t *chip) {
    switch ((chip->rtc_control2 & RCR2_PES) >> 4) {
        case 1: case 2: return 1;
        case 3: return 4;
        case 4: return 16;
        case 5: return 32;
        case 6: return 64;
        case 7: return 128;
        default: return 0;
    }
}

static void advance_rtc(sh7709_t *chip, uint64_t elapsed) {
    if (!(chip->rtc_control2 & RCR2_START)) return;
    chip->rtc_remainder += elapsed * RTC_TICK_HZ;
    uint64_t ticks = chip->rtc_remainder / chip->cpu_hz;
    chip->rtc_remainder %= chip->cpu_hz;
    uint32_t period = periodic_ticks(chip);
    while (ticks--) {
        chip->rtc_64hz = (uint8_t)((chip->rtc_64hz + 1) & 0x7F);
        if (period && (chip->rtc_64hz % period) == 0) chip->rtc_control2 |= RCR2_PEF;
        if ((chip->rtc_64hz & 0x3F) == 0) rtc_second(chip);
    }
}

static void advance_adc(sh7709_t *chip, uint64_t now) {
    if (!(chip->adc_control & ADCSR_ADST) || now < chip->adc_done) return;
    uint32_t last = chip->adc_control & ADCSR_CH & 3u;
    uint32_t first = (chip->adc_control & ADCSR_MULTI) ? 0 : last;
    for (uint32_t channel = first; channel <= last; channel++) chip->adc_data[channel] = chip->adc_input[channel];
    chip->adc_control = (uint8_t)((chip->adc_control & ~ADCSR_ADST) | ADCSR_ADF);
}

static void start_adc(sh7709_t *chip) {
    uint32_t last = chip->adc_control & ADCSR_CH & 3u;
    uint32_t channels = (chip->adc_control & ADCSR_MULTI) ? last + 1 : 1;
    chip->adc_done = chip->cpu->cycles + (uint64_t)channels * (chip->cpu_hz / ADC_CHANNEL_HZ);
}

static uint16_t port_value(const sh7709_t *chip, uint32_t index) {
    return (uint16_t)((chip->ports[index] & ~chip->port_input_mask[index]) | (chip->port_input[index] & chip->port_input_mask[index]));
}

static uint16_t pint_pending(const sh7709_t *chip) {
    uint16_t pins = (uint16_t)((port_value(chip, PORT_A_DATA) & 0xFFu) | (port_value(chip, PORT_B_DATA) & 0xFFu) << 8);
    return (uint16_t)(chip->pinter & ~(pins ^ chip->icr2));
}

void sh7709_set_port_input(sh7709_t *chip, uint32_t pa, uint16_t mask, uint16_t value) {
    uint32_t offset = (pa & 0x1FFFFFFFu) - AREA1_BASE;
    if (offset < 0x100 || offset >= 0x140) return;
    chip->port_input_mask[(offset - 0x100) / 2] = mask;
    chip->port_input[(offset - 0x100) / 2] = value;
    sh7709_update_interrupts(chip);
}

void sh7709_set_adc(sh7709_t *chip, int channel, uint16_t value) {
    if (channel < 0 || channel > 3) return;
    chip->adc_input[channel] = value & 0x3FFu;
}

void sh7709_advance(sh7709_t *chip) {
    uint64_t now = chip->cpu->cycles;
    if (now > chip->last_cycle) {
        uint64_t elapsed = now - chip->last_cycle;
        for (int i = 0; i < 3; i++) advance_timer(chip, i, elapsed);
        advance_rtc(chip, elapsed);
    }
    advance_adc(chip, now);
    chip->last_cycle = now;
    sh7709_update_interrupts(chip);
}

uint64_t sh7709_next_event(sh7709_t *chip) {
    uint64_t now = chip->last_cycle, next = UINT64_MAX;
    for (int i = 0; i < 3; i++) {
        const sh7709_timer_t *timer = &chip->timer[i];
        if (!(chip->timer_start & (1u << i)) || !(timer->control & TCR_UNIE)) continue;
        uint32_t hz = timer_hz(chip, timer);
        if (!hz) continue;
        uint64_t needed = ((uint64_t)timer->count + 1) * chip->cpu_hz - timer->remainder;
        uint64_t at = now + (needed + hz - 1) / hz;
        if (at < next) next = at;
    }
    bool rtc_interrupts = (chip->rtc_control1 & (RCR1_AIE | RCR1_CIE)) || (chip->rtc_control2 & RCR2_PES);
    if ((chip->rtc_control2 & RCR2_START) && rtc_interrupts) {
        uint64_t at = now + (chip->cpu_hz - chip->rtc_remainder + RTC_TICK_HZ - 1) / RTC_TICK_HZ;
        if (at < next) next = at;
    }
    if ((chip->adc_control & ADCSR_ADST) && chip->adc_done < next) next = chip->adc_done;
    return next;
}

typedef struct {
    uint32_t level;
    uint32_t code;
    uint32_t source;
} candidate_t;

static void consider(candidate_t *best, uint32_t level, uint32_t code, bool pending) {
    if (pending && level > best->level) {
        best->level = level;
        best->code = code;
        best->source = 0;
    }
}

static void consider_leveled(candidate_t *best, uint32_t level, uint32_t source, bool pending) {
    if (pending && level > best->level) {
        best->level = level;
        best->code = 0x200 + (15 - level) * 0x20;
        best->source = source;
    }
}

static bool sci_pending(const sh7709_serial_t *sci, uint32_t *code_offset) {
    if ((sci->status & (SCI_ORER | SCI_FER | SCI_PER)) && (sci->control & SCR_RIE)) { *code_offset = 0x00; return true; }
    if ((sci->status & SCI_RDRF) && (sci->control & SCR_RIE)) { *code_offset = 0x20; return true; }
    if ((sci->status & SCI_TDRE) && (sci->control & SCR_TIE)) { *code_offset = 0x40; return true; }
    if ((sci->status & SCI_TEND) && (sci->control & SCR_TEIE)) { *code_offset = 0x60; return true; }
    return false;
}

static bool scif_pending(const sh7709_serial_t *scif, uint32_t *code_offset) {
    if ((scif->status & (SCIF_ER | SCIF_BRK)) && (scif->control & SCR_RIE)) { *code_offset = 0x00; return true; }
    if ((scif->status & (SCIF_RDF | SCIF_DR)) && (scif->control & SCR_RIE)) { *code_offset = 0x20; return true; }
    if ((scif->status & SCIF_TDFE) && (scif->control & SCR_TIE)) { *code_offset = 0x60; return true; }
    return false;
}

void sh7709_update_interrupts(sh7709_t *chip) {
    candidate_t best = { 0, 0, 0 };
    uint16_t ipra = chip->priority[0], iprb = chip->priority[1];
    if (chip->nmi) consider(&best, 16, 0x1C0, true);
    if (chip->irl_level) consider(&best, chip->irl_level, 0x200 + (15 - chip->irl_level) * 0x20, true);
    consider(&best, chip->extra_level, chip->extra_code, chip->extra_level != 0);
    for (int i = 0; i < 3; i++) {
        uint16_t control = chip->timer[i].control;
        consider(&best, (ipra >> (12 - i * 4)) & 15, 0x400 + (uint32_t)i * 0x20, (control & TCR_UNF) && (control & TCR_UNIE));
    }
    uint32_t rtc_level = ipra & 15;
    consider(&best, rtc_level, 0x480, (chip->rtc_control1 & RCR1_AF) && (chip->rtc_control1 & RCR1_AIE));
    consider(&best, rtc_level, 0x4A0, (chip->rtc_control2 & RCR2_PEF) && (chip->rtc_control2 & RCR2_PES));
    consider(&best, rtc_level, 0x4C0, (chip->rtc_control1 & RCR1_CF) && (chip->rtc_control1 & RCR1_CIE));
    uint32_t offset;
    if (sci_pending(&chip->sci, &offset)) consider(&best, (iprb >> 4) & 15, 0x4E0 + offset, true);
    if (chip->scif_alias && scif_pending(&chip->scif[1], &offset)) consider(&best, chip->scif_alias_priority, SCIF_ALIAS_CODE + offset, true);
    if (chip->variant == SH7709) {
        uint16_t iprc = chip->priority[2], iprd = chip->priority[3], ipre = chip->priority[4];
        for (int line = 0; line < 6; line++) {
            uint32_t level = line < 4 ? (iprc >> (line * 4)) & 15 : (iprd >> ((line - 4) * 4)) & 15;
            consider_leveled(&best, level, 0x600 + (uint32_t)line * 0x20, (chip->irr0 >> line) & 1);
        }
        uint16_t pint = pint_pending(chip);
        consider_leveled(&best, (iprd >> 12) & 15, 0x700, (pint & 0x00FFu) != 0);
        consider_leveled(&best, (iprd >> 8) & 15, 0x720, (pint & 0xFF00u) != 0);
        if (scif_pending(&chip->scif[0], &offset)) consider_leveled(&best, (ipre >> 8) & 15, 0x880 + offset, true);
        if (scif_pending(&chip->scif[1], &offset)) consider_leveled(&best, (ipre >> 4) & 15, 0x900 + offset, true);
        consider_leveled(&best, ipre & 15, 0x980, (chip->adc_control & ADCSR_ADF) && (chip->adc_control & ADCSR_ADIE));
    }
    sh3_set_interrupt(chip->cpu, best.level, best.code);
    chip->cpu->interrupt_source = best.source;
    chip->cpu->standby_wakes_blocked = chip->variant == SH7709 && (chip->stbcr & STBCR_STBY);
}

void sh7709_set_irl(sh7709_t *chip, uint32_t level) {
    chip->irl_level = level & 15;
    sh7709_update_interrupts(chip);
}

void sh7709_set_extra(sh7709_t *chip, uint32_t level, uint32_t code) {
    chip->extra_level = level & 15;
    chip->extra_code = code;
    sh7709_update_interrupts(chip);
}

void sh7709_set_irq(sh7709_t *chip, int line, bool asserted) {
    if (line < 0 || line > 5) return;
    uint32_t bit = 1u << line;
    uint32_t sense = (chip->icr1 >> (line * 2)) & 3;
    bool changed = ((chip->irq_lines & bit) != 0) != asserted;
    bool pin_low = asserted != ((chip->irq_active_high & bit) != 0);
    if (asserted) chip->irq_lines |= bit;
    else chip->irq_lines &= ~bit;
    if (sense == ICR1_LEVEL) {
        if (pin_low) chip->irr0 |= (uint8_t)bit;
        else chip->irr0 &= (uint8_t)~bit;
    } else if (changed && pin_low == (sense == ICR1_FALLING)) {
        chip->irr0 |= (uint8_t)bit;
    }
    sh7709_update_interrupts(chip);
}

void sh7709_set_irq_active_high(sh7709_t *chip, uint32_t lines) {
    chip->irq_active_high = lines;
}

static void serial_push(sh7709_serial_t *serial, uint8_t byte) {
    if (serial->received_count >= sizeof serial->received) return;
    serial->received[(serial->received_head + serial->received_count) % sizeof serial->received] = byte;
    serial->received_count++;
}

static uint8_t serial_pop(sh7709_serial_t *serial) {
    if (!serial->received_count) return 0;
    uint8_t byte = serial->received[serial->received_head];
    serial->received_head = (serial->received_head + 1) % sizeof serial->received;
    serial->received_count--;
    return byte;
}

void sh7709_receive(sh7709_t *chip, int port, uint8_t byte) {
    if (port == 0) {
        sh7709_serial_t *sci = &chip->sci;
        if (sci->status & SCI_RDRF) {
            serial_push(sci, byte);
        } else {
            sci->receive = byte;
            sci->status |= SCI_RDRF;
        }
    } else if (port <= 2) {
        sh7709_serial_t *scif = &chip->scif[port - 1];
        serial_push(scif, byte);
        scif->status |= SCIF_RDF | SCIF_DR;
    }
    sh7709_update_interrupts(chip);
}

static const sh7709_serial_t *serial_port(const sh7709_t *chip, int port) {
    if (port == 0) return &chip->sci;
    if (port <= 2) return &chip->scif[port - 1];
    return NULL;
}

void sh7709_set_scif_alias(sh7709_t *chip, uint32_t pa, uint32_t priority) {
    if (chip->scif_alias == pa && chip->scif_alias_priority == priority) return;
    chip->scif_alias = pa;
    chip->scif_alias_priority = priority;
    sh7709_update_interrupts(chip);
}

size_t sh7709_receive_room(const sh7709_t *chip, int port) {
    const sh7709_serial_t *serial = serial_port(chip, port);
    if (!serial) return 0;
    if (port == 0) return (serial->status & SCI_RDRF) ? 0 : 1;
    return serial->received_count < SCIF_FIFO ? SCIF_FIFO - serial->received_count : 0;
}

uint32_t sh7709_baud(const sh7709_t *chip, int port) {
    const sh7709_serial_t *serial = serial_port(chip, port);
    if (!serial) return 0;
    uint32_t divider = (port == 0 ? 64u : 32u) << (2 * (serial->mode & SMR_CKS));
    return chip->peripheral_hz / (divider * ((uint32_t)serial->bit_rate + 1));
}

void sh7709_set_time(sh7709_t *chip, int year, int month, int day, int weekday, int hour, int minute, int second) {
    chip->rtc_counter[RTC_SECOND] = to_bcd(second);
    chip->rtc_counter[RTC_MINUTE] = to_bcd(minute);
    chip->rtc_counter[RTC_HOUR] = to_bcd(hour);
    chip->rtc_counter[RTC_WEEKDAY] = (uint8_t)weekday;
    chip->rtc_counter[RTC_DAY] = to_bcd(day);
    chip->rtc_counter[RTC_MONTH] = to_bcd(month);
    chip->rtc_counter[RTC_YEAR] = to_bcd(year % 100);
    chip->rtc_control2 |= RCR2_START;
}

void sh7709_add_seconds(sh7709_t *chip, uint32_t seconds) {
    while (seconds--) rtc_second(chip);
    sh7709_update_interrupts(chip);
}

uint32_t sh7709_timer_count(sh7709_t *chip, int index) {
    sh7709_advance(chip);
    return chip->timer[index & 3].count;
}

static uint32_t sci_read(sh7709_serial_t *sci, uint32_t offset) {
    switch (offset) {
        case 0x0: return sci->mode;
        case 0x2: return sci->bit_rate;
        case 0x4: return sci->control;
        case 0x6: return sci->transmit;
        case 0x8: return sci->status;
        case 0xA: return sci->receive;
        default: return 0;
    }
}

static void sci_write(sh7709_t *chip, uint32_t offset, uint32_t value) {
    sh7709_serial_t *sci = &chip->sci;
    switch (offset) {
        case 0x0: sci->mode = (uint8_t)value; break;
        case 0x2: sci->bit_rate = (uint8_t)value; break;
        case 0x4: sci->control = (uint8_t)value; break;
        case 0x6: sci->transmit = (uint8_t)value; break;
        case 0x8: {
            uint8_t cleared = (uint8_t)(sci->status & ~value & (SCI_TDRE | SCI_RDRF | SCI_ORER | SCI_FER | SCI_PER));
            sci->status &= (uint8_t)~cleared;
            if (cleared & SCI_TDRE) {
                if (chip->transmit) chip->transmit(chip->transmit_context, 0, sci->transmit);
                sci->status |= SCI_TDRE | SCI_TEND;
            }
            if ((cleared & SCI_RDRF) && sci->received_count) {
                sci->receive = serial_pop(sci);
                sci->status |= SCI_RDRF;
            }
            break;
        }
        default: break;
    }
}

static uint32_t scif_read(sh7709_serial_t *scif, uint32_t offset) {
    switch (offset) {
        case 0x0: return scif->mode;
        case 0x2: return scif->bit_rate;
        case 0x4: return scif->control;
        case 0x8: return scif->status;
        case 0xA: {
            uint8_t byte = serial_pop(scif);
            if (!scif->received_count) scif->status &= (uint8_t)~(SCIF_RDF | SCIF_DR);
            return byte;
        }
        case 0xE: return scif->received_count & 0x1F;
        default: return 0;
    }
}

static void scif_write(sh7709_t *chip, int port, uint32_t offset, uint32_t value) {
    sh7709_serial_t *scif = &chip->scif[port];
    switch (offset) {
        case 0x0: scif->mode = (uint8_t)value; break;
        case 0x2: scif->bit_rate = (uint8_t)value; break;
        case 0x4: scif->control = (uint8_t)value; break;
        case 0x6:
            if (chip->transmit) chip->transmit(chip->transmit_context, port + 1, (uint8_t)value);
            scif->status |= SCIF_TEND | SCIF_TDFE;
            break;
        case 0x8:
            scif->status &= (uint8_t)(value | SCIF_TEND | SCIF_TDFE);
            if (scif->received_count) scif->status |= SCIF_RDF | SCIF_DR;
            break;
        case 0xC:
            if (value & 2) { scif->received_count = 0; scif->status &= (uint8_t)~(SCIF_RDF | SCIF_DR); }
            break;
        default: break;
    }
}

static uint32_t tmu_read(sh7709_t *chip, uint32_t offset) {
    if (offset == 0x0) return chip->timer_output;
    if (offset == 0x2) return chip->timer_start;
    if (offset == 0x28) return chip->timer_capture;
    if (offset < 0x4 || offset > 0x24) return 0;
    int index = (int)(offset - 0x4) / 0xC;
    sh7709_timer_t *timer = &chip->timer[index];
    switch ((offset - 0x4) % 0xC) {
        case 0x0: return timer->constant;
        case 0x4: sh7709_advance(chip); return timer->count;
        case 0x8: sh7709_advance(chip); return timer->control;
        default: return 0;
    }
}

static void tmu_write(sh7709_t *chip, uint32_t offset, uint32_t value) {
    sh7709_advance(chip);
    if (offset == 0x0) { chip->timer_output = (uint8_t)value; return; }
    if (offset == 0x2) {
        for (int i = 0; i < 3; i++)
            if ((value & (1u << i)) && !(chip->timer_start & (1u << i))) chip->timer[i].remainder = 0;
        chip->timer_start = (uint8_t)(value & 7);
        return;
    }
    if (offset < 0x4 || offset > 0x24) return;
    int index = (int)(offset - 0x4) / 0xC;
    sh7709_timer_t *timer = &chip->timer[index];
    switch ((offset - 0x4) % 0xC) {
        case 0x0: timer->constant = value; break;
        case 0x4: timer->count = value; break;
        case 0x8: timer->control = (uint16_t)((value & ~TCR_UNF) | (timer->control & value & TCR_UNF)); break;
        default: break;
    }
}

static uint32_t rtc_read(sh7709_t *chip, uint32_t offset) {
    if (offset == 0x00) { sh7709_advance(chip); return chip->rtc_64hz; }
    if (offset >= 0x02 && offset <= 0x0E) { sh7709_advance(chip); return chip->rtc_counter[(offset - 0x02) / 2]; }
    if (offset >= 0x10 && offset <= 0x1A) return chip->rtc_alarm[(offset - 0x10) / 2];
    if (offset == 0x1C) return chip->rtc_control1;
    if (offset == 0x1E) return chip->rtc_control2;
    return 0;
}

static void rtc_write(sh7709_t *chip, uint32_t offset, uint32_t value) {
    sh7709_advance(chip);
    if (offset >= 0x02 && offset <= 0x0E) { chip->rtc_counter[(offset - 0x02) / 2] = (uint8_t)value; return; }
    if (offset >= 0x10 && offset <= 0x1A) { chip->rtc_alarm[(offset - 0x10) / 2] = (uint8_t)value; return; }
    if (offset == 0x1C) {
        uint8_t flags = RCR1_CF | RCR1_AF;
        chip->rtc_control1 = (uint8_t)((value & ~flags) | (chip->rtc_control1 & value & flags));
        return;
    }
    if (offset == 0x1E) {
        if (value & RCR2_RESET) {
            chip->rtc_64hz = 0;
            chip->rtc_remainder = 0;
        }
        chip->rtc_control2 = (uint8_t)((value & ~(RCR2_PEF | RCR2_RESET)) | (chip->rtc_control2 & value & RCR2_PEF));
    }
}

static bool area1_read(sh7709_t *chip, uint32_t offset, uint32_t *value) {
    switch (offset) {
        case 0x000: *value = chip->cpu->intevt2; return true;
        case 0x004: *value = chip->irr0; return true;
        case 0x006: *value = chip->irr1; return true;
        case 0x008: *value = chip->irr2; return true;
        case 0x010: *value = chip->icr1; return true;
        case 0x012: *value = chip->icr2; return true;
        case 0x014: *value = chip->pinter; return true;
        case 0x016: *value = chip->priority[2]; return true;
        case 0x018: *value = chip->priority[3]; return true;
        case 0x01A: *value = chip->priority[4]; return true;
        case 0x0B0: *value = chip->ccr2; return true;
        case 0x0E0: case 0x0F0: *value = PCC_NO_CARD; return true;
        default: break;
    }
    if (offset >= 0x080 && offset < 0x090) {
        uint16_t sample = chip->adc_data[(offset - 0x080) / 4];
        *value = (offset & 2) ? (uint32_t)(sample & 3) << 6 : (uint32_t)(sample >> 2);
        return true;
    }
    if (offset == 0x090) {
        advance_adc(chip, chip->cpu->cycles);
        *value = chip->adc_control;
        return true;
    }
    if (offset == 0x092) { *value = chip->adc_config; return true; }
    if (offset >= 0x0E0 && offset < 0x100) { *value = chip->pcc[(offset - 0x0E0) / 2]; return true; }
    if (offset >= 0x100 && offset < 0x140) {
        *value = port_value(chip, (offset - 0x100) / 2);
        return true;
    }
    if (offset >= 0x140 && offset < 0x150) { *value = scif_read(&chip->scif[0], offset - 0x140); return true; }
    if (offset >= 0x150 && offset < 0x160) { *value = scif_read(&chip->scif[1], offset - 0x150); return true; }
    *value = 0;
    return true;
}

static uint32_t level_sensed(const sh7709_t *chip) {
    uint32_t lines = 0;
    for (int line = 0; line < 6; line++) {
        if (((chip->icr1 >> (line * 2)) & 3) >= ICR1_LEVEL) lines |= 1u << line;
    }
    return lines;
}

static bool area1_write(sh7709_t *chip, uint32_t offset, uint32_t value) {
    switch (offset) {
        case 0x004: chip->irr0 &= (uint8_t)(value | ((chip->irq_lines ^ chip->irq_active_high) & level_sensed(chip))); return true;
        case 0x006: chip->irr1 &= (uint8_t)value; return true;
        case 0x008: chip->irr2 &= (uint8_t)value; return true;
        case 0x010: chip->icr1 = (uint16_t)value; return true;
        case 0x012: chip->icr2 = (uint16_t)value; return true;
        case 0x014: chip->pinter = (uint16_t)value; return true;
        case 0x016: chip->priority[2] = (uint16_t)value; return true;
        case 0x018: chip->priority[3] = (uint16_t)value; return true;
        case 0x01A: chip->priority[4] = (uint16_t)value; return true;
        case 0x0B0: chip->ccr2 = value; return true;
        default: break;
    }
    if (offset == 0x090) {
        uint8_t flag = chip->adc_control & ADCSR_ADF & (uint8_t)value;
        chip->adc_control = (uint8_t)((value & ~ADCSR_ADF) | flag);
        if (value & ADCSR_ADST) start_adc(chip);
        sh7709_update_interrupts(chip);
        return true;
    }
    if (offset == 0x092) { chip->adc_config = (uint8_t)value; return true; }
    if (offset >= 0x0E0 && offset < 0x100) { chip->pcc[(offset - 0x0E0) / 2] = (uint8_t)value; return true; }
    if (offset >= 0x100 && offset < 0x140) {
        chip->ports[(offset - 0x100) / 2] = (uint16_t)value;
        if (chip->ports_written) chip->ports_written(chip->transmit_context);
        sh7709_update_interrupts(chip);
        return true;
    }
    if (offset >= 0x140 && offset < 0x150) { scif_write(chip, 0, offset - 0x140, value); return true; }
    if (offset >= 0x150 && offset < 0x160) { scif_write(chip, 1, offset - 0x150, value); return true; }
    return true;
}

bool sh7709_read(sh7709_t *chip, uint32_t pa, int size, uint32_t *value) {
    if (chip->scif_alias && pa - chip->scif_alias < SCIF_SIZE) {
        *value = scif_read(&chip->scif[1], pa - chip->scif_alias);
        return true;
    }
    (void)size;
    if (chip->variant == SH7709 && pa - AREA1_BASE < AREA1_SIZE) return area1_read(chip, pa - AREA1_BASE, value);
    if (pa < SCI_BASE) return false;
    if (pa < TMU_BASE) { *value = sci_read(&chip->sci, pa - SCI_BASE); return true; }
    if (pa < RTC_BASE) { *value = tmu_read(chip, pa - TMU_BASE); return true; }
    if (pa < INTC_BASE) { *value = rtc_read(chip, pa - RTC_BASE); return true; }
    if (pa < INTC_BASE + 6) {
        uint32_t offset = pa - INTC_BASE;
        *value = offset == 0 ? (uint32_t)((chip->icr0 & ~ICR0_NMIL) | (chip->nmi ? 0 : ICR0_NMIL)) : chip->priority[(offset - 2) / 2];
        return true;
    }
    if (pa >= BSC_BASE && pa < BSC_BASE + 0x20) {
        uint32_t offset = pa - BSC_BASE;
        if (offset == 0x10) *value = (uint32_t)(chip->cpu->cycles >> 6) & 0xFF;
        else *value = chip->bsc[offset / 2];
        return true;
    }
    switch (pa) {
        case CPG_BASE + 0x0: *value = chip->frqcr; return true;
        case CPG_BASE + 0x2: *value = chip->stbcr; return true;
        case CPG_BASE + 0x4: *value = chip->watchdog_count; return true;
        case CPG_BASE + 0x6: *value = chip->watchdog_control; return true;
        case CPG_BASE + 0x8: *value = chip->stbcr2; return true;
        case CCR_ADDRESS: *value = chip->ccr; return true;
        default: break;
    }
    if (pa >= 0xFFFFFF00u) { *value = 0; return true; }
    return false;
}

bool sh7709_write(sh7709_t *chip, uint32_t pa, int size, uint32_t value) {
    if (chip->scif_alias && pa - chip->scif_alias < SCIF_SIZE) {
        scif_write(chip, 1, pa - chip->scif_alias, value);
        sh7709_update_interrupts(chip);
        return true;
    }
    (void)size;
    bool handled = true;
    if (chip->variant == SH7709 && pa - AREA1_BASE < AREA1_SIZE) handled = area1_write(chip, pa - AREA1_BASE, value);
    else if (pa < SCI_BASE) return false;
    else if (pa < TMU_BASE) sci_write(chip, pa - SCI_BASE, value);
    else if (pa < RTC_BASE) tmu_write(chip, pa - TMU_BASE, value);
    else if (pa < INTC_BASE) rtc_write(chip, pa - RTC_BASE, value);
    else if (pa < INTC_BASE + 6) {
        uint32_t offset = pa - INTC_BASE;
        if (offset == 0) chip->icr0 = (uint16_t)value;
        else chip->priority[(offset - 2) / 2] = (uint16_t)value;
    } else if (pa >= BSC_BASE && pa < BSC_BASE + 0x20) {
        chip->bsc[(pa - BSC_BASE) / 2] = (uint16_t)value;
    } else {
        switch (pa) {
            case CPG_BASE + 0x0: chip->frqcr = (uint16_t)value; break;
            case CPG_BASE + 0x2: chip->stbcr = (uint8_t)value; break;
            case CPG_BASE + 0x4: if ((value >> 8) == 0x5A) chip->watchdog_count = value & 0xFF; break;
            case CPG_BASE + 0x6: if ((value >> 8) == 0xA5) chip->watchdog_control = value & 0xFF; break;
            case CPG_BASE + 0x8: chip->stbcr2 = (uint8_t)value; break;
            case CCR_ADDRESS: chip->ccr = value & 0x2F; break;
            default: if (pa < 0xFFFFFF00u) return false; break;
        }
    }
    sh7709_update_interrupts(chip);
    return handled;
}
