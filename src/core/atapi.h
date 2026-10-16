#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "core/cdimage.h"

#define ATAPI_BUFFER_BLOCKS 16u

typedef struct {
    bool loaded, media_changed, irq, dma, writing_packet;
    uint8_t feature, error, sector_count, sector_number, byte_count_low, byte_count_high, drive_head;
    uint8_t status, device_control;
    uint8_t sense_key, sense_code, sense_qualifier;
    uint8_t packet[12];
    uint32_t packet_position;
    uint8_t buffer[ATAPI_BUFFER_BLOCKS * CDIMAGE_DATA_BYTES];
    uint32_t buffer_length, buffer_position, chunk_left;
    uint32_t next_block, blocks_left, total_blocks;
    bool audio;
    uint32_t current_block;
} atapi_t;

typedef uint64_t (*atapi_clock_fn)(void *context);

typedef struct {
    atapi_t *state;
    FILE    *image;
    cdimage_t disc;
    uint64_t ready_at;
    atapi_clock_fn clock;
    void    *clock_context;
    uint64_t clock_hz;
} atapi_slot_t;

enum {
    ATAPI_DATA = 0,
    ATAPI_FEATURE = 1,
    ATAPI_SECTOR_COUNT = 2,
    ATAPI_SECTOR_NUMBER = 3,
    ATAPI_BYTE_COUNT_LOW = 4,
    ATAPI_BYTE_COUNT_HIGH = 5,
    ATAPI_DRIVE_HEAD = 6,
    ATAPI_COMMAND = 7,
};

bool     atapi_load(atapi_slot_t *slot, FILE *image, const cdimage_t *disc);
void     atapi_unload(atapi_slot_t *slot);
void     atapi_rebind(atapi_slot_t *slot, FILE *image, const cdimage_t *disc);
void     atapi_reset(atapi_slot_t *slot);
void     atapi_sanitize(atapi_t *drive);
uint8_t  atapi_read(atapi_slot_t *slot, int reg);
void     atapi_write(atapi_slot_t *slot, int reg, uint8_t value);
uint16_t atapi_read_data(atapi_slot_t *slot);
void     atapi_write_data(atapi_slot_t *slot, uint16_t value);
uint8_t  atapi_alternate_status(const atapi_t *drive);
void     atapi_write_control(atapi_slot_t *slot, uint8_t value);
bool     atapi_interrupt(const atapi_t *drive);
bool     atapi_dma_ready(const atapi_slot_t *slot);
uint64_t atapi_dma_due(const atapi_slot_t *slot);
uint32_t atapi_dma_read(atapi_slot_t *slot, uint8_t *out, uint32_t length);
