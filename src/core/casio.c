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
#define INT_STATUS      0x028u
#define INT_MASK        0x02Cu
#define INT_VECTOR      0x02Eu
#define KEY_ROWS        0x0E4u
#define KEY_COLUMNS     0x0E6u
#define KEY_INTERRUPT   0x0004u
#define KEY_VECTOR      2u
#define HIGH_ROW        8u
#define BOOT_CTRL_ROW   8u
#define BOOT_CTRL_COLUMN 0u
#define ASIC_IRL_LEVEL  4u
#define FIRST_ROW       0x0001u
#define KEY_HOLD_SCANS  4u
#define PINS_FIRST      0x040u
#define PINS_LAST       0x066u
#define PIN_INPUT       0x0030u
#define SLOT0_STATUS    0x326u
#define SLOT0_EMPTY     0x0006u
#define SLOT1_STATUS    0x282u
#define SLOT1_EMPTY     0x0007u

#define ONCHIP_PA       0xFFFFFE00u
#define ONCHIP_SIZE     0x80u
#define ONCHIP_EXTRA_PA 0xFFFFD000u
#define ONCHIP_PRIORITY_PA 0xFFFFFEE6u
#define TIMER_PA        0xFFFFFE20u
#define TIMER_STRIDE    0x20u
#define TIMER_COUNT     0x00u
#define TIMER_COMPARE   0x04u
#define TIMER_CLEAR     0x10u
#define TIMER_RUN       0x12u
#define TIMER_MODE      0x14u
#define TIMER_RUN_BIT   0x1u
#define TIMER_INTERRUPT_BIT 0x2u
#define TIMER_CODE      0x6C0u
#define TIMER_CODE_STRIDE 0x20u

#define VRAM_PA         0x08000000u
#define LCDC_OFFSET     0x1FF00u
#define VRAM_STRIDE     256u
#define LEVEL_STEP      5u

typedef struct {
    uint8_t scancode, row, column;
} casio_key_t;

static const casio_key_t key_map[] = {
    { 0x76, 0, 0 }, { 0x0D, 0, 1 }, { 0x16, 0, 2 }, { 0x1E, 0, 3 }, { 0x26, 0, 4 }, { 0x25, 0, 5 }, { 0x2E, 0, 6 }, { 0x58, 0, 7 },
    { 0x36, 1, 0 }, { 0x3D, 1, 1 }, { 0x3E, 1, 2 }, { 0x46, 1, 3 }, { 0x45, 1, 4 }, { 0x4E, 1, 5 }, { 0x55, 1, 6 },
    { 0x15, 2, 0 }, { 0x1D, 2, 1 }, { 0x24, 2, 2 }, { 0x2D, 2, 3 }, { 0x2C, 2, 4 }, { 0x35, 2, 5 }, { 0x3C, 2, 6 }, { 0x43, 2, 7 },
    { 0x1C, 3, 0 }, { 0x1B, 3, 1 }, { 0x23, 3, 2 }, { 0x2B, 3, 3 }, { 0x34, 3, 4 }, { 0x33, 3, 5 }, { 0x44, 3, 6 }, { 0x4D, 3, 7 },
    { 0x1A, 4, 0 }, { 0x22, 4, 1 }, { 0x21, 4, 2 }, { 0x2A, 4, 3 }, { 0x32, 4, 4 }, { 0x31, 4, 5 }, { 0x3B, 4, 6 }, { 0x42, 4, 7 },
    { 0x3A, 5, 0 }, { 0x41, 5, 1 }, { 0x4B, 5, 2 }, { 0x49, 5, 3 }, { 0x4C, 5, 4 }, { 0x54, 5, 5 }, { 0x4A, 5, 6 }, { 0x52, 5, 7 },
    { 0x66, 6, 0 }, { 0x0E, 6, 1 }, { 0x5B, 6, 2 }, { 0x5D, 6, 3 }, { 0xF5, 6, 4 }, { 0x5A, 6, 5 }, { 0x61, 6, 6 },
    { 0x67, 7, 0 }, { 0x29, 7, 1 }, { 0x64, 7, 2 }, { 0x13, 7, 3 }, { 0xEB, 7, 4 }, { 0xF2, 7, 5 }, { 0xF4, 7, 6 },
    { 0x14, 8, 0 }, { 0x11, 8, 1 }, { 0x91, 8, 1 }, { 0x59, 8, 2 }, { 0x12, 8, 3 }, { 0x9F, 8, 4 }, { 0xA7, 8, 4 }, { 0x94, 8, 6 },
};

void casio_reset(casio_t *board) {
    memset(board, 0, sizeof *board);
    board->boot_ctrl_held = true;
}

static bool unlocked(const casio_t *board) {
    return board->lock_low == LOCK_LOW_KEY && board->lock_high == LOCK_HIGH_KEY;
}

static bool asic_modelled(uint32_t offset) {
    switch (offset) {
        case LOCK_STATUS: case LOCK_LOW: case LOCK_HIGH: case POWER_STATUS: case INT_STATUS: case INT_MASK: case INT_VECTOR:
        case KEY_ROWS: case KEY_COLUMNS: case SLOT0_STATUS: case SLOT1_STATUS: return true;
        default: return false;
    }
}

static uint16_t key_columns(const casio_t *board) {
    uint16_t rows = board->asic[KEY_ROWS / 2], down = 0;
    for (uint32_t row = 0; row < CASIO_KEY_ROWS; row++) {
        if (!(rows & (1u << row))) continue;
        uint16_t columns = board->keys_down[row];
        if (row == BOOT_CTRL_ROW && board->boot_ctrl_held) columns |= 1u << BOOT_CTRL_COLUMN;
        down |= row == HIGH_ROW ? (uint16_t)(columns << 8) : columns;
    }
    return down;
}

static uint16_t asic_requests(const casio_t *board) {
    return key_columns(board) ? KEY_INTERRUPT : 0;
}

static uint16_t asic_vector(const casio_t *board) {
    uint16_t active = asic_requests(board) & board->asic[INT_MASK / 2];
    return (active & KEY_INTERRUPT) ? KEY_VECTOR : 0;
}

static uint16_t asic_read(casio_t *board, uint32_t offset) {
    uint16_t stored = board->asic[offset / 2];
    if (offset - PINS_FIRST <= PINS_LAST - PINS_FIRST) return stored & (uint16_t)~PIN_INPUT;
    switch (offset) {
        case INT_STATUS: return asic_requests(board);
        case INT_VECTOR: return asic_vector(board);
        case KEY_COLUMNS: return (uint16_t)~key_columns(board);
        case LOCK_STATUS: return unlocked(board) ? LOCK_OPEN : 0;
        case LOCK_LOW: return board->lock_low;
        case LOCK_HIGH: return board->lock_high;
        case POWER_STATUS: return stored | POWER_ON;
        case SLOT0_STATUS: return stored | SLOT0_EMPTY;
        case SLOT1_STATUS: return stored | SLOT1_EMPTY;
        default: return stored;
    }
}

static void release_held_keys(casio_t *board) {
    for (uint32_t row = 0; row < CASIO_KEY_ROWS; row++) {
        for (uint32_t column = 0; column < 8; column++) {
            uint8_t bit = (uint8_t)(1u << column);
            if ((board->keys_releasing[row] & bit) && board->scans - board->key_pressed_scan[row][column] >= KEY_HOLD_SCANS) {
                board->keys_releasing[row] &= (uint8_t)~bit;
                board->keys_down[row] &= (uint8_t)~bit;
            }
        }
    }
}

static void asic_write(casio_t *board, uint32_t offset, uint16_t value) {
    switch (offset) {
        case KEY_ROWS:
            if (value == FIRST_ROW) {
                board->scans++;
                release_held_keys(board);
            }
            board->asic[offset / 2] = value;
            break;
        case LOCK_LOW: board->lock_low = value; break;
        case LOCK_HIGH: board->lock_high = value; break;
        case INT_MASK:
            board->boot_ctrl_held = false;
            board->asic[offset / 2] = value;
            break;
        default: board->asic[offset / 2] = value; break;
    }
}

static uint64_t timer_ticks(const casio_host_t *host) {
    return host->cycles(host->context) * host->timer_hz / host->cpu_hz;
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
                *value = (timer->running ? TIMER_RUN_BIT : 0) | (timer->interrupt_enabled ? TIMER_INTERRUPT_BIT : 0);
                return true;
            }
            if (((*value & TIMER_RUN_BIT) != 0) != timer->running) {
                timer_restart(timer, host, timer_count(timer, host));
                timer->running = !timer->running;
            }
            timer->interrupt_enabled = (*value & TIMER_INTERRUPT_BIT) != 0;
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
    if (timer_access(board, host, pa, write, value)) {
        if (write) casio_update(board, host);
        return true;
    }
    uint16_t *slot;
    if (pa - ONCHIP_PA < ONCHIP_SIZE) slot = &board->onchip[(pa - ONCHIP_PA) / 2];
    else if (pa == ONCHIP_EXTRA_PA) slot = &board->onchip_extra;
    else if (pa == ONCHIP_PRIORITY_PA) slot = &board->onchip_priority;
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
    if (offset == INT_MASK || offset == KEY_ROWS) casio_update(board, host);
    return true;
}

static bool timer_pending(const casio_timer_t *timer, const casio_host_t *host) {
    return timer->running && timer->interrupt_enabled && timer_count(timer, host) >= timer->compare;
}

void casio_update(casio_t *board, const casio_host_t *host) {
    uint32_t level = 0, code = 0;
    for (int index = 0; index < CASIO_TIMERS; index++) {
        uint32_t priority = (board->onchip_priority >> (12 - 4 * index)) & 15;
        if (priority > level && timer_pending(&board->timers[index], host)) {
            level = priority;
            code = TIMER_CODE + (uint32_t)index * TIMER_CODE_STRIDE;
        }
    }
    host->onchip(host->context, level, code);
    host->irl(host->context, asic_vector(board) ? ASIC_IRL_LEVEL : 0, 0);
}

uint64_t casio_next_event(const casio_t *board, const casio_host_t *host) {
    uint64_t next = UINT64_MAX;
    for (int index = 0; index < CASIO_TIMERS; index++) {
        const casio_timer_t *timer = &board->timers[index];
        if (!timer->running || !timer->interrupt_enabled || timer->count >= timer->compare) continue;
        uint64_t tick = timer->started + (timer->compare - timer->count);
        uint64_t cycle = (tick * host->cpu_hz + host->timer_hz - 1) / host->timer_hz;
        if (cycle < next) next = cycle;
    }
    return next;
}

void casio_key(casio_t *board, const casio_host_t *host, uint8_t scancode, bool up) {
    for (size_t i = 0; i < sizeof key_map / sizeof key_map[0]; i++) {
        const casio_key_t *key = &key_map[i];
        if (key->scancode != scancode) continue;
        uint8_t bit = (uint8_t)(1u << key->column);
        if (up) {
            board->keys_releasing[key->row] |= bit;
            release_held_keys(board);
        } else {
            board->keys_down[key->row] |= bit;
            board->keys_releasing[key->row] &= (uint8_t)~bit;
            board->key_pressed_scan[key->row][key->column] = board->scans;
        }
        casio_update(board, host);
        return;
    }
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
