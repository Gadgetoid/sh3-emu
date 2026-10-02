#pragma once
#include <stdbool.h>
#include <stdint.h>

#define KEY_TEXT_SHIFT 0x51

typedef enum { KEY_LAYOUT_ROM, KEY_LAYOUT_UPGRADE_CD } key_layout_t;

bool key_text_find(key_layout_t layout, char character, uint8_t *scancode, bool *shifted);
