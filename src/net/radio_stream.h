#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RADIO_STREAM_RATE 44100u

typedef struct radio_stream radio_stream_t;

bool            radio_stream_supported(void);
radio_stream_t *radio_stream_open(const char *url, const char *user_agent);
size_t          radio_stream_read(radio_stream_t *stream, int16_t *stereo, size_t frames);
void            radio_stream_close(radio_stream_t *stream);
