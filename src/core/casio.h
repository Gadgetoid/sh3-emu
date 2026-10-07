#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CASIO_SCREEN_WIDTH  480
#define CASIO_SCREEN_HEIGHT 240
#define CASIO_ASIC_WORDS    0x800
#define CASIO_TIMERS        2
#define CASIO_VRAM_SIZE     0x20000

typedef void (*casio_trace_fn)(void *context, bool write, uint32_t pa, int size, uint32_t value);
typedef uint64_t (*casio_cycles_fn)(void *context);

typedef struct {
    casio_trace_fn  trace;
    casio_cycles_fn cycles;
    uint32_t        cpu_hz;
    void           *context;
} casio_host_t;

typedef struct {
    uint32_t count, compare;
    uint64_t started;
    uint16_t mode;
    bool     running;
} casio_timer_t;

typedef struct {
    uint16_t asic[CASIO_ASIC_WORDS];
    uint8_t  vram[CASIO_VRAM_SIZE];
    casio_timer_t timers[CASIO_TIMERS];
    uint16_t lock_low, lock_high;
    uint16_t onchip[0x40];
    uint16_t onchip_extra;
} casio_t;

void casio_reset(casio_t *board);
bool casio_read(casio_t *board, const casio_host_t *host, uint32_t pa, int size, uint32_t *value);
bool casio_write(casio_t *board, const casio_host_t *host, uint32_t pa, int size, uint32_t value);
void casio_screen(const casio_t *board, uint8_t *levels);
