#pragma once
#include <stdint.h>

#define AUDIO_MIXER_FRAMES 16384u

typedef struct {
    int32_t frames[AUDIO_MIXER_FRAMES];
    uint64_t committed;
} audio_mixer_t;

typedef void (*audio_mixer_emit_fn)(void *context, const int16_t *samples, uint32_t count);

void     audio_mixer_reset(audio_mixer_t *mixer, uint64_t position);
void     audio_mixer_add(audio_mixer_t *mixer, uint64_t position, const int16_t *samples, uint32_t count);
uint32_t audio_mixer_commit(audio_mixer_t *mixer, uint64_t position, audio_mixer_emit_fn emit, void *context);
