#pragma once
#include <stdbool.h>
#include <stdint.h>

#define KEYTEXT_SHIFT 0x51

bool keytext_find(char character, uint8_t *scancode, bool *shifted);
