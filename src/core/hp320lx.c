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

static const char signature[] = "hplib.dll";

bool hp320lx_detect(const uint8_t *image, size_t size) {
    size_t length = sizeof signature - 1;
    for (size_t i = 0; i + length <= size; i++) {
        if (!memcmp(image + i, signature, length)) return true;
    }
    return false;
}

void hp320lx_reset(hp320lx_t *board) {
    memset(board, 0, sizeof *board);
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

void hp320lx_screen(const uint8_t *framebuffer, uint8_t *levels) {
    for (uint32_t y = 0; y < HP320LX_SCREEN_HEIGHT; y++) {
        for (uint32_t x = 0; x < HP320LX_SCREEN_WIDTH; x++) {
            uint8_t byte = framebuffer[y * LINE_BYTES + x / 4];
            uint32_t pixel = (byte >> ((3 - (x & 3)) * 2)) & 3;
            levels[y * HP320LX_SCREEN_WIDTH + x] = (uint8_t)((3 - pixel) * LEVEL_STEP);
        }
    }
}
