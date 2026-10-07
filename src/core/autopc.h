#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/cfcard.h"

#define AUTOPC_SCREEN_WIDTH  256
#define AUTOPC_SCREEN_HEIGHT 64
#define AUTOPC_PCI_BARS      2
#define AUTOPC_LINK_REPLIES  64
#define AUTOPC_KEY_ROWS      5
#define AUTOPC_SOCKETS       2

typedef void (*autopc_line_fn)(void *context, const char *line);
typedef void (*autopc_trace_fn)(void *context, bool write, uint32_t pa, int size, uint32_t value);
typedef void (*autopc_irl_fn)(void *context, bool asserted);

typedef struct {
    autopc_line_fn  debug_line;
    autopc_trace_fn trace;
    autopc_irl_fn   irl;
    void           *context;
    cfcard_slot_t  *card;
} autopc_host_t;

typedef struct {
    uint8_t  ier, lcr, mcr, scratch, dll, dlm;
    char     line[256];
    int      length;
} autopc_uart_t;

typedef struct {
    uint32_t size;
    bool     io;
} autopc_bar_t;

typedef struct {
    uint16_t vendor, device;
    uint32_t class_revision;
    autopc_bar_t bars[AUTOPC_PCI_BARS];
    uint32_t config[64];
} autopc_pci_t;

typedef struct {
    uint32_t fpga[0x4000];
    uint32_t bridge[128];
    autopc_uart_t debug_uart;

    autopc_pci_t faceplate;
    uint8_t  faceplate_io[0x10000];
    uint8_t  faceplate_memory[0x10000];
    uint32_t faceplate_enable, faceplate_pending;
    uint32_t link_address, link_control, link_received;
    uint32_t link_replies[AUTOPC_LINK_REPLIES];
    uint32_t link_reply_head, link_reply_count;

    uint8_t  lcd_registers[256];
    uint8_t  lcd_palette[256];
    uint8_t  lcd_index;
    uint32_t lcd_address;
    uint8_t  lcd_pixels[AUTOPC_SCREEN_WIDTH * AUTOPC_SCREEN_HEIGHT];
    uint8_t  faceplate_control;
    uint8_t  key_rows_selected;
    uint8_t  keys_down[AUTOPC_KEY_ROWS];

    uint16_t socket_control[AUTOPC_SOCKETS];
    uint16_t socket_events[AUTOPC_SOCKETS];
    uint16_t socket_shared;
} autopc_t;

bool autopc_detect(const uint8_t *image, size_t size);
void autopc_reset(autopc_t *board);
void autopc_prepare_ram(uint8_t *dram, uint32_t size);
bool autopc_read(autopc_t *board, const autopc_host_t *host, uint32_t pa, int size, uint32_t *value);
bool autopc_write(autopc_t *board, const autopc_host_t *host, uint32_t pa, int size, uint32_t value);
void autopc_screen(const autopc_t *board, uint8_t *pixels);
int  autopc_palette(uint32_t *palette);
void autopc_card_changed(autopc_t *board, const autopc_host_t *host);
void autopc_key(autopc_t *board, const autopc_host_t *host, uint8_t scancode, bool up);
