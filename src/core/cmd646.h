#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "core/atapi.h"

#define CMD646_BARS 5

typedef bool (*cmd646_memory_fn)(void *context, uint32_t bus_address, uint8_t *data, uint32_t length, bool write);

typedef struct {
    uint32_t config[64];
    uint8_t bus_master_command, bus_master_status;
    uint32_t prd_table;
    atapi_t drive;
} cmd646_t;

typedef struct {
    atapi_slot_t *drive;
    cmd646_memory_fn memory;
    void *context;
} cmd646_host_t;

void     cmd646_reset(cmd646_t *controller);
uint32_t cmd646_config_read(const cmd646_t *controller, uint32_t reg);
void     cmd646_config_write(cmd646_t *controller, uint32_t reg, uint32_t value);
bool     cmd646_io(cmd646_t *controller, const cmd646_host_t *host, uint32_t address, int size, bool write, uint32_t *value);
bool     cmd646_interrupt(const cmd646_t *controller);
void     cmd646_service(cmd646_t *controller, const cmd646_host_t *host);
uint64_t cmd646_dma_due(const cmd646_t *controller, const cmd646_host_t *host);
