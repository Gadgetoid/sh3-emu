#include "core/casio.h"

#include <string.h>

#define ASIC_PA         0x10000000u
#define ASIC_SIZE       (CASIO_ASIC_WORDS * 2u)

#define LOCK_STATUS     0x0C2u
#define LOCK_LOW        0x0C4u
#define LOCK_HIGH       0x0C6u
#define LOCK_LOW_KEY    0x5467u
#define LOCK_HIGH_KEY   0x6946u
#define LOCK_OPEN       0x0006u
#define POWER_STATUS    0x016u
#define POWER_ON        0x1000u
#define KEY_COLUMNS     0x0E6u
#define KEY_ON          0x0100u
#define SLOT0_STATUS    0x326u
#define SLOT0_EMPTY     0x0006u
#define SLOT1_STATUS    0x282u
#define SLOT1_EMPTY     0x0007u

#define ONCHIP_PA       0xFFFFFE00u
#define ONCHIP_SIZE     0x80u
#define ONCHIP_EXTRA_PA 0xFFFFD000u
#define TIMER_PA        0xFFFFFE20u
#define TIMER_STRIDE    0x20u
#define TIMER_HZ        32768u
#define TIMER_COUNT     0x00u
#define TIMER_COMPARE   0x04u
#define TIMER_CLEAR     0x10u
#define TIMER_RUN       0x12u
#define TIMER_MODE      0x14u

#define VRAM_PA         0x08000000u
#define LCDC_OFFSET     0x1FF00u
#define VRAM_STRIDE     256u
#define LEVEL_STEP      5u

void casio_reset(casio_t *board) {
    memset(board, 0, sizeof *board);
}

static bool unlocked(const casio_t *board) {
    return board->lock_low == LOCK_LOW_KEY && board->lock_high == LOCK_HIGH_KEY;
}

static bool asic_modelled(uint32_t offset) {
    switch (offset) {
        case LOCK_STATUS: case LOCK_LOW: case LOCK_HIGH: case POWER_STATUS: case KEY_COLUMNS: case SLOT0_STATUS: case SLOT1_STATUS: return true;
        default: return false;
    }
}

static uint16_t asic_read(casio_t *board, uint32_t offset) {
    uint16_t stored = board->asic[offset / 2];
    switch (offset) {
        case LOCK_STATUS: return unlocked(board) ? LOCK_OPEN : 0;
        case LOCK_LOW: return board->lock_low;
        case LOCK_HIGH: return board->lock_high;
        case POWER_STATUS: return stored | POWER_ON;
        case KEY_COLUMNS: return (uint16_t)~KEY_ON;
        case SLOT0_STATUS: return stored | SLOT0_EMPTY;
        case SLOT1_STATUS: return stored | SLOT1_EMPTY;
        default: return stored;
    }
}

static void asic_write(casio_t *board, uint32_t offset, uint16_t value) {
    switch (offset) {
        case LOCK_LOW: board->lock_low = value; break;
        case LOCK_HIGH: board->lock_high = value; break;
        default: board->asic[offset / 2] = value; break;
    }
}

static uint64_t timer_ticks(const casio_host_t *host) {
    return host->cycles(host->context) * TIMER_HZ / host->cpu_hz;
}

static uint32_t timer_count(const casio_timer_t *timer, const casio_host_t *host) {
    return timer->running ? timer->count + (uint32_t)(timer_ticks(host) - timer->started) : timer->count;
}

static void timer_restart(casio_timer_t *timer, const casio_host_t *host, uint32_t count) {
    timer->count = count;
    timer->started = timer_ticks(host);
}

static bool timer_access(casio_t *board, const casio_host_t *host, uint32_t pa, bool write, uint32_t *value) {
    uint32_t offset = pa - TIMER_PA;
    if (offset >= CASIO_TIMERS * TIMER_STRIDE) return false;
    casio_timer_t *timer = &board->timers[offset / TIMER_STRIDE];
    switch (offset % TIMER_STRIDE) {
        case TIMER_COUNT:
            if (write) timer_restart(timer, host, *value);
            else *value = timer_count(timer, host);
            return true;
        case TIMER_COMPARE:
            if (write) timer->compare = *value;
            else *value = timer->compare;
            return true;
        case TIMER_CLEAR:
            if (write && (*value & 1)) timer_restart(timer, host, 0);
            if (!write) *value = 0;
            return true;
        case TIMER_RUN:
            if (!write) {
                *value = timer->running;
            } else if (((*value & 1) != 0) != timer->running) {
                timer_restart(timer, host, timer_count(timer, host));
                timer->running = !timer->running;
            }
            return true;
        case TIMER_MODE:
            if (write) timer->mode = (uint16_t)*value;
            else *value = timer->mode;
            return true;
        default:
            return false;
    }
}

static bool onchip_access(casio_t *board, const casio_host_t *host, uint32_t pa, int size, bool write, uint32_t *value) {
    if (timer_access(board, host, pa, write, value)) return true;
    uint16_t *slot;
    if (pa - ONCHIP_PA < ONCHIP_SIZE) slot = &board->onchip[(pa - ONCHIP_PA) / 2];
    else if (pa == ONCHIP_EXTRA_PA) slot = &board->onchip_extra;
    else return false;
    if (write) *slot = (uint16_t)*value;
    else *value = size == 1 ? (pa & 1 ? *slot >> 8 : *slot & 0xFFu) : *slot;
    if (host->trace) host->trace(host->context, write, pa, size, *value);
    return true;
}

static bool vram_access(casio_t *board, const casio_host_t *host, uint32_t pa, int size, bool write, uint32_t *value) {
    uint32_t offset = pa - VRAM_PA;
    if (offset >= CASIO_VRAM_SIZE) return false;
    if (write) {
        for (int i = 0; i < size; i++) board->vram[offset + (uint32_t)i] = (uint8_t)(*value >> (8 * i));
    } else {
        *value = 0;
        for (int i = 0; i < size; i++) *value |= (uint32_t)board->vram[offset + (uint32_t)i] << (8 * i);
    }
    if (offset >= LCDC_OFFSET && host->trace) host->trace(host->context, write, pa, size, *value);
    return true;
}

bool casio_read(casio_t *board, const casio_host_t *host, uint32_t pa, int size, uint32_t *value) {
    if (onchip_access(board, host, pa, size, false, value)) return true;
    if (vram_access(board, host, pa, size, false, value)) return true;
    if (pa - ASIC_PA >= ASIC_SIZE) return false;
    uint32_t offset = (pa - ASIC_PA) & ~1u;
    *value = asic_read(board, offset);
    if (size == 4) *value |= (uint32_t)asic_read(board, offset + 2) << 16;
    else if (size == 1) *value = (pa & 1) ? *value >> 8 : *value & 0xFFu;
    if (!asic_modelled(offset) && host->trace) host->trace(host->context, false, pa, size, *value);
    return true;
}

bool casio_write(casio_t *board, const casio_host_t *host, uint32_t pa, int size, uint32_t value) {
    if (onchip_access(board, host, pa, size, true, &value)) return true;
    if (vram_access(board, host, pa, size, true, &value)) return true;
    if (pa - ASIC_PA >= ASIC_SIZE) return false;
    uint32_t offset = (pa - ASIC_PA) & ~1u;
    if (size == 1) {
        uint16_t old = asic_read(board, offset);
        value = (pa & 1) ? (old & 0x00FFu) | (value & 0xFFu) << 8 : (old & 0xFF00u) | (value & 0xFFu);
    }
    asic_write(board, offset, (uint16_t)value);
    if (size == 4) asic_write(board, offset + 2, (uint16_t)(value >> 16));
    if (!asic_modelled(offset) && host->trace) host->trace(host->context, true, pa, size, value);
    return true;
}

void casio_screen(const casio_t *board, uint8_t *levels) {
    for (uint32_t y = 0; y < CASIO_SCREEN_HEIGHT; y++) {
        for (uint32_t x = 0; x < CASIO_SCREEN_WIDTH; x++) {
            uint8_t byte = board->vram[y * VRAM_STRIDE + x / 4];
            uint32_t pixel = (byte >> ((3 - (x & 3)) * 2)) & 3;
            levels[y * CASIO_SCREEN_WIDTH + x] = (uint8_t)((3 - pixel) * LEVEL_STEP);
        }
    }
}
