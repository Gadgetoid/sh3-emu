#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RADIO_STATIONS_MAX 64
#define RADIO_URL_BYTES    512
#define RADIO_NAME_BYTES   64

typedef struct {
    uint32_t khz;
    char url[RADIO_URL_BYTES];
    char name[RADIO_NAME_BYTES];
} radio_station_t;

typedef struct {
    radio_station_t stations[RADIO_STATIONS_MAX];
    size_t count;
} radio_stations_t;

void                   radio_stations_default(radio_stations_t *list);
bool                   radio_stations_load(radio_stations_t *list, const char *path);
const radio_station_t *radio_stations_find(const radio_stations_t *list, uint32_t khz);
