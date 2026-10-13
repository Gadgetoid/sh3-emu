#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/cfcard.h"
#include "core/hd64461.h"
#include "core/ne2000.h"
#include "core/sh770x.h"

#define HPW200EC_SCREEN_WIDTH     640
#define HPW200EC_SCREEN_HEIGHT    240
#define HPW200EC_PERIPHERAL_HZ    24000000u
#define HPW200EC_REGISTERS        256
#define HPW200EC_KEY_ROWS         12
#define HPW200EC_COMPANION_IRQ    4
#define HPW200EC_SOUND_DAC        0

typedef void (*hpw200ec_trace_fn)(void *context, bool write, uint32_t pa, int size, uint32_t value);

typedef struct {
    hpw200ec_trace_fn trace;
    sh770x_t *chip;
    cfcard_slot_t *card;
    ne2000_t *ethernet;
    hd64461_host_t companion;
    void *context;
} hpw200ec_host_t;

typedef struct {
    uint32_t address[HPW200EC_REGISTERS];
    uint32_t value[HPW200EC_REGISTERS];
    uint32_t count;
    uint8_t keys_down[HPW200EC_KEY_ROWS];
    bool pen_down;
    uint16_t pen_x, pen_y;
    hd64461_t companion;
} hpw200ec_t;

bool hpw200ec_detect(const uint8_t *image, size_t size);
void hpw200ec_reset(hpw200ec_t *board);
void hpw200ec_power_on(hpw200ec_t *board, const hpw200ec_host_t *host);
bool hpw200ec_read(hpw200ec_t *board, const hpw200ec_host_t *host, uint32_t pa, int size, uint32_t *value);
bool hpw200ec_write(hpw200ec_t *board, const hpw200ec_host_t *host, uint32_t pa, int size, uint32_t value);
bool hpw200ec_card_access(hpw200ec_t *board, const hpw200ec_host_t *host, uint32_t pa, int size, bool write, uint32_t *value);
pccard_slot_t hpw200ec_socket(const hpw200ec_host_t *host, int socket);
void hpw200ec_cards_changed(hpw200ec_t *board, const hpw200ec_host_t *host);
void hpw200ec_ports_written(hpw200ec_t *board, const hpw200ec_host_t *host);
void hpw200ec_key(hpw200ec_t *board, const hpw200ec_host_t *host, uint8_t scancode, bool up);
void hpw200ec_touch(hpw200ec_t *board, const hpw200ec_host_t *host, bool down, int x, int y);
void hpw200ec_power_button(hpw200ec_t *board, const hpw200ec_host_t *host, bool down);
void hpw200ec_screen(const hpw200ec_t *board, uint8_t *levels);
int  hpw200ec_palette(const hpw200ec_t *board, uint32_t *palette);
