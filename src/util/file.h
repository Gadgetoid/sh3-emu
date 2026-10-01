#pragma once
#include <stddef.h>
#include <stdint.h>

uint8_t    *file_read(const char *path, size_t *size);
const char *file_leaf_name(const char *path);
