#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "core/cfcard.h"

#define PCCARD_SOCKETS 2

typedef enum {
    PCCARD_ATTRIBUTE,
    PCCARD_COMMON,
    PCCARD_IO,
    PCCARD_UNUSED,
} pccard_space_t;

typedef struct {
    int            socket;
    pccard_space_t space;
    uint32_t       offset;
} pccard_address_t;

bool pccard_decode(uint32_t pa, pccard_address_t *address);
void pccard_access(cfcard_slot_t *slot, const pccard_address_t *address, int size, bool write, uint32_t *value);
