#include "core/casio.h"

#include <stdlib.h>
#include <string.h>

#define ASIC_PA         0x10000000u
#define ASIC_SIZE       (CASIO_ASIC_WORDS * 2u)

#define LOCK_STATUS     0x0C2u
#define LOCK_LOW        0x0C4u
#define LOCK_HIGH       0x0C6u
#define LOCK_LOW_KEY    0x5467u
#define LOCK_HIGH_KEY   0x6946u
#define LOCK_OPEN       0x0006u
#define POWER_STATUS    0x260u
#define POWER_AC        0x0002u
#define INT_STATUS      0x028u
#define INT_CLEAR       0x02Au
#define INT_MASK        0x02Cu
#define INT_VECTOR      0x02Eu
#define KEY_ROWS        0x0E4u
#define KEY_COLUMNS     0x0E6u
#define KEY_INTERRUPT   0x0004u
#define KEY_VECTOR      2u
#define TOUCH_INTERRUPT 0x0008u
#define TOUCH_VECTOR    6u
#define TOUCH_CHANNEL   0x08Cu
#define TOUCH_PEN_UP    0x08Au
#define TOUCH_RESULT_FIRST 0x098u
#define TOUCH_RESULT_LAST  0x09Cu
#define TOUCH_CHANNEL_X 2u
#define TOUCH_CHANNEL_Y 3u
#define TOUCH_RAW_MIN   64u
#define TOUCH_RAW_SPAN  896u
#define HIGH_ROW        8u
#define ASIC_IRL_LEVEL  4u
#define FIRST_ROW       0x0001u
#define KEY_HOLD_SCANS  4u
#define PINS_FIRST      0x040u
#define PINS_LAST       0x066u
#define PIN_INPUT       0x0030u
#define CARD_SOCKET     0
#define CARD_INTERRUPT  0x0080u
#define CARD_VECTOR     3u
#define CARD_CHANGE_INTERRUPT 0x0100u
#define CARD_CHANGE_VECTOR 14u
#define CARD_AREA5_PA   0x14000000u
#define CARD_AREA6_PA   0x18000000u
#define CARD_AREA_SIZE  0x04000000u
#define CARD_WINDOW_SIZE 0x01000000u
#define CARD_ATTRIBUTE  0
#define CARD_COMMON     1
#define CARD_IO         2
#define SLOT0_STATUS    0x326u
#define SLOT0_EMPTY     0x0006u
#define SLOT1_STATUS    0x282u
#define SLOT1_EMPTY     0x0007u
#define BACKLIGHT       0x008u
#define BACKLIGHT_LIT   0x0080u
#define SERIAL_EDGES    0x052u
#define SERIAL_RISE_ENABLE 0x0002u
#define SERIAL_FALL_ENABLE 0x0001u
#define SERIAL_RISEN    0x0020u
#define SERIAL_FALLEN   0x0010u
#define SERIAL_ENABLES  0x000Fu
#define SERIAL_INTERRUPT 0x0400u
#define SERIAL_VECTOR   8u
#define SERIAL_MODE     0x230u
#define SERIAL_SPEED    0x00C0u
#define SERIAL_115200   0x0080u
#define SERIAL_9600     0x0040u
#define SERIAL_DIVISOR  0x232u
#define SERIAL_DIVISOR_MASK 0x01FFu
#define SERIAL_DIVIDED_CLOCK 57600u
#define SERIAL_LINES    0x234u
#define SERIAL_DSR      0x0004u
#define SCIF_PRIORITY_SHIFT 4
#define AUDIO_CONTROL   0x100u
#define AUDIO_RATE_SHIFT 4
#define AUDIO_RATE_MASK 0x3u
#define AUDIO_RUN       0x160u
#define AUDIO_PLAY      0x0001u
#define AUDIO_STATUS    0x164u
#define AUDIO_SWITCHED  0x0001u
#define AUDIO_ENDED     0x0002u
#define AUDIO_START_LOW 0x170u
#define AUDIO_HIGH      0x172u
#define AUDIO_END_LOW   0x174u
#define AUDIO_NEXT_START_LOW 0x178u
#define AUDIO_NEXT_HIGH 0x17Au
#define AUDIO_NEXT_END_LOW 0x17Cu
#define AUDIO_INTERRUPT 0x0020u
#define AUDIO_VECTOR    12u
#define AUDIO_FRAME_WORDS 2u
#define AUDIO_FRAMES_MAX 0x4000u
#define PHYSICAL_MASK   0x1FFFFFFFu

#define ONCHIP_PA       0xFFFFFE00u
#define ONCHIP_SIZE     0x80u
#define ONCHIP_EXTRA_PA 0xFFFFD000u
#define ONCHIP_PRIORITY_PA 0xFFFFFEE6u
#define POWER_KEY_PA    0xFFFFFE04u
#define POWER_KEY_HELD  0x0002u
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
    { 0x36, 1, 0 }, { 0x3D, 1, 1 }, { 0x3E, 1, 2 }, { 0x46, 1, 3 }, { 0x45, 1, 4 }, { 0x4E, 1, 5 }, { 0x55, 1, 6 }, { 0x5E, 1, 7 },
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
}

static bool unlocked(const casio_t *board) {
    return board->lock_low == LOCK_LOW_KEY && board->lock_high == LOCK_HIGH_KEY;
}

static bool asic_modelled(uint32_t offset) {
    switch (offset) {
        case LOCK_STATUS: case LOCK_LOW: case LOCK_HIGH: case POWER_STATUS: case INT_STATUS: case INT_CLEAR: case INT_MASK: case INT_VECTOR:
        case TOUCH_PEN_UP: case 0x098u: case 0x09Au: case 0x09Cu:
        case KEY_ROWS: case KEY_COLUMNS: case SLOT0_STATUS: case SLOT1_STATUS: case SERIAL_EDGES: case SERIAL_LINES:
        case AUDIO_CONTROL: case AUDIO_RUN: case AUDIO_STATUS: case AUDIO_START_LOW: case AUDIO_HIGH: case AUDIO_END_LOW:
        case AUDIO_NEXT_START_LOW: case AUDIO_NEXT_HIGH: case AUDIO_NEXT_END_LOW: return true;
        default: return false;
    }
}

static uint16_t key_columns(const casio_t *board) {
    uint16_t rows = board->asic[KEY_ROWS / 2], down = 0;
    for (uint32_t row = 0; row < CASIO_KEY_ROWS; row++) {
        if (!(rows & (1u << row))) continue;
        uint16_t columns = board->keys_down[row];
        down |= row == HIGH_ROW ? (uint16_t)(columns << 8) : columns;
    }
    return down;
}

static bool card_present(const casio_host_t *host, int socket) {
    return socket == CARD_SOCKET && host->card && host->card->state->inserted;
}

static bool card_line(const casio_host_t *host) {
    if (!card_present(host, CARD_SOCKET)) return false;
    const cfcard_t *card = host->card->state;
    return cfcard_io_mode(card) ? cfcard_interrupt(card) : false;
}

static uint16_t asic_requests(const casio_t *board, const casio_host_t *host) {
    return (uint16_t)((key_columns(board) ? KEY_INTERRUPT : 0) | (card_line(host) ? CARD_INTERRUPT : 0) | (board->serial_flags ? SERIAL_INTERRUPT : 0) | board->latched_requests);
}

static uint16_t asic_vector(const casio_t *board, const casio_host_t *host) {
    uint16_t active = asic_requests(board, host) & board->asic[INT_MASK / 2];
    if (active & KEY_INTERRUPT) return KEY_VECTOR;
    if (active & CARD_INTERRUPT) return CARD_VECTOR;
    if (active & TOUCH_INTERRUPT) return TOUCH_VECTOR;
    if (active & CARD_CHANGE_INTERRUPT) return CARD_CHANGE_VECTOR;
    if (active & SERIAL_INTERRUPT) return SERIAL_VECTOR;
    if (active & AUDIO_INTERRUPT) return AUDIO_VECTOR;
    return 0;
}

static uint16_t touch_result(const casio_t *board) {
    switch (board->asic[TOUCH_CHANNEL / 2]) {
        case TOUCH_CHANNEL_X: return (uint16_t)(TOUCH_RAW_MIN + board->pen_x * TOUCH_RAW_SPAN / CASIO_SCREEN_WIDTH);
        case TOUCH_CHANNEL_Y: return (uint16_t)(TOUCH_RAW_MIN + board->pen_y * TOUCH_RAW_SPAN / CASIO_SCREEN_HEIGHT);
        default: return 0;
    }
}

static uint16_t asic_read(casio_t *board, const casio_host_t *host, uint32_t offset) {
    uint16_t stored = board->asic[offset / 2];
    if (offset == SERIAL_EDGES) return (uint16_t)((stored & SERIAL_ENABLES) | board->serial_flags);
    if (offset - PINS_FIRST <= PINS_LAST - PINS_FIRST) return stored & (uint16_t)~PIN_INPUT;
    if (offset - TOUCH_RESULT_FIRST <= TOUCH_RESULT_LAST - TOUCH_RESULT_FIRST) return touch_result(board);
    switch (offset) {
        case POWER_STATUS: return stored | POWER_AC;
        case INT_STATUS: return asic_requests(board, host);
        case INT_CLEAR: return 0;
        case AUDIO_STATUS: {
            uint16_t status = host->audio->status;
            host->audio->status = 0;
            return status;
        }
        case TOUCH_PEN_UP: return board->pen_down ? 0 : 1;
        case INT_VECTOR: return asic_vector(board, host);
        case KEY_COLUMNS: return (uint16_t)~key_columns(board);
        case LOCK_STATUS: return unlocked(board) ? LOCK_OPEN : 0;
        case LOCK_LOW: return board->lock_low;
        case LOCK_HIGH: return board->lock_high;
        case SERIAL_LINES: return board->dsr ? (uint16_t)(stored | SERIAL_DSR) : (uint16_t)(stored & ~SERIAL_DSR);
        case SLOT0_STATUS: return card_present(host, 0) ? (uint16_t)(stored & ~SLOT0_EMPTY) : (uint16_t)(stored | SLOT0_EMPTY);
        case SLOT1_STATUS: return card_present(host, 1) ? (uint16_t)(stored & ~SLOT1_EMPTY) : (uint16_t)(stored | SLOT1_EMPTY);
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

static uint32_t audio_rate(const casio_t *board) {
    static const uint32_t rates[] = { 8000, 22050, 11025, 11025 };
    return rates[(board->asic[AUDIO_CONTROL / 2] >> AUDIO_RATE_SHIFT) & AUDIO_RATE_MASK];
}

static uint32_t audio_frames(uint32_t start, uint32_t end) {
    return end >= start ? (end - start + 1) / AUDIO_FRAME_WORDS : 0;
}

static uint64_t audio_duration(const casio_t *board, const casio_host_t *host, uint32_t start, uint32_t end) {
    uint64_t frames = audio_frames(start, end);
    return frames ? frames * host->cpu_hz / audio_rate(board) : 1;
}

static void audio_start(casio_t *board, const casio_host_t *host) {
    casio_audio_t *audio = host->audio;
    uint32_t high = (uint32_t)board->asic[AUDIO_HIGH / 2] << 16;
    audio->start = high | board->asic[AUDIO_START_LOW / 2];
    audio->end = high | board->asic[AUDIO_END_LOW / 2];
    audio->running = true;
    audio->ends_at = host->cycles(host->context) + audio_duration(board, host, audio->start, audio->end);
}

static void audio_play_buffer(const casio_t *board, const casio_host_t *host) {
    const casio_audio_t *audio = host->audio;
    uint32_t frames = audio_frames(audio->start, audio->end);
    if (!frames || frames > AUDIO_FRAMES_MAX || !host->read_memory || !host->samples) return;
    uint32_t length = frames * AUDIO_FRAME_WORDS * 2;
    uint8_t *data = malloc(length);
    int16_t *samples = malloc(frames * sizeof *samples);
    if (data && samples && host->read_memory(host->context, (audio->start << 1) & PHYSICAL_MASK, data, length)) {
        for (uint32_t i = 0; i < frames; i++) samples[i] = (int16_t)(data[i * 4] | data[i * 4 + 1] << 8);
        host->samples(host->context, samples, frames, audio_rate(board));
    }
    free(data);
    free(samples);
}

static void audio_advance(casio_t *board, const casio_host_t *host) {
    casio_audio_t *audio = host->audio;
    while (audio->running && host->cycles(host->context) >= audio->ends_at) {
        audio_play_buffer(board, host);
        if (audio->next_armed) {
            audio->start = audio->next_start;
            audio->end = audio->next_end;
            audio->next_armed = false;
            audio->status |= AUDIO_SWITCHED;
            audio->ends_at += audio_duration(board, host, audio->start, audio->end);
        } else {
            audio->running = false;
            audio->status |= AUDIO_ENDED;
        }
        board->latched_requests |= AUDIO_INTERRUPT;
    }
}

static void asic_write(casio_t *board, const casio_host_t *host, uint32_t offset, uint16_t value) {
    switch (offset) {
        case AUDIO_RUN:
            board->asic[offset / 2] = value;
            if ((value & AUDIO_PLAY) && !host->audio->running) audio_start(board, host);
            else if (!(value & AUDIO_PLAY)) host->audio->running = false;
            break;
        case AUDIO_NEXT_END_LOW: {
            board->asic[offset / 2] = value;
            uint32_t high = (uint32_t)board->asic[AUDIO_NEXT_HIGH / 2] << 16;
            host->audio->next_start = high | board->asic[AUDIO_NEXT_START_LOW / 2];
            host->audio->next_end = high | value;
            host->audio->next_armed = true;
            break;
        }
        case KEY_ROWS:
            if (value == FIRST_ROW) {
                board->scans++;
                release_held_keys(board);
            }
            board->asic[offset / 2] = value;
            break;
        case LOCK_LOW: board->lock_low = value; break;
        case LOCK_HIGH: board->lock_high = value; break;
        case SERIAL_EDGES:
            board->serial_flags &= (uint16_t)~value;
            board->asic[offset / 2] = value & SERIAL_ENABLES;
            break;
        case INT_CLEAR:
            board->latched_requests &= (uint16_t)~value;
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

static bool card_access(casio_t *board, const casio_host_t *host, uint32_t pa, int size, bool write, uint32_t *value) {
    int socket;
    uint32_t offset;
    if (pa - CARD_AREA5_PA < CARD_AREA_SIZE) {
        socket = 1;
        offset = pa - CARD_AREA5_PA;
    } else if (pa - CARD_AREA6_PA < CARD_AREA_SIZE) {
        socket = 0;
        offset = pa - CARD_AREA6_PA;
    } else {
        return false;
    }
    uint32_t window = offset / CARD_WINDOW_SIZE, within = offset % CARD_WINDOW_SIZE;
    if (!card_present(host, socket) || window > CARD_IO) {
        if (!write) *value = size == 1 ? 0xFFu : size == 2 ? 0xFFFFu : 0xFFFFFFFFu;
        if (host->trace) host->trace(host->context, write, pa, size, *value);
        return true;
    }
    cfcard_slot_t *slot = host->card;
    switch (window) {
        case CARD_ATTRIBUTE:
            if (write) cfcard_attribute_write(slot, within, size, *value);
            else *value = cfcard_attribute_read(slot, within, size);
            break;
        case CARD_COMMON:
            if (write) cfcard_common_write(slot, within, size, *value);
            else *value = cfcard_common_read(slot, within, size);
            break;
        default:
            if (write) cfcard_io_write(slot, within, size, *value);
            else *value = cfcard_io_read(slot, within, size);
            break;
    }
    casio_update(board, host);
    return true;
}

bool casio_read(casio_t *board, const casio_host_t *host, uint32_t pa, int size, uint32_t *value) {
    if (card_access(board, host, pa, size, false, value)) return true;
    if (onchip_access(board, host, pa, size, false, value)) return true;
    if (vram_access(board, host, pa, size, false, value)) return true;
    if (pa - ASIC_PA >= ASIC_SIZE) return false;
    uint32_t offset = (pa - ASIC_PA) & ~1u;
    *value = asic_read(board, host, offset);
    if (size == 4) *value |= (uint32_t)asic_read(board, host, offset + 2) << 16;
    else if (size == 1) *value = (pa & 1) ? *value >> 8 : *value & 0xFFu;
    if (!asic_modelled(offset) && host->trace) host->trace(host->context, false, pa, size, *value);
    return true;
}

bool casio_write(casio_t *board, const casio_host_t *host, uint32_t pa, int size, uint32_t value) {
    if (card_access(board, host, pa, size, true, &value)) return true;
    if (onchip_access(board, host, pa, size, true, &value)) return true;
    if (vram_access(board, host, pa, size, true, &value)) return true;
    if (pa - ASIC_PA >= ASIC_SIZE) return false;
    uint32_t offset = (pa - ASIC_PA) & ~1u;
    if (size == 1) {
        uint16_t old = asic_read(board, host, offset);
        value = (pa & 1) ? (old & 0x00FFu) | (value & 0xFFu) << 8 : (old & 0xFF00u) | (value & 0xFFu);
    }
    asic_write(board, host, offset, (uint16_t)value);
    if (size == 4) asic_write(board, host, offset + 2, (uint16_t)(value >> 16));
    if (!asic_modelled(offset) && host->trace) host->trace(host->context, true, pa, size, value);
    if (offset == INT_MASK || offset == INT_CLEAR || offset == KEY_ROWS || offset == SERIAL_EDGES || offset == AUDIO_RUN) casio_update(board, host);
    return true;
}

static bool timer_pending(const casio_timer_t *timer, const casio_host_t *host) {
    return timer->running && timer->interrupt_enabled && timer_count(timer, host) >= timer->compare;
}

void casio_update(casio_t *board, const casio_host_t *host) {
    audio_advance(board, host);
    uint32_t level = 0, code = 0;
    for (int index = 0; index < CASIO_TIMERS; index++) {
        uint32_t priority = (board->onchip_priority >> (12 - 4 * index)) & 15;
        if (priority > level && timer_pending(&board->timers[index], host)) {
            level = priority;
            code = TIMER_CODE + (uint32_t)index * TIMER_CODE_STRIDE;
        }
    }
    host->onchip(host->context, level, code);
    host->irl(host->context, asic_vector(board, host) ? ASIC_IRL_LEVEL : 0, 0);
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
    if (host->audio->running && host->audio->ends_at < next) next = host->audio->ends_at;
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

void casio_touch(casio_t *board, const casio_host_t *host, bool down, int x, int y) {
    board->pen_x = (uint16_t)(x < 0 ? 0 : x >= CASIO_SCREEN_WIDTH ? CASIO_SCREEN_WIDTH - 1 : x);
    board->pen_y = (uint16_t)(y < 0 ? 0 : y >= CASIO_SCREEN_HEIGHT ? CASIO_SCREEN_HEIGHT - 1 : y);
    if (down && !board->pen_down) board->latched_requests |= TOUCH_INTERRUPT;
    board->pen_down = down;
    casio_update(board, host);
}

void casio_serial_line(casio_t *board, const casio_host_t *host, bool dsr) {
    if (dsr == board->dsr) return;
    uint16_t enables = board->asic[SERIAL_EDGES / 2];
    if (dsr && (enables & SERIAL_RISE_ENABLE)) board->serial_flags |= SERIAL_RISEN;
    if (!dsr && (enables & SERIAL_FALL_ENABLE)) board->serial_flags |= SERIAL_FALLEN;
    board->dsr = dsr;
    casio_update(board, host);
}

uint32_t casio_serial_baud(const casio_t *board) {
    uint16_t mode = board->asic[SERIAL_MODE / 2] & SERIAL_SPEED;
    if (mode == SERIAL_115200) return 115200;
    if (mode == SERIAL_9600) return 9600;
    return SERIAL_DIVIDED_CLOCK / ((board->asic[SERIAL_DIVISOR / 2] & SERIAL_DIVISOR_MASK) + 1u);
}

uint32_t casio_scif_priority(const casio_t *board) {
    return (board->onchip_priority >> SCIF_PRIORITY_SHIFT) & 15u;
}

void casio_card_changed(casio_t *board, const casio_host_t *host) {
    board->latched_requests |= CARD_CHANGE_INTERRUPT;
    casio_update(board, host);
}

void casio_power_key(casio_t *board, bool down) {
    uint16_t *status = &board->onchip[(POWER_KEY_PA - ONCHIP_PA) / 2];
    *status = down ? (uint16_t)(*status | POWER_KEY_HELD) : (uint16_t)(*status & ~POWER_KEY_HELD);
}

bool casio_backlight(const casio_t *board) {
    return (board->asic[BACKLIGHT / 2] & BACKLIGHT_LIT) != 0;
}
