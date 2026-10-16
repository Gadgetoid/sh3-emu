/* Emulate the CMD646 PCI IDE controller's configuration space, task file decoding and bus master DMA, with an ATAPI drive on its primary channel. */
#include "core/cmd646.h"

#include <string.h>

#define CMD646_ID       0x06461095u
#define CMD646_CLASS    0x01018F07u
#define STATUS_DEVSEL   0x02800000u
#define INTERRUPT_PIN   0x00000100u

#define REG_ID          0x00u
#define REG_COMMAND     0x04u
#define REG_CLASS       0x08u
#define REG_BAR0        0x10u
#define REG_INTERRUPT   0x3Cu
#define REG_CFR         0x50u
#define COMMAND_IO      0x0001u
#define BAR_IO          0x00000001u
#define CFR_PRIMARY_INTERRUPT 0x04u

#define BAR_COMMAND     0
#define BAR_CONTROL     1
#define BAR_BUS_MASTER  4
#define CONTROL_OFFSET  2u

#define BM_COMMAND      0u
#define BM_MRDMODE      1u
#define BM_STATUS       2u
#define BM_PRD          4u
#define BM_START        0x01u
#define BM_TO_MEMORY    0x08u
#define BM_ACTIVE       0x01u
#define BM_ERROR        0x02u
#define BM_INTERRUPT    0x04u
#define BM_STATUS_KEPT  0x60u
#define MRDMODE_PRIMARY_INTERRUPT 0x04u

#define PRD_BYTES       8u
#define PRD_LAST        0x80000000u
#define PRD_COUNT_MASK  0xFFFEu
#define PRD_ENTRIES_MAX 512u
#define DMA_CHUNK       4096u

static const uint32_t bar_sizes[CMD646_BARS] = { 8, 4, 8, 4, 16 };

void cmd646_reset(cmd646_t *controller) {
    memset(controller->config, 0, sizeof controller->config);
    controller->config[REG_INTERRUPT / 4] = INTERRUPT_PIN;
    controller->bus_master_command = controller->bus_master_status = 0;
    controller->prd_table = 0;
}

static bool drive_interrupt(const cmd646_t *controller) {
    return atapi_interrupt(&controller->drive);
}

uint32_t cmd646_config_read(const cmd646_t *controller, uint32_t reg) {
    uint32_t index = (reg - REG_BAR0) / 4;
    if (reg >= REG_BAR0 && index < CMD646_BARS) return (controller->config[reg / 4] & ~(bar_sizes[index] - 1)) | BAR_IO;
    switch (reg) {
    case REG_ID: return CMD646_ID;
    case REG_COMMAND: return STATUS_DEVSEL | (controller->config[reg / 4] & 0xFFFFu);
    case REG_CLASS: return CMD646_CLASS;
    case REG_CFR: return controller->config[reg / 4] | (drive_interrupt(controller) ? CFR_PRIMARY_INTERRUPT : 0);
    default: return reg < sizeof controller->config ? controller->config[reg / 4] : 0;
    }
}

void cmd646_config_write(cmd646_t *controller, uint32_t reg, uint32_t value) {
    if (reg == REG_ID || reg == REG_CLASS || reg >= sizeof controller->config) return;
    if (reg == REG_COMMAND) value &= 0xFFFFu;
    controller->config[reg / 4] = value;
}

static bool decode(const cmd646_t *controller, uint32_t address, int *bar, uint32_t *offset) {
    if (!(controller->config[REG_COMMAND / 4] & COMMAND_IO)) return false;
    for (int index = 0; index < CMD646_BARS; index++) {
        uint32_t base = controller->config[REG_BAR0 / 4 + index] & ~(bar_sizes[index] - 1) & ~BAR_IO;
        if (base && address - base < bar_sizes[index]) {
            *bar = index;
            *offset = address - base;
            return true;
        }
    }
    return false;
}

static bool read_prd(const cmd646_host_t *host, uint32_t address, uint32_t *buffer, uint32_t *length, bool *last) {
    uint8_t entry[PRD_BYTES];
    if (!host->memory || !host->memory(host->context, address, entry, sizeof entry, false)) return false;
    *buffer = (uint32_t)entry[0] | (uint32_t)entry[1] << 8 | (uint32_t)entry[2] << 16 | (uint32_t)entry[3] << 24;
    uint32_t flags = (uint32_t)entry[4] | (uint32_t)entry[5] << 8 | (uint32_t)entry[6] << 16 | (uint32_t)entry[7] << 24;
    *length = flags & PRD_COUNT_MASK;
    if (*length == 0) *length = 0x10000u;
    *last = (flags & PRD_LAST) != 0;
    return true;
}

static void run_dma(cmd646_t *controller, const cmd646_host_t *host) {
    if (!(controller->bus_master_status & BM_ACTIVE) || !(controller->bus_master_command & BM_TO_MEMORY)) return;
    if (!atapi_dma_ready(&controller->drive)) return;
    uint32_t table = controller->prd_table;
    for (uint32_t entries = 0; entries < PRD_ENTRIES_MAX && atapi_dma_ready(&controller->drive); entries++) {
        uint32_t buffer, length;
        bool last;
        if (!read_prd(host, table, &buffer, &length, &last)) {
            controller->bus_master_status = (uint8_t)((controller->bus_master_status & ~BM_ACTIVE) | BM_ERROR | BM_INTERRUPT);
            return;
        }
        table += PRD_BYTES;
        uint8_t chunk[DMA_CHUNK];
        while (length && atapi_dma_ready(&controller->drive)) {
            uint32_t take = length < sizeof chunk ? length : (uint32_t)sizeof chunk;
            uint32_t got = atapi_dma_read(host->drive, chunk, take);
            if (!got) break;
            host->memory(host->context, buffer, chunk, got, true);
            buffer += got;
            length -= got;
        }
        if (last) {
            controller->bus_master_status &= (uint8_t) ~BM_ACTIVE;
            break;
        }
    }
}

static void sync_interrupt(cmd646_t *controller) {
    if (drive_interrupt(controller)) controller->bus_master_status |= BM_INTERRUPT;
}

static uint32_t bus_master_read(const cmd646_t *controller, uint32_t offset, int size) {
    uint8_t bytes[8] = { controller->bus_master_command, drive_interrupt(controller) ? MRDMODE_PRIMARY_INTERRUPT : 0, controller->bus_master_status, 0 };
    bytes[4] = (uint8_t)controller->prd_table;
    bytes[5] = (uint8_t)(controller->prd_table >> 8);
    bytes[6] = (uint8_t)(controller->prd_table >> 16);
    bytes[7] = (uint8_t)(controller->prd_table >> 24);
    uint32_t value = 0;
    for (int i = 0; i < size && offset + (uint32_t)i < sizeof bytes; i++) value |= (uint32_t)bytes[offset + (uint32_t)i] << (8 * i);
    return value;
}

static void bus_master_write_byte(cmd646_t *controller, uint32_t offset, uint8_t value) {
    switch (offset) {
    case BM_COMMAND:
        controller->bus_master_command = value & (BM_START | BM_TO_MEMORY);
        if (value & BM_START) controller->bus_master_status |= BM_ACTIVE;
        else controller->bus_master_status &= (uint8_t) ~BM_ACTIVE;
        break;
    case BM_STATUS:
        controller->bus_master_status = (uint8_t)((controller->bus_master_status & ~(BM_STATUS_KEPT | (value & (BM_ERROR | BM_INTERRUPT)))) | (value & BM_STATUS_KEPT));
        break;
    case BM_PRD:
    case BM_PRD + 1:
    case BM_PRD + 2:
    case BM_PRD + 3: {
        uint32_t shift = 8 * (offset - BM_PRD);
        controller->prd_table = (controller->prd_table & ~(0xFFu << shift)) | (uint32_t)value << shift;
        controller->prd_table &= ~3u;
        break;
    }
    default:
        break;
    }
}

static void command_block(const cmd646_host_t *host, uint32_t offset, int size, bool write, uint32_t *value) {
    atapi_slot_t *drive = host->drive;
    if (offset == ATAPI_DATA && size >= 2) {
        for (int word = 0; word < size / 2; word++) {
            if (write) atapi_write_data(drive, (uint16_t)(*value >> (16 * word)));
            else *value = (word ? *value : 0) | (uint32_t)atapi_read_data(drive) << (16 * word);
        }
        return;
    }
    for (int i = 0; i < size; i++) {
        int reg = (int)offset + i;
        if (write) atapi_write(drive, reg, (uint8_t)(*value >> (8 * i)));
        else *value = (i ? *value : 0) | (uint32_t)atapi_read(drive, reg) << (8 * i);
    }
}

bool cmd646_io(cmd646_t *controller, const cmd646_host_t *host, uint32_t address, int size, bool write, uint32_t *value) {
    int bar;
    uint32_t offset;
    if (!decode(controller, address, &bar, &offset)) return false;
    if (!host->drive || host->drive->state != &controller->drive) return true;
    switch (bar) {
    case BAR_COMMAND:
        command_block(host, offset, size, write, value);
        break;
    case BAR_CONTROL:
        if (offset != CONTROL_OFFSET) {
            if (!write) *value = 0;
        } else if (write) {
            atapi_write_control(host->drive, (uint8_t)*value);
        } else {
            *value = atapi_alternate_status(&controller->drive);
        }
        break;
    case BAR_BUS_MASTER:
        if (write) {
            for (int i = 0; i < size; i++) bus_master_write_byte(controller, offset + (uint32_t)i, (uint8_t)(*value >> (8 * i)));
        } else {
            *value = bus_master_read(controller, offset, size);
        }
        break;
    default:
        if (!write) *value = 0;
        break;
    }
    run_dma(controller, host);
    sync_interrupt(controller);
    return true;
}

bool cmd646_interrupt(const cmd646_t *controller) {
    return drive_interrupt(controller);
}
