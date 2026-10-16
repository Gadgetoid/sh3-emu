#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "net/radio_stations.h"
#include "net/radio_stream.h"

typedef struct {
    radio_stations_t stations;
    char user_agent[256];
    const radio_station_t *playing;
    radio_stream_t *stream;
    uint32_t noise_state;
    int32_t hiss;
} radio_service_t;

void radio_service_init(radio_service_t *service, const radio_stations_t *stations, const char *user_agent);
void radio_service_fill(radio_service_t *service, uint32_t khz, bool fm, int16_t *mono, uint32_t frames);
void radio_service_stop(radio_service_t *service);
