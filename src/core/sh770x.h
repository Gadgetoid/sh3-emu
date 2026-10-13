#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/sh3.h"

typedef enum {
    SH7708,
    SH7707,
    SH7709,
} sh770x_variant_t;

#define SH770X_LCD_CONTROLS 14
#define SH770X_LCD_PALETTE  16
#define SH770X_LCD_FRAMES   5
#define SH770X_PCC_SOCKETS  2

typedef struct {
    uint32_t constant;
    uint32_t count;
    uint16_t control;
    uint64_t remainder;
} sh770x_timer_t;

typedef struct {
    uint8_t mode;
    uint8_t bit_rate;
    uint8_t control;
    uint8_t transmit;
    uint8_t status;
    uint8_t receive;
    uint8_t received[64];
    uint32_t received_head;
    uint32_t received_count;
} sh770x_serial_t;

typedef void (*sh770x_transmit_fn)(void *context, int port, uint8_t byte);
typedef void (*sh770x_ports_fn)(void *context);
typedef void (*sh770x_dac_fn)(void *context, int channel, uint8_t value, uint64_t cycle);

#define SH770X_DMA_CHANNELS 4

typedef struct {
    sh3_cpu_t *cpu;
    sh770x_variant_t variant;
    uint32_t cpu_hz;
    uint32_t peripheral_hz;
    uint64_t last_cycle;

    uint16_t icr0, icr1, icr2, pinter;
    uint16_t priority[5];
    uint8_t irr0, irr1, irr2;
    uint32_t irl_level;
    uint32_t irq_lines;
    uint32_t irq_active_high;
    uint32_t extra_level, extra_code;
    bool nmi;

    uint8_t timer_start;
    uint8_t timer_output;
    uint32_t timer_capture;
    sh770x_timer_t timer[3];

    uint8_t rtc_64hz;
    uint8_t rtc_counter[7];
    uint8_t rtc_alarm[6];
    uint8_t rtc_control1, rtc_control2;
    uint64_t rtc_remainder;

    sh770x_serial_t sci;
    sh770x_serial_t scif[2];
    sh770x_transmit_fn transmit;
    void    *transmit_context;
    sh770x_ports_fn ports_written;
    sh770x_dac_fn dac_written;

    uint32_t dma_source[SH770X_DMA_CHANNELS], dma_dest[SH770X_DMA_CHANNELS];
    uint32_t dma_count[SH770X_DMA_CHANNELS], dma_control[SH770X_DMA_CHANNELS];
    uint16_t dma_operation;
    uint16_t cmt_start, cmt_control, cmt_count, cmt_constant;
    uint64_t cmt_remainder;
    uint64_t transfer_cycle;
    uint8_t dac[2], dac_control;

    uint16_t bsc[16];
    uint16_t refresh_count;
    uint16_t refresh_control;
    uint16_t refresh_constant;
    uint16_t frqcr;
    uint8_t stbcr, stbcr2;
    uint16_t watchdog_count, watchdog_control;
    uint32_t ccr, ccr2;
    uint16_t ports[64];
    uint8_t pcc[16];
    uint32_t scif_alias, scif_alias_priority;
    uint16_t port_input_mask[64], port_input[64];

    uint8_t adc_control, adc_config;
    uint16_t adc_data[4];
    uint16_t adc_input[4];
    uint64_t adc_done;

    uint16_t lcd_index;
    uint16_t lcd_control[SH770X_LCD_CONTROLS];
    uint16_t lcd_palette[SH770X_LCD_PALETTE];
    uint16_t lcd_frame[SH770X_LCD_FRAMES];

    uint8_t pcc_status[SH770X_PCC_SOCKETS];
    bool pcc_card_irq[SH770X_PCC_SOCKETS];
    uint16_t iprf;
} sh770x_t;

void sh770x_init(sh770x_t *chip, sh3_cpu_t *cpu, sh770x_variant_t variant, uint32_t cpu_hz, uint32_t peripheral_hz);
void sh770x_reset(sh770x_t *chip);
bool sh770x_read(sh770x_t *chip, uint32_t pa, int size, uint32_t *value);
bool sh770x_write(sh770x_t *chip, uint32_t pa, int size, uint32_t value);
void sh770x_advance(sh770x_t *chip);
uint64_t sh770x_next_event(sh770x_t *chip);
void sh770x_update_interrupts(sh770x_t *chip);
void sh770x_set_irl(sh770x_t *chip, uint32_t level);
void sh770x_set_irq(sh770x_t *chip, int line, bool asserted);
void sh770x_set_irq_active_high(sh770x_t *chip, uint32_t lines);
void sh770x_set_extra(sh770x_t *chip, uint32_t level, uint32_t code);
void sh770x_receive(sh770x_t *chip, int port, uint8_t byte);
void sh770x_set_scif_alias(sh770x_t *chip, uint32_t pa, uint32_t priority);
size_t sh770x_receive_room(const sh770x_t *chip, int port);
uint32_t sh770x_baud(const sh770x_t *chip, int port);
void sh770x_set_time(sh770x_t *chip, int year, int month, int day, int weekday, int hour, int minute, int second);
void sh770x_add_seconds(sh770x_t *chip, uint32_t seconds);
uint32_t sh770x_timer_count(sh770x_t *chip, int index);
void sh770x_set_port_input(sh770x_t *chip, uint32_t pa, uint16_t mask, uint16_t value);
void sh770x_set_adc(sh770x_t *chip, int channel, uint16_t value);
void sh770x_set_pcc_status(sh770x_t *chip, int socket, uint8_t status);
void sh770x_set_pcc_irq(sh770x_t *chip, int socket, bool asserted);

extern const uint32_t SH770X_OLDER_STATE_SIZES[3];
bool sh770x_load_older_state(void *chip, const uint8_t *old, uint32_t size);
