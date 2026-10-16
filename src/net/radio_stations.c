/* Map tuner frequencies to internet radio stream URLs, from built-in defaults or a stations file. */
#include "net/radio_stations.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LINE_BYTES 1024
#define FM_LIMIT_MHZ 200.0

typedef struct {
    uint32_t khz;
    const char *url, *name;
} default_station_t;

static const default_station_t defaults[] = {
    { 88500, "https://ice2.somafm.com/groovesalad-128-mp3", "Groove Salad" },
    { 90100, "https://ice2.somafm.com/dronezone-128-mp3", "Drone Zone" },
    { 93300, "https://ice2.somafm.com/u80s-128-mp3", "Underground 80s" },
    { 94900, "https://ice2.somafm.com/secretagent-128-mp3", "Secret Agent" },
    { 98700, "https://ice2.somafm.com/indiepop-128-mp3", "Indie Pop Rocks" },
    { 101100, "https://ice2.somafm.com/lush-128-mp3", "Lush" },
    { 104300, "https://ice2.somafm.com/beatblender-128-mp3", "Beat Blender" },
    { 106700, "https://ice2.somafm.com/spacestation-128-mp3", "Space Station" },
};

static void add(radio_stations_t *list, uint32_t khz, const char *url, const char *name) {
    if (list->count == RADIO_STATIONS_MAX) return;
    radio_station_t *station = &list->stations[list->count++];
    station->khz = khz;
    snprintf(station->url, sizeof station->url, "%s", url);
    snprintf(station->name, sizeof station->name, "%s", name ? name : "");
}

void radio_stations_default(radio_stations_t *list) {
    list->count = 0;
    for (size_t i = 0; i < sizeof defaults / sizeof defaults[0]; i++) add(list, defaults[i].khz, defaults[i].url, defaults[i].name);
}

static char *skip_space(char *text) {
    while (*text && isspace((unsigned char)*text)) text++;
    return text;
}

static char *next_word(char **text) {
    char *start = skip_space(*text), *end = start;
    while (*end && !isspace((unsigned char)*end)) end++;
    if (*end) *end++ = 0;
    *text = end;
    return start;
}

bool radio_stations_load(radio_stations_t *list, const char *path) {
    char expanded[LINE_BYTES];
    const char *home = getenv("HOME");
    if (path[0] == '~' && path[1] == '/' && home) {
        snprintf(expanded, sizeof expanded, "%s%s", home, path + 1);
        path = expanded;
    }
    FILE *file = fopen(path, "r");
    if (!file) return false;
    char line[LINE_BYTES];
    list->count = 0;
    while (fgets(line, sizeof line, file)) {
        line[strcspn(line, "\r\n")] = 0;
        char *cursor = skip_space(line);
        if (!*cursor || *cursor == '#') continue;
        char *end;
        double frequency = strtod(next_word(&cursor), &end);
        char *url = next_word(&cursor);
        if (frequency <= 0 || !*url) continue;
        uint32_t khz = frequency < FM_LIMIT_MHZ ? (uint32_t)(frequency * 1000.0 + 0.5) : (uint32_t)(frequency + 0.5);
        add(list, khz, url, skip_space(cursor));
    }
    fclose(file);
    return true;
}

const radio_station_t *radio_stations_find(const radio_stations_t *list, uint32_t khz) {
    for (size_t i = 0; i < list->count; i++) {
        if (list->stations[i].khz == khz) return &list->stations[i];
    }
    return NULL;
}
