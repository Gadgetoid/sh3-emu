#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/pccard.h"

#define HD64461_PA              0x10000000u
#define HD64461_REGISTERS       512
#define HD64461_VRAM_PA         0x12000000u
#define HD64461_VRAM_SIZE       0x80000u
#define HD64461_PALETTE         256
#define HD64461_SOCKETS         2
#define HD64461_UART_FIFO       16

typedef void (*hd64461_trace_fn)(void *context, bool write, uint32_t pa, int size, uint32_t value);
typedef void (*hd64461_transmit_fn)(void *context, uint8_t byte);
typedef void (*hd64461_interrupt_fn)(void *context, bool asserted);
typedef pccard_slot_t (*hd64461_socket_fn)(void *context, int socket);

typedef struct {
    hd64461_trace_fn trace;
    hd64461_transmit_fn transmit;
    hd64461_interrupt_fn interrupt;
    hd64461_socket_fn socket;
    void *context;
} hd64461_host_t;

typedef struct {
    uint8_t general, changes, enables, control, status;
    bool card_irq;
} hd64461_socket_t;

typedef struct {
    uint8_t ier, lcr, mcr, fcr, scratch, divisor_low, divisor_high;
    uint8_t modem, line_errors;
    bool transmit_empty;
    uint32_t sending;
    uint8_t received[HD64461_UART_FIFO];
    uint32_t received_head, received_count;
} hd64461_uart_t;

typedef struct {
    uint32_t address[HD64461_REGISTERS];
    uint32_t value[HD64461_REGISTERS];
    uint32_t count;
    uint16_t interrupt_mask;
    hd64461_socket_t sockets[HD64461_SOCKETS];
    hd64461_uart_t uart;
    uint16_t palette_write, palette_read;
    uint8_t palette[HD64461_PALETTE][3];
    uint8_t vram[HD64461_VRAM_SIZE];
} hd64461_t;

void hd64461_reset(hd64461_t *chip);
bool hd64461_read(hd64461_t *chip, const hd64461_host_t *host, uint32_t pa, int size, uint32_t *value);
bool hd64461_write(hd64461_t *chip, const hd64461_host_t *host, uint32_t pa, int size, uint32_t value);
bool hd64461_card_access(hd64461_t *chip, const hd64461_host_t *host, uint32_t pa, int size, bool write, uint32_t *value);
void hd64461_cards_changed(hd64461_t *chip, const hd64461_host_t *host);
void hd64461_uart_receive(hd64461_t *chip, const hd64461_host_t *host, uint8_t byte);
size_t hd64461_uart_room(const hd64461_t *chip);
bool hd64461_uart_sending(const hd64461_t *chip);
void hd64461_uart_drain(hd64461_t *chip, const hd64461_host_t *host, uint32_t bytes);
uint32_t hd64461_uart_baud(const hd64461_t *chip);
bool hd64461_uart_dtr(const hd64461_t *chip);
void hd64461_uart_connect(hd64461_t *chip, const hd64461_host_t *host, bool connected);
int  hd64461_palette(const hd64461_t *chip, uint32_t *palette);
