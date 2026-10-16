/* Simulate a GPS receiver at a fixed or moving position, or following a GPX route, and report its fix. */
#include "core/gps_receiver.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#ifdef _WIN32
#define timegm _mkgmtime
#endif

#define EARTH_RADIUS_M     6371000.0
#define DEFAULT_ROUTE_KMH  50.0
#define KMH_PER_MS         3.6
#define GPX_BYTES_MAX      (32u << 20)
#define PI                 3.14159265358979323846

static double radians(double degrees) {
    return degrees * PI / 180.0;
}

static double degrees(double radians_value) {
    return radians_value * 180.0 / PI;
}

static double distance_m(const gps_point_t *from, const gps_point_t *to) {
    double phi1 = radians(from->latitude), phi2 = radians(to->latitude);
    double dphi = phi2 - phi1, dlambda = radians(to->longitude - from->longitude);
    double a = sin(dphi / 2) * sin(dphi / 2) + cos(phi1) * cos(phi2) * sin(dlambda / 2) * sin(dlambda / 2);
    return 2 * EARTH_RADIUS_M * atan2(sqrt(a), sqrt(1 - a));
}

static double bearing(const gps_point_t *from, const gps_point_t *to) {
    double phi1 = radians(from->latitude), phi2 = radians(to->latitude), dlambda = radians(to->longitude - from->longitude);
    double y = sin(dlambda) * cos(phi2), x = cos(phi1) * sin(phi2) - sin(phi1) * cos(phi2) * cos(dlambda);
    return fmod(degrees(atan2(y, x)) + 360.0, 360.0);
}

static gps_point_t travel(const gps_point_t *from, double heading, double metres) {
    double delta = metres / EARTH_RADIUS_M, theta = radians(heading), phi1 = radians(from->latitude), lambda1 = radians(from->longitude);
    double phi2 = asin(sin(phi1) * cos(delta) + cos(phi1) * sin(delta) * cos(theta));
    double lambda2 = lambda1 + atan2(sin(theta) * sin(delta) * cos(phi1), cos(delta) - sin(phi1) * sin(phi2));
    gps_point_t to = *from;
    to.latitude = degrees(phi2);
    to.longitude = fmod(degrees(lambda2) + 540.0, 360.0) - 180.0;
    return to;
}

void gps_receiver_clear(gps_receiver_t *receiver) {
    memset(receiver, 0, sizeof *receiver);
}

void gps_receiver_set_position(gps_receiver_t *receiver, double latitude, double longitude, double altitude, double speed_kmh, double heading) {
    gps_receiver_clear(receiver);
    receiver->points[0] = (gps_point_t){ latitude, longitude, altitude, 0 };
    receiver->count = 1;
    receiver->speed_kmh = speed_kmh;
    receiver->heading = heading;
    receiver->active = true;
}

bool gps_receiver_parse_position(gps_receiver_t *receiver, const char *text) {
    double values[5] = { 0, 0, 0, 0, 0 };
    int count = 0;
    const char *cursor = text;
    while (count < 5) {
        char *end;
        values[count] = strtod(cursor, &end);
        if (end == cursor) return false;
        count++;
        cursor = end;
        if (*cursor != ',') break;
        cursor++;
    }
    if (*cursor || count < 2 || count == 4 || fabs(values[0]) > 90 || fabs(values[1]) > 180 || values[3] < 0) return false;
    gps_receiver_set_position(receiver, values[0], values[1], values[2], values[3], values[4]);
    return true;
}

static char *read_file(const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file) return NULL;
    char *text = NULL;
    if (fseek(file, 0, SEEK_END) == 0) {
        long size = ftell(file);
        if (size > 0 && (unsigned long)size < GPX_BYTES_MAX && fseek(file, 0, SEEK_SET) == 0 && (text = malloc((size_t)size + 1))) {
            if (fread(text, 1, (size_t)size, file) == (size_t)size) {
                text[size] = 0;
            } else {
                free(text);
                text = NULL;
            }
        }
    }
    fclose(file);
    return text;
}

static bool attribute(const char *tag, const char *tag_end, const char *name, double *value) {
    size_t length = strlen(name);
    for (const char *at = tag; at + length + 2 < tag_end; at++) {
        if (strncmp(at, name, length) || (at[-1] != ' ' && at[-1] != '\t' && at[-1] != '\n' && at[-1] != '\r') || at[length] != '=') continue;
        char quote = at[length + 1];
        if (quote != '"' && quote != '\'') return false;
        char *end;
        *value = strtod(at + length + 2, &end);
        return *end == quote;
    }
    return false;
}

static const char *element_text(const char *from, const char *to, const char *name) {
    char open[16];
    snprintf(open, sizeof open, "<%s>", name);
    const char *at = strstr(from, open);
    return at && at < to ? at + strlen(open) : NULL;
}

static bool parse_time(const char *text, double *seconds) {
    struct tm parts = { 0 };
    double fraction = 0;
    int year, month, day, hour, minute;
    double second;
    if (sscanf(text, "%d-%d-%dT%d:%d:%lf", &year, &month, &day, &hour, &minute, &second) != 6) return false;
    parts.tm_year = year - 1900;
    parts.tm_mon = month - 1;
    parts.tm_mday = day;
    parts.tm_hour = hour;
    parts.tm_min = minute;
    parts.tm_sec = (int)second;
    fraction = second - (int)second;
    *seconds = (double)timegm(&parts) + fraction;
    return true;
}

static const char *next_point(const char *text, const char **tag_end) {
    static const char *const names[] = { "<trkpt", "<rtept", "<wpt" };
    const char *best = NULL;
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        const char *at = strstr(text, names[i]);
        if (at && (!best || at < best)) best = at;
    }
    if (best) *tag_end = strchr(best, '>');
    return best && *tag_end ? best : NULL;
}

bool gps_receiver_load_gpx(gps_receiver_t *receiver, const char *path) {
    char *text = read_file(path);
    if (!text) return false;
    gps_receiver_t loaded;
    gps_receiver_clear(&loaded);
    bool timed = true;
    const char *cursor = text, *tag_end;
    const char *tag;
    while ((tag = next_point(cursor, &tag_end)) && loaded.count < GPS_ROUTE_POINTS_MAX) {
        const char *next_end;
        const char *after = next_point(tag_end, &next_end);
        const char *limit = after ? after : tag_end + strlen(tag_end);
        gps_point_t point = { 0 };
        if (attribute(tag, tag_end, "lat", &point.latitude) && attribute(tag, tag_end, "lon", &point.longitude)) {
            const char *elevation = element_text(tag_end, limit, "ele");
            if (elevation) point.altitude = strtod(elevation, NULL);
            const char *stamp = element_text(tag_end, limit, "time");
            if (!stamp || !parse_time(stamp, &point.seconds)) timed = false;
            loaded.points[loaded.count++] = point;
        }
        cursor = tag_end + 1;
    }
    free(text);
    if (!loaded.count) return false;
    double start = loaded.points[0].seconds;
    for (uint32_t i = 0; i < loaded.count; i++) {
        if (timed) {
            loaded.points[i].seconds -= start;
            if (i && loaded.points[i].seconds <= loaded.points[i - 1].seconds) timed = false;
        }
    }
    if (!timed) {
        loaded.points[0].seconds = 0;
        for (uint32_t i = 1; i < loaded.count; i++) {
            loaded.points[i].seconds = loaded.points[i - 1].seconds + distance_m(&loaded.points[i - 1], &loaded.points[i]) * KMH_PER_MS / DEFAULT_ROUTE_KMH;
        }
    }
    loaded.active = true;
    *receiver = loaded;
    return true;
}

void gps_receiver_advance(gps_receiver_t *receiver, double seconds) {
    if (receiver->active) receiver->elapsed += seconds;
}

static void position_now(const gps_receiver_t *receiver, gps_point_t *where, double *speed_kmh, double *heading) {
    const gps_point_t *points = receiver->points;
    if (receiver->count == 1) {
        *speed_kmh = receiver->speed_kmh;
        *heading = receiver->heading;
        *where = travel(&points[0], receiver->heading, receiver->speed_kmh / KMH_PER_MS * receiver->elapsed);
        where->altitude = points[0].altitude;
        return;
    }
    uint32_t segment = 0;
    while (segment + 2 < receiver->count && receiver->elapsed >= points[segment + 1].seconds) segment++;
    const gps_point_t *from = &points[segment], *to = &points[segment + 1];
    double span = to->seconds - from->seconds;
    double progress = span > 0 ? (receiver->elapsed - from->seconds) / span : 1;
    if (progress < 0) progress = 0;
    double metres = distance_m(from, to);
    *heading = bearing(from, to);
    *speed_kmh = progress < 1 && span > 0 ? metres / span * KMH_PER_MS : 0;
    if (progress > 1) progress = 1;
    *where = travel(from, *heading, metres * progress);
    where->altitude = from->altitude + (to->altitude - from->altitude) * progress;
}

void gps_receiver_fix(const gps_receiver_t *receiver, int64_t utc_seconds, clarion_gps_fix_t *fix) {
    memset(fix, 0, sizeof *fix);
    time_t seconds = (time_t)utc_seconds;
    struct tm parts;
    gmtime_r(&seconds, &parts);
    fix->year = parts.tm_year + 1900;
    fix->month = parts.tm_mon + 1;
    fix->day = parts.tm_mday;
    fix->hour = parts.tm_hour;
    fix->minute = parts.tm_min;
    fix->second = parts.tm_sec;
    if (!receiver->active || !receiver->count) return;
    gps_point_t where;
    position_now(receiver, &where, &fix->speed_kmh, &fix->heading);
    fix->latitude = where.latitude;
    fix->longitude = where.longitude;
    fix->altitude = where.altitude;
    fix->valid = true;
}
