#pragma once
#include <stdbool.h>
#include <stdint.h>

#define CLARION_GPS_FRAME_BYTES 83u

typedef struct {
    double latitude, longitude, altitude;
    double speed_kmh, heading;
    int year, month, day, hour, minute, second;
    bool valid;
} clarion_gps_fix_t;

void clarion_gps_encode(const clarion_gps_fix_t *fix, uint8_t frame[CLARION_GPS_FRAME_BYTES]);
