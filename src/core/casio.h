#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/cfcard.h"

#define CASIO_SCREEN_WIDTH  480
#define CASIO_SCREEN_HEIGHT 240
#define CASIO_ASIC_WORDS    0x800
#define CASIO_TIMERS        2
#define CASIO_VRAM_SIZE     0x20000
#define CASIO_KEY_ROWS      9
#define CASIO_SCIF_PA       0xFFFFFE70u
#define CASIO_SCIF_PORT     2
#define CASIO_PERIPHERAL_HZ 10000000u
#define CASIO_BACKLIGHT_COLOUR 0x38B697u

typedef void (*casio_trace_fn)(void *context, bool write, uint32_t pa, int size, uint32_t value);
typedef uint64_t (*casio_cycles_fn)(void *context);
typedef void (*casio_interrupt_fn)(void *context, uint32_t level, uint32_t code);
typedef bool (*casio_memory_fn)(void *context, uint32_t pa, uint8_t *data, uint32_t length);
typedef void (*casio_samples_fn)(void *context, const int16_t *samples, uint32_t count, uint32_t rate);

typedef struct {
    uint32_t start, end, next_start, next_end;
    uint64_t ends_at;
    uint16_t status;
    bool     running, next_armed;
} casio_audio_t;

typedef struct {
    casio_trace_fn     trace;
    casio_cycles_fn    cycles;
    casio_interrupt_fn irl;
    casio_interrupt_fn onchip;
    uint32_t           cpu_hz;
    uint32_t           timer_hz;
    cfcard_slot_t     *card;
    casio_audio_t     *audio;
    casio_memory_fn    read_memory;
    casio_samples_fn   samples;
    void              *context;
} casio_host_t;

typedef struct {
    uint32_t count, compare;
    uint64_t started;
    uint16_t mode;
    bool     running, interrupt_enabled;
} casio_timer_t;

typedef struct {
    uint16_t asic[CASIO_ASIC_WORDS];
    uint8_t  vram[CASIO_VRAM_SIZE];
    casio_timer_t timers[CASIO_TIMERS];
    uint16_t lock_low, lock_high;
    uint16_t onchip[0x40];
    uint16_t onchip_extra;
    uint16_t onchip_priority;
    uint8_t  keys_down[CASIO_KEY_ROWS];
    uint8_t  keys_releasing[CASIO_KEY_ROWS];
    uint32_t key_pressed_scan[CASIO_KEY_ROWS][8];
    uint32_t scans;
    bool     powered_on;
    bool     pen_down;
    bool     dsr;
    uint16_t serial_flags;
    uint16_t pen_x, pen_y;
    uint16_t latched_requests;
} casio_t;

void casio_reset(casio_t *board);
bool casio_read(casio_t *board, const casio_host_t *host, uint32_t pa, int size, uint32_t *value);
bool casio_write(casio_t *board, const casio_host_t *host, uint32_t pa, int size, uint32_t value);
void casio_update(casio_t *board, const casio_host_t *host);
uint64_t casio_next_event(const casio_t *board, const casio_host_t *host);
void casio_key(casio_t *board, const casio_host_t *host, uint8_t scancode, bool up);
void casio_serial_line(casio_t *board, const casio_host_t *host, bool dsr);
uint32_t casio_serial_baud(const casio_t *board);
bool casio_backlight(const casio_t *board);
void casio_power_key(casio_t *board, bool down);
uint32_t casio_scif_priority(const casio_t *board);
void casio_card_changed(casio_t *board, const casio_host_t *host);
void casio_touch(casio_t *board, const casio_host_t *host, bool down, int x, int y);
void casio_screen(const casio_t *board, uint8_t *levels);
