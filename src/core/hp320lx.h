#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HP320LX_SCREEN_WIDTH  640
#define HP320LX_SCREEN_HEIGHT 240
#define HP320LX_REGISTERS     4096
#define HP320LX_FRAMEBUFFER   0x0C005000u
#define HP320LX_ADC_HEALTHY   0x3A0u
#define HP320LX_MODEL_PORT    0xA4000134u
#define HP320LX_MODEL_LUKE    0x0010u

typedef void (*hp320lx_trace_fn)(void *context, bool write, uint32_t pa, int size, uint32_t value);
typedef void (*hp320lx_line_fn)(void *context, const char *line);

typedef struct {
    hp320lx_trace_fn trace;
    hp320lx_line_fn  debug_line;
    void            *context;
} hp320lx_host_t;

typedef struct {
    uint32_t address[HP320LX_REGISTERS];
    uint32_t value[HP320LX_REGISTERS];
    uint32_t count;
    char     line[256];
    int      line_length;
} hp320lx_t;

bool hp320lx_detect(const uint8_t *image, size_t size);
void hp320lx_reset(hp320lx_t *board);
bool hp320lx_read(hp320lx_t *board, const hp320lx_host_t *host, uint32_t pa, int size, uint32_t *value);
bool hp320lx_write(hp320lx_t *board, const hp320lx_host_t *host, uint32_t pa, int size, uint32_t value);
void hp320lx_screen(const uint8_t *framebuffer, uint8_t *levels);
