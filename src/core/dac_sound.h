#pragma once
#include <stdint.h>

typedef void (*dac_sound_sample_fn)(void *context, int16_t sample);

typedef struct {
    uint64_t clock, last;
    int16_t level;
} dac_sound_t;

void dac_sound_write(dac_sound_t *sound, uint32_t cpu_hz, uint32_t sample_rate, uint8_t value, uint64_t cycle, dac_sound_sample_fn sample, void *context);
