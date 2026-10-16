/* Read data blocks and raw audio sectors from ISO 9660 and single-file BIN/CUE disc images. */
#include "core/cdimage.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define MODE1_DATA_OFFSET 16u
#define FRAMES_PER_SECOND 75u
#define PATH_BYTES        1024u
#define LINE_BYTES        512u

static long file_bytes(FILE *image) {
    if (!image || fseek(image, 0, SEEK_END) != 0) return 0;
    long bytes = ftell(image);
    return bytes > 0 ? bytes : 0;
}

void cdimage_describe_iso(FILE *image, cdimage_t *disc) {
    memset(disc, 0, sizeof *disc);
    disc->sector_bytes = CDIMAGE_DATA_BYTES;
    disc->total_blocks = (uint32_t)(file_bytes(image) / CDIMAGE_DATA_BYTES);
    disc->track_count = 1;
    disc->tracks[0] = (cdimage_track_t){ 1, CDIMAGE_CONTROL_DATA, 0 };
}

static bool has_extension(const char *path, const char *extension) {
    size_t length = strlen(path), suffix = strlen(extension);
    return length >= suffix && !strcasecmp(path + length - suffix, extension);
}

static const char *skip_space(const char *text) {
    while (*text && isspace((unsigned char)*text)) text++;
    return text;
}

static bool keyword(const char **text, const char *word) {
    size_t length = strlen(word);
    if (strncasecmp(*text, word, length) || ((*text)[length] && !isspace((unsigned char)(*text)[length]))) return false;
    *text = skip_space(*text + length);
    return true;
}

static bool parse_file_name(const char *text, const char *cue_path, char *path, size_t size) {
    char name[PATH_BYTES];
    size_t length = 0;
    if (*text == '"') {
        text++;
        while (*text && *text != '"' && length + 1 < sizeof name) name[length++] = *text++;
    } else {
        while (*text && !isspace((unsigned char)*text) && length + 1 < sizeof name) name[length++] = *text++;
    }
    name[length] = 0;
    if (!length) return false;
    const char *slash = strrchr(cue_path, '/');
    int written = name[0] == '/' || !slash ? snprintf(path, size, "%s", name) : snprintf(path, size, "%.*s/%s", (int)(slash - cue_path), cue_path, name);
    return written > 0 && (size_t)written < size;
}

static bool parse_track(const char *text, cdimage_t *disc, bool *raw, bool *cooked) {
    char *end;
    long number = strtol(text, &end, 10);
    if (end == text || number < 1 || number > CDIMAGE_TRACKS_MAX || disc->track_count >= CDIMAGE_TRACKS_MAX) return false;
    text = skip_space(end);
    uint8_t control;
    if (keyword(&text, "AUDIO")) {
        control = 0;
        *raw = true;
    } else if (keyword(&text, "MODE1/2352")) {
        control = CDIMAGE_CONTROL_DATA;
        *raw = true;
    } else if (keyword(&text, "MODE1/2048")) {
        control = CDIMAGE_CONTROL_DATA;
        *cooked = true;
    } else {
        return false;
    }
    disc->tracks[disc->track_count++] = (cdimage_track_t){ (uint8_t)number, control, UINT32_MAX };
    return true;
}

static bool parse_index(const char *text, cdimage_t *disc) {
    unsigned index, minutes, seconds, frames;
    if (!disc->track_count || sscanf(text, "%u %u:%u:%u", &index, &minutes, &seconds, &frames) != 4) return false;
    if (index == 1) disc->tracks[disc->track_count - 1].start = (minutes * 60 + seconds) * FRAMES_PER_SECOND + frames;
    return true;
}

static FILE *open_cue(const char *cue_path, cdimage_t *disc) {
    FILE *cue = fopen(cue_path, "r");
    if (!cue) return NULL;
    char line[LINE_BYTES], bin_path[PATH_BYTES] = { 0 };
    bool ok = true, raw = false, cooked = false;
    int files = 0;
    while (ok && fgets(line, sizeof line, cue)) {
        const char *text = skip_space(line);
        if (keyword(&text, "FILE")) ok = ++files == 1 && parse_file_name(text, cue_path, bin_path, sizeof bin_path);
        else if (keyword(&text, "TRACK")) ok = parse_track(text, disc, &raw, &cooked);
        else if (keyword(&text, "INDEX")) ok = parse_index(text, disc);
    }
    fclose(cue);
    if (!ok || !bin_path[0] || !disc->track_count || raw == cooked) return NULL;
    for (uint32_t i = 0; i < disc->track_count; i++) {
        if (disc->tracks[i].start == UINT32_MAX || (i && disc->tracks[i].start <= disc->tracks[i - 1].start)) return NULL;
    }
    FILE *image = fopen(bin_path, "rb");
    if (!image) return NULL;
    disc->sector_bytes = raw ? CDIMAGE_RAW_BYTES : CDIMAGE_DATA_BYTES;
    disc->total_blocks = (uint32_t)(file_bytes(image) / disc->sector_bytes);
    return image;
}

FILE *cdimage_open(const char *path, cdimage_t *disc) {
    memset(disc, 0, sizeof *disc);
    if (has_extension(path, ".cue")) return open_cue(path, disc);
    FILE *image = fopen(path, "rb");
    if (image) cdimage_describe_iso(image, disc);
    return image;
}

const cdimage_track_t *cdimage_track_at(const cdimage_t *disc, uint32_t block) {
    const cdimage_track_t *found = NULL;
    for (uint32_t i = 0; i < disc->track_count && disc->tracks[i].start <= block; i++) found = &disc->tracks[i];
    return found;
}

static bool read_sector(FILE *image, const cdimage_t *disc, uint32_t block, uint32_t offset, uint8_t *out, uint32_t bytes) {
    if (!image || block >= disc->total_blocks) return false;
    if (fseek(image, (long)block * (long)disc->sector_bytes + (long)offset, SEEK_SET) != 0) return false;
    return fread(out, 1, bytes, image) == bytes;
}

bool cdimage_read_data(FILE *image, const cdimage_t *disc, uint32_t block, uint8_t *out) {
    const cdimage_track_t *track = cdimage_track_at(disc, block);
    if (!track || !(track->control & CDIMAGE_CONTROL_DATA)) return false;
    uint32_t offset = disc->sector_bytes == CDIMAGE_RAW_BYTES ? MODE1_DATA_OFFSET : 0;
    return read_sector(image, disc, block, offset, out, CDIMAGE_DATA_BYTES);
}

bool cdimage_read_audio(FILE *image, const cdimage_t *disc, uint32_t block, uint8_t *out) {
    const cdimage_track_t *track = cdimage_track_at(disc, block);
    if (!track || (track->control & CDIMAGE_CONTROL_DATA) || disc->sector_bytes != CDIMAGE_RAW_BYTES) return false;
    return read_sector(image, disc, block, 0, out, CDIMAGE_RAW_BYTES);
}
