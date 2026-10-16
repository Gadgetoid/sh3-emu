/* Fetch an internet MP3 audio stream on a background thread and decode it to 44.1 kHz stereo PCM. */
#include "net/radio_stream.h"

#include <curl/curl.h>
#include <stdio.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "vendor/minimp3.h"

#define RING_FRAMES      (RADIO_STREAM_RATE * 4)
#define INPUT_BYTES      (64 * 1024)
#define DECODE_THRESHOLD (16 * 1024)
#define RETRY_SECONDS    3
#define CONNECT_SECONDS  10L
#define URL_BYTES        1024
#define AGENT_BYTES      256
#define SOURCES_MAX      8

struct radio_stream {
    pthread_t thread;
    pthread_mutex_t lock;
    volatile int stopping;
    char url[URL_BYTES];
    char sources[SOURCES_MAX][URL_BYTES];
    int source_count, source_index;
    bool sniffed, playlist;
    char user_agent[AGENT_BYTES];
    mp3dec_t decoder;
    uint8_t input[INPUT_BYTES];
    size_t input_length;
    int16_t ring[RING_FRAMES * 2];
    size_t head, count;
    double phase;
    int16_t last[2];
};

bool radio_stream_supported(void) {
    return true;
}

static void push_frame(radio_stream_t *stream, int16_t left, int16_t right) {
    size_t tail = (stream->head + stream->count) % RING_FRAMES;
    stream->ring[2 * tail] = left;
    stream->ring[2 * tail + 1] = right;
    if (stream->count < RING_FRAMES) stream->count++;
    else stream->head = (stream->head + 1) % RING_FRAMES;
}

static void push_pcm(radio_stream_t *stream, const int16_t *pcm, int samples, int channels, int hz) {
    double step = (double)hz / RADIO_STREAM_RATE;
    pthread_mutex_lock(&stream->lock);
    for (int i = 0; i < samples; i++) {
        int16_t left = pcm[i * channels], right = pcm[i * channels + (channels > 1)];
        while (stream->phase < 1.0) {
            double t = stream->phase;
            push_frame(stream, (int16_t)(stream->last[0] + (left - stream->last[0]) * t), (int16_t)(stream->last[1] + (right - stream->last[1]) * t));
            stream->phase += step;
        }
        stream->phase -= 1.0;
        stream->last[0] = left;
        stream->last[1] = right;
    }
    pthread_mutex_unlock(&stream->lock);
}

static void decode_input(radio_stream_t *stream, bool flush) {
    int16_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
    size_t offset = 0;
    while (stream->input_length - offset > (flush ? 0 : DECODE_THRESHOLD)) {
        mp3dec_frame_info_t info;
        int samples = mp3dec_decode_frame(&stream->decoder, stream->input + offset, (int)(stream->input_length - offset), pcm, &info);
        if (!info.frame_bytes) break;
        offset += (size_t)info.frame_bytes;
        if (samples > 0 && info.hz > 0 && info.channels > 0) push_pcm(stream, pcm, samples, info.channels, info.hz);
    }
    memmove(stream->input, stream->input + offset, stream->input_length - offset);
    stream->input_length -= offset;
}

static bool starts_with(const char *data, size_t length, const char *prefix) {
    size_t prefix_length = strlen(prefix);
    return length >= prefix_length && !strncasecmp(data, prefix, prefix_length);
}

static void add_source(radio_stream_t *stream, const char *url, size_t length) {
    if (stream->source_count == SOURCES_MAX || length == 0 || length >= URL_BYTES) return;
    memcpy(stream->sources[stream->source_count], url, length);
    stream->sources[stream->source_count++][length] = 0;
}

static void parse_playlist(radio_stream_t *stream) {
    char *text = (char *)stream->input;
    size_t length = stream->input_length < INPUT_BYTES ? stream->input_length : INPUT_BYTES - 1;
    text[length] = 0;
    stream->source_count = 0;
    for (char *line = strtok(text, "\r\n"); line; line = strtok(NULL, "\r\n")) {
        while (*line == ' ' || *line == '\t') line++;
        if (!strncasecmp(line, "file", 4) && strchr(line, '=')) line = strchr(line, '=') + 1;
        if (starts_with(line, strlen(line), "http://") || starts_with(line, strlen(line), "https://")) add_source(stream, line, strcspn(line, " \t"));
    }
    stream->source_index = 0;
}

static size_t collect_playlist(radio_stream_t *stream, const char *data, size_t bytes) {
    size_t room = INPUT_BYTES - 1 - stream->input_length;
    size_t take = bytes < room ? bytes : room;
    memcpy(stream->input + stream->input_length, data, take);
    stream->input_length += take;
    return bytes;
}

static size_t received(char *data, size_t size, size_t count, void *context) {
    radio_stream_t *stream = context;
    size_t bytes = size * count, used = 0;
    if (stream->stopping) return 0;
    if (!stream->sniffed) {
        stream->sniffed = true;
        stream->playlist = starts_with(data, bytes, "#EXTM3U") || starts_with(data, bytes, "[playlist]") || starts_with(data, bytes, "http://") ||
                           starts_with(data, bytes, "https://");
    }
    if (stream->playlist) return collect_playlist(stream, data, bytes);
    while (used < bytes) {
        size_t room = INPUT_BYTES - stream->input_length;
        size_t take = bytes - used < room ? bytes - used : room;
        memcpy(stream->input + stream->input_length, data + used, take);
        stream->input_length += take;
        used += take;
        decode_input(stream, false);
        if (stream->input_length == INPUT_BYTES) stream->input_length = 0;
    }
    return bytes;
}

static int progress(void *context, curl_off_t download_total, curl_off_t downloaded, curl_off_t upload_total, curl_off_t uploaded) {
    (void)download_total;
    (void)downloaded;
    (void)upload_total;
    (void)uploaded;
    return ((radio_stream_t *)context)->stopping != 0;
}

static const char *current_source(radio_stream_t *stream) {
    return stream->source_count ? stream->sources[stream->source_index % stream->source_count] : stream->url;
}

static void *run(void *context) {
    radio_stream_t *stream = context;
    while (!stream->stopping) {
        CURL *curl = curl_easy_init();
        bool was_playlist = false;
        if (curl) {
            mp3dec_init(&stream->decoder);
            stream->input_length = 0;
            stream->sniffed = stream->playlist = false;
            curl_easy_setopt(curl, CURLOPT_URL, current_source(stream));
            curl_easy_setopt(curl, CURLOPT_USERAGENT, stream->user_agent);
            curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
            curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, CONNECT_SECONDS);
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, received);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, stream);
            curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress);
            curl_easy_setopt(curl, CURLOPT_XFERINFODATA, stream);
            curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
            curl_easy_perform(curl);
            curl_easy_cleanup(curl);
            if (stream->playlist) {
                parse_playlist(stream);
                was_playlist = stream->source_count > 0;
            } else if (stream->source_count) {
                stream->source_index++;
            }
        }
        if (was_playlist) continue;
        for (int i = 0; i < RETRY_SECONDS * 10 && !stream->stopping; i++) usleep(100000);
    }
    return NULL;
}

radio_stream_t *radio_stream_open(const char *url, const char *user_agent) {
    radio_stream_t *stream = calloc(1, sizeof *stream);
    if (!stream) return NULL;
    snprintf(stream->url, sizeof stream->url, "%s", url);
    snprintf(stream->user_agent, sizeof stream->user_agent, "%s", user_agent && user_agent[0] ? user_agent : "sh3-emu");
    pthread_mutex_init(&stream->lock, NULL);
    if (pthread_create(&stream->thread, NULL, run, stream) != 0) {
        pthread_mutex_destroy(&stream->lock);
        free(stream);
        return NULL;
    }
    return stream;
}

size_t radio_stream_read(radio_stream_t *stream, int16_t *stereo, size_t frames) {
    size_t got;
    pthread_mutex_lock(&stream->lock);
    got = frames < stream->count ? frames : stream->count;
    for (size_t i = 0; i < got; i++) {
        stereo[2 * i] = stream->ring[2 * stream->head];
        stereo[2 * i + 1] = stream->ring[2 * stream->head + 1];
        stream->head = (stream->head + 1) % RING_FRAMES;
    }
    stream->count -= got;
    pthread_mutex_unlock(&stream->lock);
    return got;
}

void radio_stream_close(radio_stream_t *stream) {
    if (!stream) return;
    stream->stopping = 1;
    pthread_join(stream->thread, NULL);
    pthread_mutex_destroy(&stream->lock);
    free(stream);
}
