#pragma once

#include <stddef.h>
#include <stdint.h>

#include "core/machine.h"

typedef struct {
    char path[MACHINE_BOARD_COUNT][1024];
    size_t size[MACHINE_BOARD_COUNT];
} rom_set_t;

void         rom_catalog_find(rom_set_t *roms, const char *folder);
int          rom_catalog_probe(const char *path, uint32_t *screens);
uint32_t     rom_catalog_label(const char *path, char *label, size_t label_size);
screen_size_t rom_catalog_screen(const char *path, screen_size_t preferred);
