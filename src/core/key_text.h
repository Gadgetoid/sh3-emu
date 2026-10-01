#pragma once
#include <stdbool.h>
#include <stdint.h>

#define KEY_TEXT_SHIFT 0x51

bool key_text_find(char character, uint8_t *scancode, bool *shifted);
