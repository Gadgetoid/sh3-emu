#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/cfcard.h"
#include "core/ne2000.h"
#include "core/sh770x.h"

#define HP320LX_SCREEN_WIDTH  640
#define HP320LX_SCREEN_HEIGHT 240
#define HP320LX_REGISTERS     4096
#define HP320LX_FRAMEBUFFER   0x0C005000u
#define HP300LX_FRAMEBUFFER   0x0C004800u
#define HP320LX_ADC_HEALTHY   0x3A0u
#define HP320LX_BACKLIGHT_COLOUR 0x41B432u
#define HP320LX_PERIPHERAL_HZ 11059200u
#define HP320LX_MODEL_PORT    0xA4000134u
#define HP320LX_MODEL_PINS    0x0032u
#define HP300LX_MODEL_PINS    0x0012u
#define HP320LX_POWER_PORT    0xA4000126u
#define HP320LX_POWER_AC      0x0010u
#define HP320LX_SERIAL_PORT   0xA400012Eu
#define HP320LX_SERIAL_NO_CABLE 0x0004u
#define HP320LX_SERIAL_SCIF   2
#define HP320LX_SERIAL_IRQ    2
#define HP320LX_SERIAL_CONTROL_PORT 0x12
#define HP320LX_SERIAL_DTR    0x0004u
#define HP320LX_BACKLIGHT_PORT 0x12
#define HP320LX_BACKLIGHT_OFF  0x0008u
#define HP320LX_KEY_COLUMNS_LOW  0xA4000120u
#define HP320LX_KEY_COLUMNS_HIGH 0xA4000122u
#define HP320LX_KEY_ROWS      8
#define HP320LX_KEY_COLUMNS   11
#define HP320LX_KEY_EVENTS    64
#define HP320LX_PEN_IRQ       3
#define HP320LX_PC_CARD_SOCKET 0
#define HP320LX_CF_SOCKET     1
#define HP320LX_CARD_PRESENT  0x23u
#define HP320LX_CARD_RDY_PIN  0x80u
#define HP320LX_CARD_EMPTY    0xFFu
#define HP320LX_LCD_FRAME_LOW  0
#define HP320LX_LCD_FRAME_HIGH 1
#define HP320LX_LCD_MODE       4
#define HP320LX_ON_IRQ        0
#define HP320LX_SOUND_DAC     1

typedef enum { HP_MODEL_NONE, HP_MODEL_320LX, HP_MODEL_300LX } hp_model_t;

typedef void (*hp320lx_trace_fn)(void *context, bool write, uint32_t pa, int size, uint32_t value);
typedef void (*hp320lx_line_fn)(void *context, const char *line);
typedef void (*hp320lx_sample_fn)(void *context, int16_t sample);

typedef struct {
    uint64_t audio_clock, audio_last;
    int16_t audio_level;
    bool on_key;
} hp320lx_runtime_t;

typedef struct {
    hp320lx_trace_fn trace;
    hp320lx_line_fn debug_line;
    hp320lx_sample_fn sample;
    hp_model_t model;
    uint32_t cpu_hz, sample_rate;
    sh770x_t *chip;
    cfcard_slot_t *card;
    ne2000_t *ethernet;
    hp320lx_runtime_t *runtime;
    void            *context;
} hp320lx_host_t;

typedef struct {
    uint8_t row, column;
    bool up;
} hp320lx_key_event_t;

typedef struct {
    uint32_t address[HP320LX_REGISTERS];
    uint32_t value[HP320LX_REGISTERS];
    uint32_t count;
    char line[256];
    int line_length;
    uint16_t keys_down[HP320LX_KEY_ROWS];
    hp320lx_key_event_t key_events[HP320LX_KEY_EVENTS];
    uint32_t key_event_head, key_event_count;
    uint32_t scans, key_changed_scan;
    uint8_t rows_driven;
    bool pen_down;
    uint16_t pen_x, pen_y;
} hp320lx_t;

hp_model_t hp320lx_detect(const uint8_t *image, size_t size);
void hp320lx_reset(hp320lx_t *board);
bool hp320lx_read(hp320lx_t *board, const hp320lx_host_t *host, uint32_t pa, int size, uint32_t *value);
bool hp320lx_write(hp320lx_t *board, const hp320lx_host_t *host, uint32_t pa, int size, uint32_t value);
void hp320lx_power_on(hp320lx_t *board, const hp320lx_host_t *host);
void hp320lx_ports_written(hp320lx_t *board, const hp320lx_host_t *host);
void hp320lx_dac_written(const hp320lx_host_t *host, int channel, uint8_t value, uint64_t cycle);
bool hp320lx_card_access(const hp320lx_host_t *host, uint32_t pa, int size, bool write, uint32_t *value);
void hp320lx_cards_changed(const hp320lx_host_t *host);
void hp320lx_key(hp320lx_t *board, const hp320lx_host_t *host, uint8_t scancode, bool up, bool suspended);
void hp320lx_touch(hp320lx_t *board, const hp320lx_host_t *host, bool down, int x, int y);
void hp320lx_power_button(hp320lx_t *board, const hp320lx_host_t *host, bool down, bool suspended);
bool hp320lx_serial_dtr(const hp320lx_host_t *host);
void hp320lx_screen(const uint8_t *framebuffer, const uint16_t *palette, uint16_t mode, uint8_t *levels);
