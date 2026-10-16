#pragma once
#include <stdbool.h>
#include <stdint.h>

#define AUTOPC_I2C_BASE    0xA000u
#define AUTOPC_I2C_SIZE    0x000Cu
#define AUTOPC_I2C_PENDING 0x000E0000u

typedef bool (*autopc_i2c_station_fn)(void *context, uint32_t khz);

typedef struct {
    uint32_t control, status;
    uint8_t data;
    uint8_t device;
    bool reading, last, active;
    uint32_t index;
    uint8_t codec[32];
    uint8_t codec_pointer;
    bool codec_increment;
    uint8_t tuner_control[2];
    uint8_t pll[4];
} autopc_i2c_t;

typedef struct {
    autopc_i2c_station_fn station;
    void *context;
} autopc_i2c_host_t;

void     autopc_i2c_reset(autopc_i2c_t *bus);
uint32_t autopc_i2c_read(autopc_i2c_t *bus, const autopc_i2c_host_t *host, uint32_t offset, uint32_t *pending);
void     autopc_i2c_write(autopc_i2c_t *bus, const autopc_i2c_host_t *host, uint32_t offset, uint32_t value, uint32_t *pending);
bool     autopc_i2c_fm(const autopc_i2c_t *bus);
bool     autopc_i2c_tuner_audible(const autopc_i2c_t *bus);
uint32_t autopc_i2c_khz(const autopc_i2c_t *bus);
