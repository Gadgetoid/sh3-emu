/* Wire the Hitachi HPW-200EC board: its keyboard matrix, touch panel, power key, card slots, display and DOLPHIN gate array around the SH7709 and HD64461. */
#include "core/hpw200ec.h"

#include <string.h>

#define DOLPHIN_PA        0x0A000000u
#define DOLPHIN_SIZE      0x00600000u
#define PC_CARD_SOCKET    0
#define CF_SOCKET         1
#define PORT_C_DATA       0xA4000124u
#define PORT_F_DATA       0xA400012Au
#define PORT_C_IDLE       0x0003u
#define PORT_F_IDLE       0x00FFu
#define PORT_C_COLUMN     0x01u
#define PORT_F_COLUMNS    0xFEu
#define ROW_ALWAYS        0xFFu
#define POWER_ROW         10
#define POWER_COLUMN      0
#define PORT_H_DATA       0xA400012Eu
#define PEN_UP            0x0040u
#define PEN_IRQ           2
#define TOUCH_Y_ADC       0
#define TOUCH_X_ADC       1
#define SUPPLY_ADC        2
#define REFERENCE_ADC     3
#define SUPPLY_LEVEL      0x3FFu
#define REFERENCE_LEVEL   0x002u
#define TOUCH_RAW_MIN     64u
#define TOUCH_RAW_SPAN    896u

typedef struct {
    uint8_t control, data, bit;
} hpw200ec_row_t;

typedef struct {
    uint8_t scancode, row, column;
} hpw200ec_keymap_t;

static const hpw200ec_row_t key_rows[HPW200EC_KEY_ROWS] = {
    { 0x04, 0x14, 6 }, { 0x04, 0x14, 3 }, { 0x04, 0x14, 1 }, { 0x04, 0x14, 0 },
    { 0x02, 0x12, 7 }, { 0x02, 0x12, 6 }, { 0x02, 0x12, 5 }, { 0x02, 0x12, 4 }, { 0x02, 0x12, 3 }, { 0x02, 0x12, 2 },
    { 0x03, 0x13, 1 }, { 0, 0, ROW_ALWAYS },
};

static const hpw200ec_keymap_t key_map[] = {
    { 0x05, 0, 0 }, { 0x9F, 0, 1 }, { 0x76, 0, 2 }, { 0x0E, 0, 3 }, { 0x1A, 0, 4 }, { 0x1C, 0, 5 }, { 0x1D, 0, 6 }, { 0x1B, 0, 7 },
    { 0x06, 1, 0 }, { 0x14, 1, 2 }, { 0x94, 1, 2 }, { 0x0D, 1, 3 }, { 0x15, 1, 5 }, { 0x16, 1, 6 }, { 0x1E, 1, 7 },
    { 0x04, 2, 0 }, { 0x11, 2, 3 }, { 0x91, 2, 3 }, { 0x58, 2, 4 }, { 0x22, 2, 5 }, { 0x26, 2, 6 }, { 0x24, 2, 7 },
    { 0x0C, 3, 0 }, { 0x2B, 3, 3 }, { 0x2C, 3, 4 }, { 0x23, 3, 5 }, { 0x25, 3, 6 }, { 0x2D, 3, 7 },
    { 0x03, 4, 0 }, { 0x2A, 4, 1 }, { 0x34, 4, 3 }, { 0x33, 4, 4 }, { 0x35, 4, 5 }, { 0x2E, 4, 6 },
    { 0x0B, 5, 0 }, { 0x32, 5, 1 }, { 0x21, 5, 3 }, { 0x3B, 5, 4 }, { 0x3C, 5, 5 }, { 0x36, 5, 6 }, { 0x3D, 5, 7 },
    { 0x83, 6, 0 }, { 0x31, 6, 1 }, { 0x3A, 6, 2 }, { 0x42, 6, 3 }, { 0x44, 6, 4 }, { 0x43, 6, 5 }, { 0x3E, 6, 6 },
    { 0x0A, 7, 0 }, { 0x29, 7, 1 }, { 0x41, 7, 2 }, { 0x4B, 7, 3 }, { 0x4C, 7, 4 }, { 0x4D, 7, 5 }, { 0x46, 7, 7 },
    { 0x01, 8, 0 }, { 0x54, 8, 1 }, { 0x5B, 8, 2 }, { 0x49, 8, 3 }, { 0x4A, 8, 4 }, { 0x5A, 8, 5 }, { 0xDA, 8, 5 }, { 0x4E, 8, 6 }, { 0x45, 8, 7 },
    { 0x09, 9, 0 }, { 0xEB, 9, 1 }, { 0xF2, 9, 2 }, { 0xF5, 9, 3 }, { 0x52, 9, 4 }, { 0x55, 9, 5 }, { 0x66, 9, 6 }, { 0x5D, 9, 7 },
    { 0x12, 10, 4 }, { 0x59, 10, 5 }, { 0xF4, 10, 6 }, { 0xF1, 10, 7 },
    { 0x78, 11, 0 },
};

static bool contains(const uint8_t *image, size_t size, const char *text) {
    size_t length = strlen(text);
    for (size_t i = 0; i + length <= size; i++) {
        if (!memcmp(image + i, text, length)) return true;
    }
    return false;
}

bool hpw200ec_detect(const uint8_t *image, size_t size) {
    return contains(image, size, "dlpncfg.cpl");
}

void hpw200ec_reset(hpw200ec_t *board) {
    memset(board, 0, sizeof *board);
    hd64461_reset(&board->companion);
}

static bool row_driven(const hpw200ec_row_t *row, const uint16_t *ports) {
    if (row->bit == ROW_ALWAYS) return true;
    uint32_t mode = (ports[row->control] >> (row->bit * 2)) & 3;
    return mode == 1 && !(ports[row->data] & (1u << row->bit));
}

void hpw200ec_ports_written(hpw200ec_t *board, const hpw200ec_host_t *host) {
    uint8_t low = 0;
    for (uint32_t row = 0; row < HPW200EC_KEY_ROWS; row++) {
        if (row_driven(&key_rows[row], host->chip->ports)) low |= board->keys_down[row];
    }
    sh770x_set_port_input(host->chip, PORT_C_DATA, PORT_C_IDLE, PORT_C_IDLE & ~(low & PORT_C_COLUMN));
    sh770x_set_port_input(host->chip, PORT_F_DATA, PORT_F_IDLE, PORT_F_IDLE & ~(low & PORT_F_COLUMNS));
}

static void set_key(hpw200ec_t *board, const hpw200ec_host_t *host, uint32_t row, uint32_t column, bool down) {
    if (down) board->keys_down[row] |= (uint8_t)(1u << column);
    else board->keys_down[row] &= (uint8_t) ~(1u << column);
    hpw200ec_ports_written(board, host);
}

void hpw200ec_key(hpw200ec_t *board, const hpw200ec_host_t *host, uint8_t scancode, bool up) {
    for (size_t i = 0; i < sizeof key_map / sizeof key_map[0]; i++) {
        if (key_map[i].scancode == scancode) set_key(board, host, key_map[i].row, key_map[i].column, !up);
    }
}

static uint16_t touch_raw(int pixel, int size) {
    if (pixel < 0) pixel = 0;
    if (pixel >= size) pixel = size - 1;
    return (uint16_t)(TOUCH_RAW_MIN + (uint32_t)pixel * TOUCH_RAW_SPAN / (uint32_t)size);
}

static void touch_lines(const hpw200ec_t *board, const hpw200ec_host_t *host) {
    sh770x_set_adc(host->chip, TOUCH_X_ADC, board->pen_down ? board->pen_x : 0);
    sh770x_set_adc(host->chip, TOUCH_Y_ADC, board->pen_down ? board->pen_y : 0);
    sh770x_set_port_input(host->chip, PORT_H_DATA, PEN_UP, board->pen_down ? 0 : PEN_UP);
    sh770x_set_irq(host->chip, PEN_IRQ, board->pen_down);
}

void hpw200ec_touch(hpw200ec_t *board, const hpw200ec_host_t *host, bool down, int x, int y) {
    board->pen_down = down;
    board->pen_x = touch_raw(x, HPW200EC_SCREEN_WIDTH);
    board->pen_y = touch_raw(y, HPW200EC_SCREEN_HEIGHT);
    touch_lines(board, host);
}

void hpw200ec_power_button(hpw200ec_t *board, const hpw200ec_host_t *host, bool down) {
    set_key(board, host, POWER_ROW, POWER_COLUMN, down);
}

void hpw200ec_power_on(hpw200ec_t *board, const hpw200ec_host_t *host) {
    sh770x_set_adc(host->chip, SUPPLY_ADC, SUPPLY_LEVEL);
    sh770x_set_adc(host->chip, REFERENCE_ADC, REFERENCE_LEVEL);
    touch_lines(board, host);
    hpw200ec_ports_written(board, host);
    hd64461_cards_changed(&board->companion, &host->companion);
}

static uint32_t *register_slot(hpw200ec_t *board, uint32_t address, bool create) {
    for (uint32_t index = 0; index < board->count; index++) {
        if (board->address[index] == address) return &board->value[index];
    }
    if (!create || board->count == HPW200EC_REGISTERS) return NULL;
    board->address[board->count] = address;
    board->value[board->count] = 0;
    return &board->value[board->count++];
}

bool hpw200ec_read(hpw200ec_t *board, const hpw200ec_host_t *host, uint32_t pa, int size, uint32_t *value) {
    if (hd64461_read(&board->companion, &host->companion, pa, size, value)) return true;
    if (pa - DOLPHIN_PA >= DOLPHIN_SIZE) return false;
    uint32_t *slot = register_slot(board, pa, false);
    uint32_t mask = size == 1 ? 0xFFu : size == 2 ? 0xFFFFu : 0xFFFFFFFFu;
    *value = slot ? *slot & mask : 0;
    if (host->trace) host->trace(host->context, false, pa, size, *value);
    return true;
}

bool hpw200ec_write(hpw200ec_t *board, const hpw200ec_host_t *host, uint32_t pa, int size, uint32_t value) {
    if (hd64461_write(&board->companion, &host->companion, pa, size, value)) return true;
    if (pa - DOLPHIN_PA >= DOLPHIN_SIZE) return false;
    uint32_t *slot = register_slot(board, pa, true);
    uint32_t mask = size == 1 ? 0xFFu : size == 2 ? 0xFFFFu : 0xFFFFFFFFu;
    if (slot) *slot = value & mask;
    if (host->trace) host->trace(host->context, true, pa, size, value);
    return true;
}

pccard_slot_t hpw200ec_socket(const hpw200ec_host_t *host, int socket) {
    pccard_slot_t slot = { NULL, NULL };
    if (socket == PC_CARD_SOCKET && host->ethernet->inserted) slot.ethernet = host->ethernet;
    if (socket == CF_SOCKET) slot.storage = host->card;
    return slot;
}

bool hpw200ec_card_access(hpw200ec_t *board, const hpw200ec_host_t *host, uint32_t pa, int size, bool write, uint32_t *value) {
    return hd64461_card_access(&board->companion, &host->companion, pa, size, write, value);
}

void hpw200ec_cards_changed(hpw200ec_t *board, const hpw200ec_host_t *host) {
    hd64461_cards_changed(&board->companion, &host->companion);
}

void hpw200ec_screen(const hpw200ec_t *board, uint8_t *levels) {
    memcpy(levels, board->companion.vram, HPW200EC_SCREEN_WIDTH * HPW200EC_SCREEN_HEIGHT);
}

int hpw200ec_palette(const hpw200ec_t *board, uint32_t *palette) {
    return hd64461_palette(&board->companion, palette);
}
