/* Play the internet stream mapped to the tuned frequency, with static between and around stations. */
#include "net/radio_service.h"

#include <stdio.h>
#include <string.h>

#define NEAR_KHZ_FM    100u
#define NEAR_KHZ_AM    5u
#define STATIC_LEVEL   2500
#define STREAM_LEVEL   256
#define NEAR_STREAM    96
#define CHUNK_FRAMES   1024u

void radio_service_init(radio_service_t *service, const radio_stations_t *stations, const char *user_agent) {
    memset(service, 0, sizeof *service);
    service->stations = *stations;
    snprintf(service->user_agent, sizeof service->user_agent, "%s", user_agent ? user_agent : "");
    service->noise_state = 0x12345678u;
}

void radio_service_stop(radio_service_t *service) {
    radio_stream_close(service->stream);
    service->stream = NULL;
    service->playing = NULL;
}

static int32_t static_sample(radio_service_t *service) {
    service->noise_state = service->noise_state * 1664525u + 1013904223u;
    int32_t white = (int32_t)(service->noise_state >> 16) - 32768;
    service->hiss += (white - service->hiss) / 3;
    return service->hiss * STATIC_LEVEL / 32768;
}

static const radio_station_t *nearest(const radio_service_t *service, uint32_t khz, bool fm, uint32_t *distance) {
    const radio_station_t *best = NULL;
    uint32_t limit = fm ? NEAR_KHZ_FM : NEAR_KHZ_AM;
    for (size_t i = 0; i < service->stations.count; i++) {
        const radio_station_t *station = &service->stations.stations[i];
        uint32_t gap = station->khz > khz ? station->khz - khz : khz - station->khz;
        if (gap <= limit && (!best || gap < *distance)) {
            best = station;
            *distance = gap;
        }
    }
    return best;
}

static void tune(radio_service_t *service, const radio_station_t *station) {
    if (station == service->playing) return;
    radio_service_stop(service);
    service->playing = station;
    if (station && radio_stream_supported()) service->stream = radio_stream_open(station->url, service->user_agent);
}

void radio_service_fill(radio_service_t *service, uint32_t khz, bool fm, int16_t *mono, uint32_t frames) {
    uint32_t distance = 0;
    const radio_station_t *station = nearest(service, khz, fm, &distance);
    tune(service, station);
    int stream_gain = !station ? 0 : distance == 0 ? STREAM_LEVEL : NEAR_STREAM;
    int static_gain = !station ? STREAM_LEVEL : distance == 0 ? 0 : STREAM_LEVEL - NEAR_STREAM;
    for (uint32_t done = 0; done < frames;) {
        int16_t stereo[CHUNK_FRAMES * 2];
        uint32_t count = frames - done < CHUNK_FRAMES ? frames - done : CHUNK_FRAMES;
        size_t got = service->stream && stream_gain ? radio_stream_read(service->stream, stereo, count) : 0;
        for (uint32_t i = 0; i < count; i++) {
            int32_t music = i < got ? ((int32_t)stereo[2 * i] + stereo[2 * i + 1]) / 2 : 0;
            int32_t gain_static = i < got ? static_gain : STREAM_LEVEL;
            int32_t value = (music * stream_gain + static_sample(service) * gain_static) / STREAM_LEVEL;
            mono[done + i] = (int16_t)(value > 32767 ? 32767 : value < -32768 ? -32768 : value);
        }
        done += count;
    }
}
