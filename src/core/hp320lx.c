#include "core/hp320lx.h"

#include <string.h>

#define LINE_BYTES         (HP320LX_SCREEN_WIDTH / 4)
#define LEVEL_STEP         5u
#define CONTROLLER_COMMAND 0x0200000Au
#define DEBUG_STATUS       0x020000CEu
#define DEBUG_TX_READY     0x02u
#define DEBUG_BANK_STATUS  0x020000D4u
#define DEBUG_TX_BANK      0x02u
#define DEBUG_TX_DATA      0x020000C6u
#define LINK_STATUS        0x0200005Cu
#define LINK_READY         0x02u
#define LINK_NO_HOST       0x08u
#define LINK_CONTROL       0x02000056u
#define LINK_STROBE        0x40u
#define KEY_HOLD_SCANS     8u
#define ALL_ROWS           0xFFu
#define PORT_H_DATA        0x17u
#define PORT_K_DATA        0x19u
#define TOUCH_DRIVE        0x80u
#define TOUCH_X_SELECT     0x04u
#define TOUCH_Y_SELECT     0x01u
#define TOUCH_RAW_MIN      64u
#define TOUCH_RAW_SPAN     896u
#define TOUCH_PEN_LEVEL    0x3FFu

typedef struct {
    uint8_t control, data, bit;
} hp320lx_row_t;

typedef struct {
    uint8_t scancode, row, column;
} hp320lx_keymap_t;

static const hp320lx_row_t key_rows[HP320LX_KEY_ROWS] = {
    { 0x01, 0x11, 3 }, { 0x01, 0x11, 4 }, { 0x01, 0x11, 5 }, { 0x01, 0x11, 6 },
    { 0x01, 0x11, 7 }, { 0x08, 0x18, 4 }, { 0x0B, 0x1B, 3 }, { 0x03, 0x13, 7 },
};

static const hp320lx_keymap_t key_map[] = {
    { 0x12, 0, 0 }, { 0x2E, 0, 5 }, { 0x36, 0, 6 }, { 0x3E, 0, 7 }, { 0x46, 0, 8 }, { 0x4E, 0, 9 }, { 0x66, 0, 10 },
    { 0x0E, 1, 0 }, { 0x76, 1, 1 }, { 0x15, 1, 2 }, { 0x1E, 1, 3 }, { 0x26, 1, 4 }, { 0x25, 1, 5 }, { 0x2C, 1, 6 }, { 0x3D, 1, 7 }, { 0x45, 1, 8 }, { 0x5B, 1, 9 }, { 0x5D, 1, 10 },
    { 0x1C, 2, 0 }, { 0x0D, 2, 1 }, { 0x16, 2, 2 }, { 0x1D, 2, 3 }, { 0x24, 2, 4 }, { 0x2D, 2, 5 }, { 0x35, 2, 6 }, { 0x3C, 2, 7 }, { 0x4D, 2, 8 }, { 0x54, 2, 9 }, { 0x5A, 2, 10 },
    { 0x2B, 3, 5 }, { 0x34, 3, 6 }, { 0x43, 3, 7 }, { 0x44, 3, 8 }, { 0xF4, 3, 9 }, { 0x59, 3, 10 },
    { 0x14, 4, 1 }, { 0x1B, 4, 4 }, { 0x23, 4, 5 }, { 0x33, 4, 6 }, { 0x3B, 4, 7 }, { 0x42, 4, 8 }, { 0x52, 4, 9 }, { 0x55, 4, 10 },
    { 0x9F, 5, 2 }, { 0x1A, 5, 4 }, { 0x2A, 5, 5 }, { 0x3A, 5, 6 }, { 0x41, 5, 7 }, { 0x4B, 5, 8 }, { 0x4C, 5, 9 },
    { 0x11, 6, 3 }, { 0x21, 6, 4 }, { 0x32, 6, 5 }, { 0x31, 6, 6 }, { 0x49, 6, 7 }, { 0x4A, 6, 8 }, { 0xF5, 6, 9 },
    { 0x29, 7, 1 }, { 0x22, 7, 3 }, { 0x01, 7, 4 }, { 0x5E, 7, 4 }, { 0x09, 7, 6 }, { 0xF1, 7, 7 }, { 0xEB, 7, 8 }, { 0xF2, 7, 9 },
};

static bool contains(const uint8_t *image, size_t size, const char *text) {
    size_t length = strlen(text);
    for (size_t i = 0; i + length <= size; i++) {
        if (!memcmp(image + i, text, length)) return true;
    }
    return false;
}

hp_model_t hp320lx_detect(const uint8_t *image, size_t size) {
    if (contains(image, size, "hplib.dll")) return HP_MODEL_320LX;
    if (contains(image, size, "hpst.exe")) return HP_MODEL_300LX;
    return HP_MODEL_NONE;
}

void hp320lx_reset(hp320lx_t *board) {
    memset(board, 0, sizeof *board);
    board->key_changed_scan = UINT32_MAX;
}

static uint32_t *register_slot(hp320lx_t *board, uint32_t address, bool create) {
    for (uint32_t index = 0; index < board->count; index++) {
        if (board->address[index] == address) return &board->value[index];
    }
    if (!create || board->count == HP320LX_REGISTERS) return NULL;
    board->address[board->count] = address;
    board->value[board->count] = 0;
    return &board->value[board->count++];
}

static void debug_character(hp320lx_t *board, const hp320lx_host_t *host, uint8_t character) {
    if (character == '\r') return;
    if (character == '\n' || board->line_length >= (int)sizeof board->line - 1) {
        board->line[board->line_length] = 0;
        if (host->debug_line) host->debug_line(host->context, board->line);
        board->line_length = 0;
        if (character == '\n') return;
    }
    board->line[board->line_length++] = (char)(character >= 0x20 && character < 0x7F ? character : '?');
}

bool hp320lx_read(hp320lx_t *board, const hp320lx_host_t *host, uint32_t pa, int size, uint32_t *value) {
    uint32_t *slot = register_slot(board, pa, false);
    uint32_t mask = size == 1 ? 0xFFu : size == 2 ? 0xFFFFu : 0xFFFFFFFFu;
    switch (pa) {
    case CONTROLLER_COMMAND: *value = 0; return true;
    case DEBUG_STATUS: *value = DEBUG_TX_READY; return true;
    case DEBUG_BANK_STATUS: *value = DEBUG_TX_BANK; return true;
    case LINK_STATUS: {
        uint32_t *control = register_slot(board, LINK_CONTROL, false);
        bool strobe = control && (*control & LINK_STROBE);
        *value = ((slot ? *slot : 0) & ~LINK_READY) | (strobe ? 0 : LINK_READY) | LINK_NO_HOST;
        return true;
    }
    default: break;
    }
    *value = slot ? *slot & mask : 0;
    if (host->trace) host->trace(host->context, false, pa, size, *value);
    return true;
}

bool hp320lx_write(hp320lx_t *board, const hp320lx_host_t *host, uint32_t pa, int size, uint32_t value) {
    uint32_t *slot = register_slot(board, pa, true);
    uint32_t mask = size == 1 ? 0xFFu : size == 2 ? 0xFFFFu : 0xFFFFFFFFu;
    if (pa == DEBUG_TX_DATA) {
        debug_character(board, host, (uint8_t)value);
        return true;
    }
    if (slot) *slot = value & mask;
    if (host->trace) host->trace(host->context, true, pa, size, value);
    return true;
}

static void apply_key_events(hp320lx_t *board) {
    if (!board->key_event_count) return;
    bool idle = board->rows_driven == ALL_ROWS && board->scans != board->key_changed_scan;
    if (!idle && board->scans - board->key_changed_scan < KEY_HOLD_SCANS) return;
    const hp320lx_key_event_t *event = &board->key_events[board->key_event_head];
    uint16_t bit = (uint16_t)(1u << event->column);
    if (event->up) board->keys_down[event->row] &= (uint16_t) ~bit;
    else board->keys_down[event->row] |= bit;
    board->key_event_head = (board->key_event_head + 1) % HP320LX_KEY_EVENTS;
    board->key_event_count--;
    board->key_changed_scan = board->scans;
}

void hp320lx_woken(hp320lx_t *board) {
    board->key_changed_scan = UINT32_MAX;
}

bool hp320lx_key(hp320lx_t *board, uint8_t scancode, bool up) {
    for (size_t i = 0; i < sizeof key_map / sizeof key_map[0]; i++) {
        const hp320lx_keymap_t *key = &key_map[i];
        if (key->scancode != scancode) continue;
        if (board->key_event_count == HP320LX_KEY_EVENTS) return false;
        uint32_t tail = (board->key_event_head + board->key_event_count) % HP320LX_KEY_EVENTS;
        board->key_events[tail] = (hp320lx_key_event_t){ key->row, key->column, up };
        board->key_event_count++;
        apply_key_events(board);
        return true;
    }
    return false;
}

static bool row_driven_low(const hp320lx_row_t *row, const uint16_t *ports) {
    uint32_t mode = (ports[row->control] >> (row->bit * 2)) & 3;
    return mode == 1 && !(ports[row->data] & (1u << row->bit));
}

uint16_t hp320lx_key_columns(hp320lx_t *board, const uint16_t *ports) {
    uint8_t driven = 0;
    for (uint32_t row = 0; row < HP320LX_KEY_ROWS; row++) {
        if (row_driven_low(&key_rows[row], ports)) driven |= (uint8_t)(1u << row);
    }
    if (driven == 1 && board->rows_driven != 1) {
        board->scans++;
        apply_key_events(board);
    }
    board->rows_driven = driven;
    if (driven == ALL_ROWS) apply_key_events(board);
    uint16_t low = 0;
    for (uint32_t row = 0; row < HP320LX_KEY_ROWS; row++) {
        if (driven & (1u << row)) low |= board->keys_down[row];
    }
    return (uint16_t)(~low & ((1u << HP320LX_KEY_COLUMNS) - 1));
}

static uint16_t touch_raw(int pixel, int size) {
    if (pixel < 0) pixel = 0;
    if (pixel >= size) pixel = size - 1;
    return (uint16_t)(TOUCH_RAW_MIN + (uint32_t)pixel * TOUCH_RAW_SPAN / (uint32_t)size);
}

void hp320lx_touch(hp320lx_t *board, bool down, int x, int y) {
    board->pen_down = down;
    board->pen_x = touch_raw(x, HP320LX_SCREEN_WIDTH);
    board->pen_y = touch_raw(y, HP320LX_SCREEN_HEIGHT);
}

hp320lx_touch_t hp320lx_touch_inputs(const hp320lx_t *board, const uint16_t *ports) {
    hp320lx_touch_t inputs = { 0, 0, false };
    if (!board->pen_down) return inputs;
    uint16_t drive = ports[PORT_H_DATA] & TOUCH_DRIVE, select = ports[PORT_K_DATA] & (TOUCH_X_SELECT | TOUCH_Y_SELECT);
    if (!drive) {
        inputs.channel_a = TOUCH_PEN_LEVEL;
        inputs.pen_interrupt = true;
    } else if (select == TOUCH_X_SELECT) {
        inputs.channel_a = board->pen_x;
    } else if (select == TOUCH_Y_SELECT) {
        inputs.channel_b = board->pen_y;
    }
    return inputs;
}

void hp320lx_screen(const uint8_t *framebuffer, uint8_t *levels) {
    for (uint32_t y = 0; y < HP320LX_SCREEN_HEIGHT; y++) {
        for (uint32_t x = 0; x < HP320LX_SCREEN_WIDTH; x++) {
            uint8_t byte = framebuffer[y * LINE_BYTES + x / 4];
            uint32_t pixel = (byte >> ((3 - (x & 3)) * 2)) & 3;
            levels[y * HP320LX_SCREEN_WIDTH + x] = (uint8_t)((3 - pixel) * LEVEL_STEP);
        }
    }
}
