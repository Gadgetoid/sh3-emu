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

bool pccard_present(const pccard_slot_t *slot) {
    if (!slot) return false;
    return (slot->storage && slot->storage->state->inserted) || (slot->ethernet && slot->ethernet->inserted);
}

bool pccard_interrupt(const pccard_slot_t *slot) {
    if (!pccard_present(slot)) return false;
    if (slot->storage && slot->storage->state->inserted) return cfcard_io_mode(slot->storage->state) && cfcard_interrupt(slot->storage->state);
    return ne2000_interrupt(slot->ethernet);
}

static void storage_access(cfcard_slot_t *slot, const pccard_address_t *address, int size, bool write, uint32_t *value) {
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

static void ethernet_access(ne2000_t *card, const pccard_address_t *address, int size, bool write, uint32_t *value) {
    switch (address->space) {
    case PCCARD_ATTRIBUTE:
        if (write) ne2000_attribute_write(card, address->offset, size, *value);
        else *value = ne2000_attribute_read(card, address->offset, size);
        break;
    case PCCARD_IO:
        if (write) ne2000_io_write(card, address->offset, size, *value);
        else *value = ne2000_io_read(card, address->offset, size);
        break;
    default:
        if (!write) *value = size == 1 ? 0xFFu : size == 2 ? 0xFFFFu : 0xFFFFFFFFu;
        break;
    }
}

void pccard_access(const pccard_slot_t *slot, const pccard_address_t *address, int size, bool write, uint32_t *value) {
    if (!pccard_present(slot) || address->space == PCCARD_UNUSED) {
        if (!write) *value = size == 1 ? 0xFFu : size == 2 ? 0xFFFFu : 0xFFFFFFFFu;
        return;
    }
    if (slot->storage && slot->storage->state->inserted) storage_access(slot->storage, address, size, write, value);
    else ethernet_access(slot->ethernet, address, size, write, value);
}
