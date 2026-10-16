/* Report that internet radio streams aren't available in a build without libcurl. */
#include "net/radio_stream.h"

bool radio_stream_supported(void) {
    return false;
}

radio_stream_t *radio_stream_open(const char *url, const char *user_agent) {
    (void)url;
    (void)user_agent;
    return NULL;
}

size_t radio_stream_read(radio_stream_t *stream, int16_t *stereo, size_t frames) {
    (void)stream;
    (void)stereo;
    (void)frames;
    return 0;
}

void radio_stream_close(radio_stream_t *stream) {
    (void)stream;
}
