/* Emulate the Hitachi HD64461 companion chip: its LCD controller, PC Card controller, UART, interrupt controller and frame buffer. */
#include "core/hd64461.h"

#include <string.h>

#define REGISTER_SPACE    0x00010000u
#define LCDCCR            0x10001004u
#define LCDCCR_SPON       0x0010u
#define LCDCCR_EPON       0x0020u
#define LCDCCR_STREQ      0x0100u
#define LCDCCR_STBACK     0x0400u
#define CPTWAR            0x10001030u
#define CPTWDR            0x10001032u
#define CPTRAR            0x10001034u
#define CPTRDR            0x10001036u
#define PCC_BASE          0x10002000u
#define PCC_STRIDE        0x10u
#define PCC_ISR           0x0u
#define PCC_GCR           0x2u
#define PCC_CSCR          0x4u
#define PCC_CSCIER        0x6u
#define PCC_SCR           0x8u
#define PCC_EMPTY         0x3Cu
#define PCC_READY         0x80u
#define PCC_BATTERY_OK    0x03u
#define PCC_DETECT_CHANGE 0x08u
#define PCC_CHANGES       0x1Fu
#define PCC0_IREQ         0x20u
#define PCC0_IREQ_ENABLE  0x60u
#define PCC1_IREQ         0x04u
#define CODEC_COMMAND     0x10003200u
#define CODEC_BUSY        0x8000u
#define NIRR              0x10005000u
#define NIMR              0x10005002u
#define NIRR_UART         0x0020u
#define NIRR_PCC1         0x2000u
#define NIRR_PCC0         0x4000u
#define UART_PA           0x10008000u
#define UART_SIZE         0x10u
#define UART_DATA         0
#define UART_IER          1
#define UART_IIR          2
#define UART_LCR          3
#define UART_MCR          4
#define UART_LSR          5
#define UART_MSR          6
#define UART_SCRATCH      7
#define IER_RECEIVED      0x01u
#define IER_TRANSMIT      0x02u
#define IER_LINE          0x04u
#define IER_MODEM         0x08u
#define IIR_NONE          0x01u
#define IIR_MODEM         0x00u
#define IIR_TRANSMIT      0x02u
#define IIR_RECEIVED      0x04u
#define IIR_LINE          0x06u
#define IIR_FIFO          0xC0u
#define FCR_ENABLE        0x01u
#define FCR_CLEAR_RECEIVE 0x02u
#define LCR_DLAB          0x80u
#define MCR_DTR           0x01u
#define LSR_DATA          0x01u
#define LSR_OVERRUN       0x02u
#define LSR_EMPTY         0x60u
#define MSR_DELTAS        0x0Fu
#define MSR_DELTA_CTS     0x01u
#define MSR_DELTA_DSR     0x02u
#define MSR_DELTA_DCD     0x08u
#define MSR_CTS           0x10u
#define MSR_DSR           0x20u
#define MSR_DCD           0x80u
#define MSR_CONNECTED     (MSR_DCD | MSR_DSR | MSR_CTS)
#define UART_CLOCK_BAUD   115200u

void hd64461_reset(hd64461_t *chip) {
    memset(chip, 0, sizeof *chip);
    chip->interrupt_mask = 0xFFFFu;
    for (int socket = 0; socket < HD64461_SOCKETS; socket++) chip->sockets[socket].status = PCC_EMPTY;
}

static uint32_t *register_slot(hd64461_t *chip, uint32_t address, bool create) {
    for (uint32_t index = 0; index < chip->count; index++) {
        if (chip->address[index] == address) return &chip->value[index];
    }
    if (!create || chip->count == HD64461_REGISTERS) return NULL;
    chip->address[chip->count] = address;
    chip->value[chip->count] = 0;
    return &chip->value[chip->count++];
}

static uint8_t uart_identity(const hd64461_uart_t *uart) {
    uint8_t identity = IIR_NONE;
    if ((uart->ier & IER_LINE) && uart->line_errors) identity = IIR_LINE;
    else if ((uart->ier & IER_RECEIVED) && uart->received_count) identity = IIR_RECEIVED;
    else if ((uart->ier & IER_TRANSMIT) && uart->transmit_empty) identity = IIR_TRANSMIT;
    else if ((uart->ier & IER_MODEM) && (uart->modem & MSR_DELTAS)) identity = IIR_MODEM;
    return identity;
}

static bool socket_pending(const hd64461_socket_t *socket, int index) {
    if (socket->changes & socket->enables & PCC_CHANGES) return true;
    uint8_t enable = index == 0 ? PCC0_IREQ_ENABLE : PCC1_IREQ;
    return socket->card_irq && (socket->enables & enable);
}

static uint16_t interrupt_requests(const hd64461_t *chip) {
    uint16_t requests = 0;
    if (!(uart_identity(&chip->uart) & IIR_NONE)) requests |= NIRR_UART;
    if (socket_pending(&chip->sockets[0], 0)) requests |= NIRR_PCC0;
    if (socket_pending(&chip->sockets[1], 1)) requests |= NIRR_PCC1;
    return requests;
}

static void update_interrupt(const hd64461_t *chip, const hd64461_host_t *host) {
    if (host->interrupt) host->interrupt(host->context, (interrupt_requests(chip) & ~chip->interrupt_mask) != 0);
}

static pccard_slot_t socket_slot(const hd64461_host_t *host, int index) {
    pccard_slot_t empty = { NULL, NULL };
    return host->socket ? host->socket(host->context, index) : empty;
}

static void socket_lines(hd64461_t *chip, const hd64461_host_t *host, int index) {
    hd64461_socket_t *socket = &chip->sockets[index];
    pccard_slot_t slot = socket_slot(host, index);
    bool interrupt = pccard_interrupt(&slot);
    uint8_t status = PCC_EMPTY;
    if (pccard_present(&slot)) status = (uint8_t)(PCC_BATTERY_OK | (interrupt ? 0 : PCC_READY));
    if ((status ^ socket->status) & PCC_EMPTY) socket->changes |= PCC_DETECT_CHANGE;
    socket->status = status;
    socket->card_irq = interrupt;
}

void hd64461_cards_changed(hd64461_t *chip, const hd64461_host_t *host) {
    for (int index = 0; index < HD64461_SOCKETS; index++) socket_lines(chip, host, index);
    update_interrupt(chip, host);
}

bool hd64461_card_access(hd64461_t *chip, const hd64461_host_t *host, uint32_t pa, int size, bool write, uint32_t *value) {
    pccard_address_t address;
    if (!pccard_decode(pa, &address)) return false;
    int index = address.socket;
    pccard_slot_t slot = socket_slot(host, index);
    pccard_access(&slot, &address, size, write, value);
    socket_lines(chip, host, index);
    update_interrupt(chip, host);
    return true;
}

static uint32_t socket_read(const hd64461_socket_t *socket, int index, uint32_t offset) {
    switch (offset) {
    case PCC_ISR: return socket->status;
    case PCC_GCR: return socket->general;
    case PCC_CSCR: {
        uint8_t level = index == 0 ? PCC0_IREQ : PCC1_IREQ;
        return (uint32_t)(socket->changes | (socket->card_irq ? level : 0));
    }
    case PCC_CSCIER: return socket->enables;
    default: return socket->control;
    }
}

static void socket_write(hd64461_socket_t *socket, uint32_t offset, uint8_t value) {
    switch (offset) {
    case PCC_ISR: break;
    case PCC_GCR: socket->general = value; break;
    case PCC_CSCR: socket->changes &= value; break;
    case PCC_CSCIER: socket->enables = value; break;
    default: socket->control = value; break;
    }
}

static void uart_push(hd64461_uart_t *uart, uint8_t byte) {
    if (uart->received_count == HD64461_UART_FIFO) {
        uart->line_errors |= LSR_OVERRUN;
        return;
    }
    uart->received[(uart->received_head + uart->received_count) % HD64461_UART_FIFO] = byte;
    uart->received_count++;
}

static uint8_t uart_pop(hd64461_uart_t *uart) {
    if (!uart->received_count) return 0;
    uint8_t byte = uart->received[uart->received_head];
    uart->received_head = (uart->received_head + 1) % HD64461_UART_FIFO;
    uart->received_count--;
    return byte;
}

static uint32_t uart_read(hd64461_uart_t *uart, uint32_t index) {
    bool latch = uart->lcr & LCR_DLAB;
    switch (index) {
    case UART_DATA: return latch ? uart->divisor_low : uart_pop(uart);
    case UART_IER: return latch ? uart->divisor_high : uart->ier;
    case UART_IIR: {
        uint8_t identity = uart_identity(uart);
        if (identity == IIR_TRANSMIT) uart->transmit_empty = false;
        return identity | ((uart->fcr & FCR_ENABLE) ? IIR_FIFO : 0);
    }
    case UART_LCR: return uart->lcr;
    case UART_MCR: return uart->mcr;
    case UART_LSR: {
        uint8_t status = (uint8_t)((uart->sending ? 0 : LSR_EMPTY) | uart->line_errors | (uart->received_count ? LSR_DATA : 0));
        uart->line_errors = 0;
        return status;
    }
    case UART_MSR: {
        uint8_t status = uart->modem;
        uart->modem &= (uint8_t) ~MSR_DELTAS;
        return status;
    }
    default: return uart->scratch;
    }
}

static void uart_write(hd64461_uart_t *uart, const hd64461_host_t *host, uint32_t index, uint8_t value) {
    bool latch = uart->lcr & LCR_DLAB;
    switch (index) {
    case UART_DATA:
        if (latch) {
            uart->divisor_low = value;
            break;
        }
        if (host->transmit) host->transmit(host->context, value);
        if (uart->sending < HD64461_UART_FIFO) uart->sending++;
        uart->transmit_empty = false;
        break;
    case UART_IER:
        if (latch) {
            uart->divisor_high = value;
            break;
        }
        if ((value & IER_TRANSMIT) && !(uart->ier & IER_TRANSMIT) && !uart->sending) uart->transmit_empty = true;
        uart->ier = value & 0x0Fu;
        break;
    case UART_IIR:
        uart->fcr = value;
        if (value & FCR_CLEAR_RECEIVE) uart->received_count = 0;
        break;
    case UART_LCR: uart->lcr = value; break;
    case UART_MCR: uart->mcr = value; break;
    case UART_SCRATCH: uart->scratch = value; break;
    default: break;
    }
}

void hd64461_uart_receive(hd64461_t *chip, const hd64461_host_t *host, uint8_t byte) {
    uart_push(&chip->uart, byte);
    update_interrupt(chip, host);
}

size_t hd64461_uart_room(const hd64461_t *chip) {
    return HD64461_UART_FIFO - chip->uart.received_count;
}

bool hd64461_uart_sending(const hd64461_t *chip) {
    return chip->uart.sending != 0;
}

void hd64461_uart_drain(hd64461_t *chip, const hd64461_host_t *host, uint32_t bytes) {
    hd64461_uart_t *uart = &chip->uart;
    if (!uart->sending) return;
    uart->sending = bytes >= uart->sending ? 0 : uart->sending - bytes;
    if (uart->sending) return;
    uart->transmit_empty = true;
    update_interrupt(chip, host);
}

uint32_t hd64461_uart_baud(const hd64461_t *chip) {
    uint32_t divisor = (uint32_t)chip->uart.divisor_high << 8 | chip->uart.divisor_low;
    return divisor ? UART_CLOCK_BAUD / divisor : 0;
}

bool hd64461_uart_dtr(const hd64461_t *chip) {
    return (chip->uart.mcr & MCR_DTR) != 0;
}

void hd64461_uart_connect(hd64461_t *chip, const hd64461_host_t *host, bool connected) {
    hd64461_uart_t *uart = &chip->uart;
    uint8_t lines = connected ? MSR_CONNECTED : 0;
    uint8_t changed = (uint8_t)((uart->modem ^ lines) & MSR_CONNECTED);
    uint8_t deltas = (uint8_t)(((changed & MSR_DCD) ? MSR_DELTA_DCD : 0) | ((changed & MSR_DSR) ? MSR_DELTA_DSR : 0) | ((changed & MSR_CTS) ? MSR_DELTA_CTS : 0));
    uart->modem = (uint8_t)(lines | (uart->modem & MSR_DELTAS) | deltas);
    update_interrupt(chip, host);
}

static uint32_t read_bytes(const uint8_t *base, int size) {
    uint32_t value = 0;
    for (int i = 0; i < size; i++) value |= (uint32_t)base[i] << (8 * i);
    return value;
}

bool hd64461_read(hd64461_t *chip, const hd64461_host_t *host, uint32_t pa, int size, uint32_t *value) {
    if (pa - HD64461_VRAM_PA < HD64461_VRAM_SIZE) {
        *value = read_bytes(chip->vram + (pa - HD64461_VRAM_PA), size);
        return true;
    }
    if (pa - HD64461_PA >= REGISTER_SPACE) return false;
    if (pa - UART_PA < UART_SIZE) {
        *value = uart_read(&chip->uart, (pa - UART_PA) / 2);
        update_interrupt(chip, host);
        return true;
    }
    if (pa - PCC_BASE < PCC_STRIDE * HD64461_SOCKETS && (pa - PCC_BASE) % PCC_STRIDE <= PCC_SCR) {
        int index = (int)((pa - PCC_BASE) / PCC_STRIDE);
        *value = socket_read(&chip->sockets[index], index, (pa - PCC_BASE) % PCC_STRIDE);
        return true;
    }
    switch (pa) {
    case NIRR: *value = interrupt_requests(chip); return true;
    case NIMR: *value = chip->interrupt_mask; return true;
    case CPTRDR: {
        uint32_t entry = chip->palette_read / 3, component = chip->palette_read % 3;
        *value = chip->palette[entry % HD64461_PALETTE][component];
        chip->palette_read = (uint16_t)((chip->palette_read + 1) % (HD64461_PALETTE * 3));
        return true;
    }
    default: break;
    }
    uint32_t *slot = register_slot(chip, pa, false);
    uint32_t mask = size == 1 ? 0xFFu : size == 2 ? 0xFFFFu : 0xFFFFFFFFu;
    *value = slot ? *slot & mask : 0;
    if (host->trace) host->trace(host->context, false, pa, size, *value);
    return true;
}

bool hd64461_write(hd64461_t *chip, const hd64461_host_t *host, uint32_t pa, int size, uint32_t value) {
    if (pa - HD64461_VRAM_PA < HD64461_VRAM_SIZE) {
        uint8_t *base = chip->vram + (pa - HD64461_VRAM_PA);
        for (int i = 0; i < size; i++) base[i] = (uint8_t)(value >> (8 * i));
        return true;
    }
    if (pa - HD64461_PA >= REGISTER_SPACE) return false;
    if (pa - UART_PA < UART_SIZE) {
        uart_write(&chip->uart, host, (pa - UART_PA) / 2, (uint8_t)value);
        update_interrupt(chip, host);
        return true;
    }
    if (pa - PCC_BASE < PCC_STRIDE * HD64461_SOCKETS && (pa - PCC_BASE) % PCC_STRIDE <= PCC_SCR) {
        int index = (int)((pa - PCC_BASE) / PCC_STRIDE);
        socket_write(&chip->sockets[index], (pa - PCC_BASE) % PCC_STRIDE, (uint8_t)value);
        update_interrupt(chip, host);
        return true;
    }
    switch (pa) {
    case NIRR: return true;
    case NIMR:
        chip->interrupt_mask = (uint16_t)value;
        update_interrupt(chip, host);
        return true;
    case CPTWAR: chip->palette_write = (uint16_t)((value & 0xFF) * 3); return true;
    case CPTRAR: chip->palette_read = (uint16_t)((value & 0xFF) * 3); return true;
    case CPTWDR: {
        uint32_t entry = chip->palette_write / 3, component = chip->palette_write % 3;
        chip->palette[entry % HD64461_PALETTE][component] = (uint8_t)(value & 0x3F);
        chip->palette_write = (uint16_t)((chip->palette_write + 1) % (HD64461_PALETTE * 3));
        return true;
    }
    case CODEC_COMMAND: value &= ~CODEC_BUSY; break;
    case LCDCCR:
        value &= ~(uint32_t)(LCDCCR_EPON | LCDCCR_STBACK);
        if (value & LCDCCR_SPON) value |= LCDCCR_EPON;
        if (value & LCDCCR_STREQ) value |= LCDCCR_STBACK;
        break;
    default: break;
    }
    uint32_t *slot = register_slot(chip, pa, true);
    uint32_t mask = size == 1 ? 0xFFu : size == 2 ? 0xFFFFu : 0xFFFFFFFFu;
    if (slot) *slot = value & mask;
    if (host->trace) host->trace(host->context, true, pa, size, value);
    return true;
}

static uint32_t scale_component(uint8_t value) {
    return (uint32_t)(value << 2 | value >> 4);
}

int hd64461_palette(const hd64461_t *chip, uint32_t *palette) {
    for (uint32_t entry = 0; entry < HD64461_PALETTE; entry++) {
        const uint8_t *colour = chip->palette[entry];
        palette[entry] = scale_component(colour[2]) << 16 | scale_component(colour[1]) << 8 | scale_component(colour[0]);
    }
    return HD64461_PALETTE;
}
