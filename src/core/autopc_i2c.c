/* Emulate the AutoPC faceplate's I2C controller and the audio codec, AM/FM tuner and PLL on its bus. */
#include "core/autopc_i2c.h"

#include <string.h>

#define REG_ADDRESS 0x0u
#define REG_DATA    0x4u
#define REG_CONTROL 0x8u

#define CONTROL_STOP      0x0100u
#define STATUS_RECEIVED   0x0200u
#define STATUS_ACKED      0x0400u
#define STATUS_NACKED     0x0800u
#define STATUS_BITS       (STATUS_RECEIVED | STATUS_ACKED | STATUS_NACKED)
#define PENDING_RECEIVED  0x00020000u
#define PENDING_ACKED     0x00040000u
#define PENDING_NACKED    0x00080000u

#define DEVICE_CODEC      0x26u
#define DEVICE_TUNER      0xC2u
#define DEVICE_PLL        0xC4u
#define CODEC_INCREMENT   0x80u
#define CODEC_POINTER     0x1Fu
#define TUNER_FM          0x01u
#define TUNER_IF_WINDOW   0x04u
#define TUNER_UNMUTED     0x40u
#define STATION_STATUS    0xC7u
#define FM_IF_COUNT_HIGH  0xD6u
#define FM_IF_COUNT_LOW   0x5Cu
#define AM_IF_COUNT_HIGH  0x10u
#define AM_IF_COUNT_LOW   0xA0u
#define FM_OFFSET_KHZ     72100u
#define FM_STEP_KHZ       50u
#define AM_OFFSET_KHZ     10700u

void autopc_i2c_reset(autopc_i2c_t *bus) {
    memset(bus, 0, sizeof *bus);
}

static bool device_present(uint8_t device) {
    return device == DEVICE_CODEC || device == DEVICE_TUNER || device == DEVICE_PLL;
}

bool autopc_i2c_fm(const autopc_i2c_t *bus) {
    return (bus->tuner_control[1] & TUNER_FM) != 0;
}

bool autopc_i2c_tuner_audible(const autopc_i2c_t *bus) {
    return (bus->pll[0] || bus->pll[1]) && (bus->tuner_control[0] & TUNER_UNMUTED);
}

uint32_t autopc_i2c_khz(const autopc_i2c_t *bus) {
    uint32_t divider = (uint32_t)bus->pll[0] | (uint32_t)bus->pll[1] << 8;
    if (autopc_i2c_fm(bus)) return divider * FM_STEP_KHZ > FM_OFFSET_KHZ ? divider * FM_STEP_KHZ - FM_OFFSET_KHZ : 0;
    return divider > AM_OFFSET_KHZ ? divider - AM_OFFSET_KHZ : 0;
}

static uint8_t tuner_status(const autopc_i2c_t *bus, const autopc_i2c_host_t *host, uint32_t index) {
    bool station = host->station && host->station(host->context, autopc_i2c_khz(bus));
    bool window = (bus->tuner_control[1] & TUNER_IF_WINDOW) != 0;
    if (index == 0) return station ? STATION_STATUS : 0;
    if (autopc_i2c_fm(bus)) return window ? FM_IF_COUNT_HIGH : FM_IF_COUNT_LOW;
    return window ? AM_IF_COUNT_HIGH : AM_IF_COUNT_LOW;
}

static uint8_t next_byte(autopc_i2c_t *bus, const autopc_i2c_host_t *host) {
    uint32_t index = bus->index++;
    if (bus->device == DEVICE_TUNER) return tuner_status(bus, host, index);
    if (bus->device == DEVICE_CODEC) {
        uint8_t value = bus->codec[bus->codec_pointer & CODEC_POINTER];
        if (bus->codec_increment) bus->codec_pointer = (uint8_t)((bus->codec_pointer + 1) & CODEC_POINTER);
        return value;
    }
    return 0xFF;
}

static void write_byte(autopc_i2c_t *bus, uint8_t value) {
    uint32_t index = bus->index++;
    if (bus->device == DEVICE_CODEC) {
        if (index == 0) {
            bus->codec_pointer = value & CODEC_POINTER;
            bus->codec_increment = (value & CODEC_INCREMENT) != 0;
            return;
        }
        bus->codec[bus->codec_pointer & CODEC_POINTER] = value;
        if (bus->codec_increment) bus->codec_pointer = (uint8_t)((bus->codec_pointer + 1) & CODEC_POINTER);
    } else if (bus->device == DEVICE_TUNER) {
        if (index < sizeof bus->tuner_control) bus->tuner_control[index] = value;
    } else if (bus->device == DEVICE_PLL) {
        if (index < sizeof bus->pll) bus->pll[index] = value;
    }
}

static void acknowledge(autopc_i2c_t *bus, uint32_t *pending) {
    bus->status |= STATUS_ACKED;
    *pending |= PENDING_ACKED;
}

static void start(autopc_i2c_t *bus, const autopc_i2c_host_t *host, uint32_t value, uint32_t *pending) {
    bus->status &= ~STATUS_BITS;
    bus->device = (uint8_t)(value & 0xFE);
    bus->reading = (value & 1) != 0;
    bus->last = (value & CONTROL_STOP) != 0;
    bus->index = 0;
    if (!device_present(bus->device)) {
        bus->active = false;
        bus->status |= STATUS_NACKED;
        *pending |= PENDING_NACKED;
        return;
    }
    bus->active = !bus->last;
    acknowledge(bus, pending);
    if (bus->reading && bus->active) {
        bus->data = next_byte(bus, host);
        bus->status |= STATUS_RECEIVED;
        *pending |= PENDING_RECEIVED;
    }
}

uint32_t autopc_i2c_read(autopc_i2c_t *bus, const autopc_i2c_host_t *host, uint32_t offset, uint32_t *pending) {
    switch (offset) {
    case REG_DATA: {
        uint8_t value = bus->data;
        if (bus->active && bus->reading) {
            if (bus->last) {
                bus->active = false;
            } else {
                bus->data = next_byte(bus, host);
                bus->status |= STATUS_RECEIVED;
                *pending |= PENDING_RECEIVED;
            }
        }
        return value;
    }
    case REG_CONTROL: return (bus->control & ~(CONTROL_STOP | STATUS_BITS)) | bus->status;
    default: return 0;
    }
}

void autopc_i2c_write(autopc_i2c_t *bus, const autopc_i2c_host_t *host, uint32_t offset, uint32_t value, uint32_t *pending) {
    switch (offset) {
    case REG_ADDRESS:
        start(bus, host, value, pending);
        break;
    case REG_DATA:
        if (!bus->active) break;
        if (bus->reading) {
            if (value & CONTROL_STOP) bus->last = true;
            break;
        }
        write_byte(bus, (uint8_t)value);
        bus->status &= ~STATUS_BITS;
        acknowledge(bus, pending);
        if (value & CONTROL_STOP) bus->active = false;
        break;
    case REG_CONTROL:
        bus->control = value;
        if (value & CONTROL_STOP) {
            bus->status &= ~STATUS_NACKED;
            bus->active = false;
        }
        break;
    default:
        break;
    }
}
