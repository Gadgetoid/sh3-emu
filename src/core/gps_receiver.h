#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/clarion_gps.h"

#define GPS_ROUTE_POINTS_MAX 4096

typedef struct {
    double latitude, longitude, altitude;
    double seconds;
} gps_point_t;

typedef struct {
    bool active;
    gps_point_t points[GPS_ROUTE_POINTS_MAX];
    uint32_t count;
    double speed_kmh, heading;
    double elapsed;
} gps_receiver_t;

void gps_receiver_clear(gps_receiver_t *receiver);
void gps_receiver_set_position(gps_receiver_t *receiver, double latitude, double longitude, double altitude, double speed_kmh, double heading);
bool gps_receiver_parse_position(gps_receiver_t *receiver, const char *text);
bool gps_receiver_load_gpx(gps_receiver_t *receiver, const char *path);
void gps_receiver_advance(gps_receiver_t *receiver, double seconds);
void gps_receiver_fix(const gps_receiver_t *receiver, int64_t utc_seconds, clarion_gps_fix_t *fix);
