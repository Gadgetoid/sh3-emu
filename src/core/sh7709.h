#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/sh3.h"

typedef enum {
    SH7708,
    SH7709,
} sh7709_variant_t;

typedef struct {
    uint32_t constant;
    uint32_t count;
    uint16_t control;
    uint64_t remainder;
} sh7709_timer_t;

typedef struct {
    uint8_t  mode;
    uint8_t  bit_rate;
    uint8_t  control;
    uint8_t  transmit;
    uint8_t  status;
    uint8_t  receive;
    uint8_t  received[64];
    uint32_t received_head;
    uint32_t received_count;
} sh7709_serial_t;

typedef void (*sh7709_transmit_fn)(void *context, int port, uint8_t byte);

typedef struct {
    sh3_cpu_t *cpu;
    sh7709_variant_t variant;
    uint32_t cpu_hz;
    uint32_t peripheral_hz;
    uint64_t last_cycle;

    uint16_t icr0, icr1, icr2, pinter;
    uint16_t priority[5];
    uint8_t  irr0, irr1, irr2;
    uint32_t irl_level;
    uint32_t irq_lines;
    uint32_t extra_level, extra_code;
    bool     nmi;

    uint8_t  timer_start;
    uint8_t  timer_output;
    uint32_t timer_capture;
    sh7709_timer_t timer[3];

    uint8_t  rtc_64hz;
    uint8_t  rtc_counter[7];
    uint8_t  rtc_alarm[6];
    uint8_t  rtc_control1, rtc_control2;
    uint64_t rtc_remainder;

    sh7709_serial_t sci;
    sh7709_serial_t scif[2];
    sh7709_transmit_fn transmit;
    void    *transmit_context;

    uint16_t bsc[16];
    uint16_t refresh_count;
    uint16_t refresh_control;
    uint16_t refresh_constant;
    uint16_t frqcr;
    uint8_t  stbcr, stbcr2;
    uint16_t watchdog_count, watchdog_control;
    uint32_t ccr, ccr2;
    uint16_t ports[64];

    uint8_t  adc_control, adc_config;
    uint16_t adc_data[4];
    uint16_t adc_input[4];
    uint64_t adc_done;
} sh7709_t;

void sh7709_init(sh7709_t *chip, sh3_cpu_t *cpu, sh7709_variant_t variant, uint32_t cpu_hz, uint32_t peripheral_hz);
void sh7709_reset(sh7709_t *chip);
bool sh7709_read(sh7709_t *chip, uint32_t pa, int size, uint32_t *value);
bool sh7709_write(sh7709_t *chip, uint32_t pa, int size, uint32_t value);
void sh7709_advance(sh7709_t *chip);
uint64_t sh7709_next_event(sh7709_t *chip);
void sh7709_update_interrupts(sh7709_t *chip);
void sh7709_set_irl(sh7709_t *chip, uint32_t level);
void sh7709_set_irq(sh7709_t *chip, int line, bool asserted);
void sh7709_set_extra(sh7709_t *chip, uint32_t level, uint32_t code);
void sh7709_receive(sh7709_t *chip, int port, uint8_t byte);
void sh7709_set_time(sh7709_t *chip, int year, int month, int day, int weekday, int hour, int minute, int second);
void sh7709_add_seconds(sh7709_t *chip, uint32_t seconds);
uint32_t sh7709_timer_count(sh7709_t *chip, int index);
void sh7709_set_adc(sh7709_t *chip, int channel, uint16_t value);
