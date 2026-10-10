#include "core/pccard.h"

#define AREA5_PA    0x14000000u
#define AREA6_PA    0x18000000u
#define AREA_SIZE   0x04000000u
#define WINDOW_SIZE 0x01000000u

bool pccard_decode(uint32_t pa, pccard_address_t *address) {
    uint32_t offset;
    if (pa - AREA6_PA < AREA_SIZE) {
        address->socket = 0;
        offset = pa - AREA6_PA;
    } else if (pa - AREA5_PA < AREA_SIZE) {
        address->socket = 1;
        offset = pa - AREA5_PA;
    } else {
        return false;
    }
    address->space = (pccard_space_t)(offset / WINDOW_SIZE);
    address->offset = offset % WINDOW_SIZE;
    return true;
}

void pccard_access(cfcard_slot_t *slot, const pccard_address_t *address, int size, bool write, uint32_t *value) {
    if (!slot || !slot->state->inserted || address->space == PCCARD_UNUSED) {
        if (!write) *value = size == 1 ? 0xFFu : size == 2 ? 0xFFFFu : 0xFFFFFFFFu;
        return;
    }
    switch (address->space) {
    case PCCARD_ATTRIBUTE:
        if (write) cfcard_attribute_write(slot, address->offset, size, *value);
        else *value = cfcard_attribute_read(slot, address->offset, size);
        break;
    case PCCARD_COMMON:
        if (write) cfcard_common_write(slot, address->offset, size, *value);
        else *value = cfcard_common_read(slot, address->offset, size);
        break;
    default:
        if (write) cfcard_io_write(slot, address->offset, size, *value);
        else *value = cfcard_io_read(slot, address->offset, size);
        break;
    }
}
