#include "core/key_text.h"

#include <stddef.h>

typedef struct { char plain, shifted; uint8_t scancode; } key_char_t;

static const key_char_t key_chars[] = {
    { 'a', 'A', 0x1C }, { 'b', 'B', 0x32 }, { 'c', 'C', 0x21 }, { 'd', 'D', 0x23 }, { 'e', 'E', 0x24 },
    { 'f', 'F', 0x2B }, { 'g', 'G', 0x34 }, { 'h', 'H', 0x33 }, { 'i', 'I', 0x43 }, { 'j', 'J', 0x3B },
    { 'k', 'K', 0x42 }, { 'l', 'L', 0x4B }, { 'm', 'M', 0x3A }, { 'n', 'N', 0x31 }, { 'o', 'O', 0x44 },
    { 'p', 'P', 0x4D }, { 'q', 'Q', 0x15 }, { 'r', 'R', 0x2D }, { 's', 'S', 0x1B }, { 't', 'T', 0x2C },
    { 'u', 'U', 0x3C }, { 'v', 'V', 0x2A }, { 'w', 'W', 0x1D }, { 'x', 'X', 0x22 }, { 'y', 'Y', 0x35 },
    { 'z', 'Z', 0x1A },
    { '0', ')', 0x45 }, { '1', '!', 0x16 }, { '2', '@', 0x1E }, { '3', '#', 0x26 }, { '4', '$', 0x25 },
    { '5', '%', 0x2E }, { '6', '^', 0x36 }, { '7', '&', 0x3D }, { '8', '*', 0x3E }, { '9', '(', 0x46 },
    { ' ', 0, 0x29 }, { ';', ':', 0x4C }, { '=', '+', 0x55 }, { ',', '<', 0x41 }, { '-', '_', 0x4E },
    { '.', '>', 0x49 }, { '/', '?', 0x4A }, { '`', '~', 0x0E }, { '[', '{', 0x54 }, { '\\', '|', 0x5D },
    { ']', '}', 0x5B }, { '\'', '"', 0x52 }, { '\n', 0, 0x5A }, { '\t', 0, 0x0D },
};

static bool find_in(const key_char_t *table, size_t count, char character, uint8_t *scancode, bool *shifted, bool *listed) {
    for (size_t i = 0; i < count; i++) {
        bool is_shifted = table[i].shifted && table[i].shifted == character;
        if (table[i].plain != character && !is_shifted) continue;
        *listed = true;
        if (!table[i].scancode) return false;
        *scancode = table[i].scancode;
        *shifted = is_shifted;
        return true;
    }
    *listed = false;
    return false;
}

bool key_text_find(key_layout_t layout, char character, uint8_t *scancode, bool *shifted) {
    bool listed;
    (void)layout;
    return find_in(key_chars, sizeof key_chars / sizeof key_chars[0], character, scancode, shifted, &listed);
}
