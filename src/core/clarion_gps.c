/* Encode position fixes as the 83-byte frames of the Clarion YX004A GPS receiver that TrClarion.dll reads. */
#include "core/clarion_gps.h"

#include <math.h>
#include <string.h>

#define FRAME_START      0xC6u
#define FRAME_END        0x98u
#define UNITS_PER_DEGREE 60000.0
#define FIX_3D           0x02u
#define SATELLITE_USED   0x03u
#define SATELLITE_BYTES  6u
#define SATELLITES       8u
#define SATELLITE_OFFSET 24u
#define DILUTION_TENTHS  10

typedef struct {
    uint8_t prn;
    int azimuth;
    uint8_t elevation, signal;
} satellite_t;

static const satellite_t satellites[] = {
    { 5, 45, 60, 40 },
    { 12, 120, 35, 35 },
    { 17, 230, 50, 38 },
    { 24, 300, 25, 30 },
    { 29, 15, 70, 42 },
};

static void put28(uint8_t *bytes, long value) {
    uint32_t raw = (uint32_t)value & 0x0FFFFFFFu;
    bytes[0] = (uint8_t)(raw >> 21 & 0x7F);
    bytes[1] = (uint8_t)(raw >> 14 & 0x7F);
    bytes[2] = (uint8_t)(raw >> 7 & 0x7F);
    bytes[3] = (uint8_t)(raw & 0x7F);
}

static void put14(uint8_t *bytes, long value) {
    uint32_t raw = (uint32_t)value & 0x3FFFu;
    bytes[0] = (uint8_t)(raw >> 7 & 0x7F);
    bytes[1] = (uint8_t)(raw & 0x7F);
}

static uint8_t seven_bits(int value) {
    return (uint8_t)(value & 0x7F);
}

void clarion_gps_encode(const clarion_gps_fix_t *fix, uint8_t frame[CLARION_GPS_FRAME_BYTES]) {
    memset(frame, 0, CLARION_GPS_FRAME_BYTES);
    frame[0] = FRAME_START;
    frame[CLARION_GPS_FRAME_BYTES - 1] = FRAME_END;
    put28(frame + 1, lround(fix->latitude * UNITS_PER_DEGREE));
    put28(frame + 5, lround(fix->longitude * UNITS_PER_DEGREE));
    frame[9] = seven_bits(fix->year % 100);
    frame[10] = seven_bits(fix->month);
    frame[11] = seven_bits(fix->day);
    frame[12] = seven_bits(fix->hour);
    frame[13] = seven_bits(fix->minute);
    frame[14] = seven_bits(fix->second);
    put14(frame + 15, lround(fix->altitude));
    put14(frame + 17, lround(fix->speed_kmh * 10));
    put14(frame + 19, lround(fmod(fix->heading + 360.0, 360.0) * 10));
    put14(frame + 21, DILUTION_TENTHS);
    if (!fix->valid) return;
    frame[23] = FIX_3D;
    for (size_t i = 0; i < sizeof satellites / sizeof satellites[0] && i < SATELLITES; i++) {
        uint8_t *entry = frame + SATELLITE_OFFSET + i * SATELLITE_BYTES;
        entry[0] = satellites[i].prn;
        entry[1] = SATELLITE_USED;
        put14(entry + 2, satellites[i].azimuth);
        entry[4] = satellites[i].elevation;
        entry[5] = satellites[i].signal;
    }
}
