/* Mix sound from several sources into one stream, placing each by its sample position in emulated time. */
#include "core/audio_mixer.h"

#include <string.h>

#define CHUNK_FRAMES 1024u

void audio_mixer_reset(audio_mixer_t *mixer, uint64_t position) {
    memset(mixer->frames, 0, sizeof mixer->frames);
    mixer->committed = position;
}

void audio_mixer_add(audio_mixer_t *mixer, uint64_t position, const int16_t *samples, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        uint64_t at = position + i;
        if (at < mixer->committed || at >= mixer->committed + AUDIO_MIXER_FRAMES) continue;
        mixer->frames[at % AUDIO_MIXER_FRAMES] += samples[i];
    }
}

uint32_t audio_mixer_commit(audio_mixer_t *mixer, uint64_t position, audio_mixer_emit_fn emit, void *context) {
    uint32_t total = 0;
    if (position > mixer->committed + AUDIO_MIXER_FRAMES) position = mixer->committed + AUDIO_MIXER_FRAMES;
    while (mixer->committed < position) {
        int16_t out[CHUNK_FRAMES];
        uint32_t count = position - mixer->committed < CHUNK_FRAMES ? (uint32_t)(position - mixer->committed) : CHUNK_FRAMES;
        for (uint32_t i = 0; i < count; i++) {
            int32_t *slot = &mixer->frames[(mixer->committed + i) % AUDIO_MIXER_FRAMES];
            int32_t value = *slot;
            out[i] = (int16_t)(value > 32767 ? 32767 : value < -32768 ? -32768 : value);
            *slot = 0;
        }
        emit(context, out, count);
        mixer->committed += count;
        total += count;
    }
    return total;
}
