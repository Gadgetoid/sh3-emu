#include "core/key_text.h"

#include <stddef.h>

typedef struct { char plain, shifted; uint8_t scancode; } key_char_t;

static const key_char_t key_chars[] = {
    { 'a', 'A', 0x14 }, { 'b', 'B', 0x2B }, { 'c', 'C', 0x2A }, { 'd', 'D', 0x2C }, { 'e', 'E', 0x28 },
    { 'f', 'F', 0x34 }, { 'g', 'G', 0x38 }, { 'h', 'H', 0x40 }, { 'i', 'I', 0x45 }, { 'j', 'J', 0x3C },
    { 'k', 'K', 0x44 }, { 'l', 'L', 0x36 }, { 'm', 'M', 0x3B }, { 'n', 'N', 0x33 }, { 'o', 'O', 0x3E },
    { 'p', 'P', 0x4D }, { 'q', 'Q', 0x26 }, { 'r', 'R', 0x30 }, { 's', 'S', 0x24 }, { 't', 'T', 0x2D },
    { 'u', 'U', 0x3D }, { 'v', 'V', 0x23 }, { 'w', 'W', 0x18 }, { 'x', 'X', 0x22 }, { 'y', 'Y', 0x35 },
    { 'z', 'Z', 0x12 },
    { '0', ')', 0x47 }, { '1', '!', 0x13 }, { '2', '@', 0x16 }, { '3', '#', 0x15 }, { '4', '$', 0x25 },
    { '5', '%', 0x17 }, { '6', '^', 0x27 }, { '7', '&', 0x2F }, { '8', '*', 0x37 }, { '9', '(', 0x3F },
    { ' ', 0, 0x21 }, { ';', ':', 0x4C }, { '=', '+', 0x4F }, { ',', '<', 0x43 }, { '-', '_', 0x4E },
    { '.', '>', 0x3A }, { '/', '?', 0x42 }, { '`', '~', 0x31 }, { '[', '{', 0x48 }, { '\\', '|', 0x50 },
    { ']', '}', 0x46 }, { '\'', '"', 0x2E }, { '\n', 0, 0x4B }, { '\t', 0, 0x11 },
};

static const key_char_t ce2_key_chars[] = {
    { '=', '+', 0x46 }, { '[', '{', 0x4F }, { ']', '}', 0x50 }, { '\\', '|', 0x31 }, { '`', '~', 0 },
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
    if (layout == KEY_LAYOUT_UPGRADE_CD) {
        bool found = find_in(ce2_key_chars, sizeof ce2_key_chars / sizeof ce2_key_chars[0], character, scancode, shifted, &listed);
        if (listed) return found;
    }
    return find_in(key_chars, sizeof key_chars / sizeof key_chars[0], character, scancode, shifted, &listed);
}
