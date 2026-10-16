#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define CDIMAGE_TRACKS_MAX  99
#define CDIMAGE_DATA_BYTES  2048u
#define CDIMAGE_RAW_BYTES   2352u
#define CDIMAGE_CONTROL_DATA 0x04u

typedef struct {
    uint8_t number, control;
    uint32_t start;
} cdimage_track_t;

typedef struct {
    cdimage_track_t tracks[CDIMAGE_TRACKS_MAX];
    uint32_t track_count;
    uint32_t total_blocks;
    uint32_t sector_bytes;
} cdimage_t;

FILE *cdimage_open(const char *path, cdimage_t *disc);
void  cdimage_describe_iso(FILE *image, cdimage_t *disc);
const cdimage_track_t *cdimage_track_at(const cdimage_t *disc, uint32_t block);
bool  cdimage_read_data(FILE *image, const cdimage_t *disc, uint32_t block, uint8_t *out);
bool  cdimage_read_audio(FILE *image, const cdimage_t *disc, uint32_t block, uint8_t *out);
