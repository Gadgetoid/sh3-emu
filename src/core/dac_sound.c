/* Turn the levels written to a DAC into audio samples at a fixed rate. */
#include "core/dac_sound.h"

#include <stdbool.h>

#define GAP_DIVISOR 20u

void dac_sound_write(dac_sound_t *sound, uint32_t cpu_hz, uint32_t sample_rate, uint8_t value, uint64_t cycle, dac_sound_sample_fn sample, void *context) {
    uint64_t scaled = cycle * sample_rate;
    bool resumed = !sound->last || cycle - sound->last > cpu_hz / GAP_DIVISOR || scaled < sound->clock;
    if (resumed) sound->clock = scaled;
    while (sound->clock < scaled) {
        sample(context, sound->level);
        sound->clock += cpu_hz;
    }
    sound->level = (int16_t)(((int)value - 128) * 256);
    sound->last = cycle;
}
