#include "core/autopc.h"

#include <string.h>

#define BOOT_MAGIC_OFFSET 0x0001FFF0u
#define BOOTLOADER_DOWNLOAD 0xBA53E96Cu

#define FPGA_PA         0x11000000u
#define FPGA_SIZE       0x00010000u
#define FPGA_ENABLE     0x0008u
#define FPGA_PENDING    0x000Cu
#define FPGA_SOCKET_INTR 0x0400u
#define FPGA_INPUTS     0x1000u
#define INPUT_IGNITION  0x0001u
#define FPGA_STATUS     0x8000u
#define STATUS_POWER_ON 0x0200u

#define BRIDGE_PA       0x10000000u
#define BRIDGE_SIZE     0x00000200u
#define BRIDGE_STATUS   0x04u
#define BRIDGE_CONFIG_ADDRESS 0xACu
#define BRIDGE_DMA_STATUS 0x128u
#define MASTER_ABORT    0x20000000u
#define DMA_IDLE        0x00001010u
#define CONFIG_DATA_PA  0x0A000000u
#define CONFIG_ENABLE   0x80000000u
#define NO_DEVICE       0xFFFFFFFFu
#define PCI_MEMORY_PA   0x04000000u
#define PCI_IO_PA       0x08000000u
#define PCI_IO_WINDOWS  3u
#define PCI_IO_WINDOW_SIZE 0x01000000u

#define PCI_ID          0x00u
#define PCI_COMMAND     0x04u
#define PCI_CLASS       0x08u
#define PCI_BAR0        0x10u
#define COMMAND_IO      0x0001u
#define COMMAND_MEMORY  0x0002u
#define BAR_IO          0x00000001u

#define FACEPLATE_SLOT  1u
#define CLARION_VENDOR  0x1398u
#define FACEPLATE_DEVICE 0x0003u
#define FACEPLATE_CLASS 0x0B400000u
#define FACEPLATE_IO_SIZE 0x00010000u
#define FACEPLATE_MEMORY_SIZE 0x00010000u

#define FACEPLATE_ENABLE  0x1008u
#define FACEPLATE_PENDING 0x1010u
#define LINK_ADDRESS    0x5000u
#define LINK_RECEIVE    0x5004u
#define LINK_CONTROL    0x5008u
#define LINK_CONFIG     0x500Cu
#define LINK_OFFSET_MASK 0x000FFFFFu
#define LINK_TRANSMIT   0x00100000u
#define LINK_RX_READY   0x00000400u
#define LINK_TX_DONE    0x00000800u
#define FACEPLATE_MESSAGE 0x02000000u

#define WORD_CONTROL    0x80u
#define WORD_LCD_READ   0x40u
#define WORD_LCD        0xC0u
#define WORD_ADDRESS    0xD0u
#define WORD_PIXELS     0xE0u
#define WORD_KEY_READ   0x10u
#define WORD_KEY_ROWS   0x90u
#define WORD_DATA       0x00010000u

#define LCD_PALETTE_INDEX 0x0Eu
#define LCD_PALETTE_DATA  0x0Fu
#define PIXEL_BITS      3u
#define PIXEL_RED       4u
#define PIXEL_GREEN     2u
#define PIXEL_BLUE      1u
#define PIXELS_PER_WORD 8u
#define NIBBLES_PER_WORD 6u

#define SOCKET_PA       0x11800000u
#define SOCKET_STRIDE   0x1000u
#define SOCKET_CONTROL  0x0u
#define SOCKET_STATUS   0x4u
#define SOCKET_SHARED   0x2004u
#define SOCKET_SIZE     0x2008u
#define CONTROL_IREQ_ENABLE 0x0008u
#define CONTROL_CHANGE_ENABLE 0x0010u
#define CONTROL_RESET   0x0020u
#define STATUS_IREQ     0x0001u
#define STATUS_CHANGED  0x0002u
#define STATUS_NO_CARD  0x000Cu
#define SHARED_BATTERY_OK 0xCCCCu
#define CARD_PA         0x14000000u
#define CARD_END        0x1C000000u
#define CARD_SOCKET_SHIFT 23
#define CARD_OFFSET_MASK 0x007FFFFFu

#define DEBUG_UART_PA   0x10800000u
#define UART_STRIDE     4u
#define UART_SIZE       (8u * UART_STRIDE)

#define UART_DATA       0
#define UART_IER        1
#define UART_IIR        2
#define UART_LCR        3
#define UART_MCR        4
#define UART_LSR        5
#define UART_MSR        6
#define UART_SCRATCH    7

#define LCR_DLAB        0x80u
#define LSR_THRE        0x20u
#define LSR_TEMT        0x40u
#define IIR_NONE        0x01u
#define MSR_CTS_DSR_DCD 0xB0u

static const char signature[] = "apcdll.dll";

typedef struct {
    uint8_t scancode, row, column;
} faceplate_key_t;

static const faceplate_key_t faceplate_keys[] = {
    { 0xF5, 0, 0 }, { 0xF2, 0, 1 }, { 0xEB, 0, 2 }, { 0xF4, 0, 3 }, { 0x5A, 0, 4 }, { 0x9F, 0, 5 },
    { 0x11, 1, 0 }, { 0x16, 1, 3 }, { 0x1E, 1, 4 }, { 0x26, 1, 5 },
    { 0x25, 2, 0 }, { 0x2E, 2, 1 }, { 0x36, 2, 2 }, { 0x3D, 2, 3 }, { 0x3E, 2, 4 }, { 0x46, 2, 5 },
    { 0x05, 3, 0 }, { 0x45, 3, 1 }, { 0x06, 3, 2 }, { 0x03, 3, 4 }, { 0x76, 3, 5 },
    { 0x0B, 4, 0 }, { 0x83, 4, 1 },
};

bool autopc_detect(const uint8_t *image, size_t size) {
    size_t length = sizeof signature - 1;
    for (size_t i = 0; i + length <= size; i++) {
        if (!memcmp(image + i, signature, length)) return true;
    }
    return false;
}

void autopc_reset(autopc_t *board) {
    memset(board, 0, sizeof *board);
    board->fpga[FPGA_INPUTS / 4] = INPUT_IGNITION;
    board->fpga[FPGA_STATUS / 4] = STATUS_POWER_ON;
    board->socket_events[0] = board->socket_events[1] = STATUS_CHANGED;
    board->faceplate = (autopc_pci_t){
        .vendor = CLARION_VENDOR,
        .device = FACEPLATE_DEVICE,
        .class_revision = FACEPLATE_CLASS,
        .bars = { { FACEPLATE_IO_SIZE, true }, { FACEPLATE_MEMORY_SIZE, false } },
    };
}

void autopc_prepare_ram(uint8_t *dram, uint32_t size) {
    uint32_t magic = BOOTLOADER_DOWNLOAD;
    if (size >= BOOT_MAGIC_OFFSET + sizeof magic) memcpy(dram + BOOT_MAGIC_OFFSET, &magic, sizeof magic);
}

static uint32_t bytes_read(const uint8_t *bytes, uint32_t offset, int size) {
    uint32_t value = 0;
    for (int i = 0; i < size; i++) value |= (uint32_t)bytes[offset + (uint32_t)i] << (8 * i);
    return value;
}

static void bytes_write(uint8_t *bytes, uint32_t offset, int size, uint32_t value) {
    for (int i = 0; i < size; i++) bytes[offset + (uint32_t)i] = (uint8_t)(value >> (8 * i));
}

static uint32_t sized(uint32_t value, uint32_t pa, int size) {
    value >>= 8 * (pa & 3);
    return size == 4 ? value : size == 2 ? value & 0xFFFFu : value & 0xFFu;
}

static void sized_store(uint32_t *slot, uint32_t pa, int size, uint32_t value) {
    if (size == 4) {
        *slot = value;
        return;
    }
    uint32_t shift = 8 * (pa & 3), mask = (size == 2 ? 0xFFFFu : 0xFFu) << shift;
    *slot = (*slot & ~mask) | ((value << shift) & mask);
}

static void uart_character(autopc_uart_t *uart, const autopc_host_t *host, uint8_t ch) {
    if (ch == '\r') return;
    if (ch == '\n' || uart->length >= (int)sizeof uart->line - 1) {
        uart->line[uart->length] = 0;
        if (host->debug_line) host->debug_line(host->context, uart->line);
        uart->length = 0;
        if (ch == '\n') return;
    }
    uart->line[uart->length++] = (char)(ch >= 0x20 && ch < 0x7F ? ch : '?');
}

static uint32_t uart_read(const autopc_uart_t *uart, uint32_t index) {
    bool divisor = (uart->lcr & LCR_DLAB) != 0;
    switch (index) {
        case UART_DATA: return divisor ? uart->dll : 0;
        case UART_IER: return divisor ? uart->dlm : uart->ier;
        case UART_IIR: return IIR_NONE;
        case UART_LCR: return uart->lcr;
        case UART_MCR: return uart->mcr;
        case UART_LSR: return LSR_THRE | LSR_TEMT;
        case UART_MSR: return MSR_CTS_DSR_DCD;
        default: return uart->scratch;
    }
}

static void uart_write(autopc_uart_t *uart, const autopc_host_t *host, uint32_t index, uint8_t value) {
    bool divisor = (uart->lcr & LCR_DLAB) != 0;
    switch (index) {
        case UART_DATA:
            if (divisor) uart->dll = value;
            else uart_character(uart, host, value);
            break;
        case UART_IER:
            if (divisor) uart->dlm = value;
            else uart->ier = value;
            break;
        case UART_LCR: uart->lcr = value; break;
        case UART_MCR: uart->mcr = value; break;
        case UART_SCRATCH: uart->scratch = value; break;
        default: break;
    }
}

static uint32_t bridge_read(const autopc_t *board, uint32_t offset, int size) {
    uint32_t value = board->bridge[offset / 4];
    if ((offset & ~3u) == BRIDGE_DMA_STATUS) value |= DMA_IDLE;
    return sized(value, offset, size);
}

static void bridge_write(autopc_t *board, uint32_t offset, int size, uint32_t value) {
    uint32_t *slot = &board->bridge[offset / 4];
    if ((offset & ~3u) == BRIDGE_STATUS) {
        uint32_t shift = size == 4 ? 0 : 8 * (offset & 3);
        *slot &= ~((value << shift) & MASTER_ABORT);
        value &= ~(MASTER_ABORT >> shift);
    }
    sized_store(slot, offset, size, value);
}

static autopc_pci_t *config_target(autopc_t *board, uint32_t *reg) {
    uint32_t address = board->bridge[BRIDGE_CONFIG_ADDRESS / 4];
    *reg = address & 0xFCu;
    if (((address >> 8) & 0xFFu) != FACEPLATE_SLOT << 3) {
        board->bridge[BRIDGE_STATUS / 4] |= MASTER_ABORT;
        return NULL;
    }
    return &board->faceplate;
}

static const autopc_bar_t *bar_for(const autopc_pci_t *pci, uint32_t reg) {
    uint32_t index = (reg - PCI_BAR0) / 4;
    return reg >= PCI_BAR0 && index < AUTOPC_PCI_BARS ? &pci->bars[index] : NULL;
}

static uint32_t bar_base(const autopc_pci_t *pci, int index) {
    return pci->config[PCI_BAR0 / 4 + index] & ~(pci->bars[index].size - 1);
}

static uint32_t config_read(autopc_t *board) {
    uint32_t reg;
    autopc_pci_t *pci = config_target(board, &reg);
    if (!pci) return NO_DEVICE;
    const autopc_bar_t *bar = bar_for(pci, reg);
    if (bar) return (pci->config[reg / 4] & ~(bar->size - 1)) | (bar->io ? BAR_IO : 0);
    switch (reg) {
        case PCI_ID: return (uint32_t)pci->device << 16 | pci->vendor;
        case PCI_CLASS: return pci->class_revision;
        default: return pci->config[reg / 4];
    }
}

static void config_write(autopc_t *board, uint32_t value) {
    uint32_t reg;
    autopc_pci_t *pci = config_target(board, &reg);
    if (pci && reg != PCI_ID && reg != PCI_CLASS) pci->config[reg / 4] = value;
}

static bool config_selected(const autopc_t *board, uint32_t pa) {
    return pa - CONFIG_DATA_PA < 4 && (board->bridge[BRIDGE_CONFIG_ADDRESS / 4] & CONFIG_ENABLE);
}

static bool io_hit(const autopc_pci_t *pci, uint32_t pa, uint32_t *offset) {
    if (!(pci->config[PCI_COMMAND / 4] & COMMAND_IO)) return false;
    if (pa - PCI_IO_PA >= PCI_IO_WINDOWS * PCI_IO_WINDOW_SIZE) return false;
    *offset = (pa - PCI_IO_PA) % PCI_IO_WINDOW_SIZE - bar_base(pci, 0);
    return *offset < pci->bars[0].size;
}

static bool memory_hit(const autopc_pci_t *pci, uint32_t pa, uint32_t *offset) {
    if (!(pci->config[PCI_COMMAND / 4] & COMMAND_MEMORY)) return false;
    *offset = pa - PCI_MEMORY_PA - bar_base(pci, 1);
    return *offset < pci->bars[1].size;
}

static bool card_present(const autopc_host_t *host, int socket) {
    return socket == 0 && host->card && host->card->state->inserted;
}

static uint16_t socket_status(const autopc_t *board, const autopc_host_t *host, int socket) {
    uint16_t status = board->socket_events[socket];
    if (!card_present(host, socket)) return status | STATUS_NO_CARD;
    const cfcard_t *card = host->card->state;
    bool line = cfcard_io_mode(card) ? cfcard_interrupt(card) : !cfcard_ready(card);
    return line ? (uint16_t)(status | STATUS_IREQ) : status;
}

static uint32_t fpga_pending(const autopc_t *board, const autopc_host_t *host) {
    uint32_t pending = board->fpga[FPGA_PENDING / 4];
    for (int socket = 0; socket < AUTOPC_SOCKETS; socket++) {
        uint16_t active = socket_status(board, host, socket) & (uint16_t)(board->socket_control[socket] >> 3) & (STATUS_IREQ | STATUS_CHANGED);
        if (active) pending |= FPGA_SOCKET_INTR << socket;
    }
    return pending;
}

static void update_irl(const autopc_t *board, const autopc_host_t *host) {
    bool faceplate = (board->faceplate_pending & board->faceplate_enable) != 0;
    bool fpga = (fpga_pending(board, host) & board->fpga[FPGA_ENABLE / 4]) != 0;
    if (host->irl) host->irl(host->context, faceplate || fpga);
}

static void raise_faceplate(autopc_t *board, const autopc_host_t *host, uint32_t bits) {
    board->faceplate_pending |= bits;
    update_irl(board, host);
}

static void link_reply(autopc_t *board, uint32_t value) {
    if (board->link_reply_count == AUTOPC_LINK_REPLIES) return;
    board->link_replies[(board->link_reply_head + board->link_reply_count) % AUTOPC_LINK_REPLIES] = value;
    board->link_reply_count++;
}

static void link_next_reply(autopc_t *board, const autopc_host_t *host) {
    if (!board->link_reply_count) return;
    board->link_received = board->link_replies[board->link_reply_head];
    board->link_reply_head = (board->link_reply_head + 1) % AUTOPC_LINK_REPLIES;
    board->link_reply_count--;
    raise_faceplate(board, host, LINK_RX_READY);
}

static uint8_t lcd_read(const autopc_t *board) {
    uint8_t index = board->lcd_index, value = board->lcd_registers[index];
    switch (index) {
        case 0x03: return value | 0x0E;
        case 0x0B: return value | 0xFC;
        case 0x0C: return 0xFF;
        case LCD_PALETTE_INDEX: return value | 0x20;
        case LCD_PALETTE_DATA: return board->lcd_palette[board->lcd_registers[LCD_PALETTE_INDEX]] | 0xF0;
        default: return value;
    }
}

static void lcd_write(autopc_t *board, uint8_t value) {
    if (board->lcd_index == LCD_PALETTE_DATA) board->lcd_palette[board->lcd_registers[LCD_PALETTE_INDEX]] = value & 0x0F;
    else board->lcd_registers[board->lcd_index] = value;
}

static void lcd_pixels_write(autopc_t *board, uint32_t pixels) {
    uint32_t first = board->lcd_address * 4 / PIXEL_BITS;
    for (uint32_t k = 0; k < PIXELS_PER_WORD; k++) {
        if (first + k < sizeof board->lcd_pixels) board->lcd_pixels[first + k] = (uint8_t)((pixels >> (PIXEL_BITS * k)) & 7);
    }
    board->lcd_address += NIBBLES_PER_WORD;
}

static uint8_t key_columns(const autopc_t *board) {
    uint8_t down = 0;
    for (int row = 0; row < AUTOPC_KEY_ROWS; row++) {
        if (board->key_rows_selected & (1u << row)) down |= board->keys_down[row];
    }
    return (uint8_t)~down;
}

static void faceplate_word(autopc_t *board, uint32_t word) {
    uint8_t value = (uint8_t)(word >> 8);
    switch ((uint8_t)word) {
        case WORD_CONTROL: board->faceplate_control = value; break;
        case WORD_LCD:
            if (word & WORD_DATA) lcd_write(board, value);
            else board->lcd_index = value;
            break;
        case WORD_LCD_READ: link_reply(board, lcd_read(board)); break;
        case WORD_KEY_ROWS: board->key_rows_selected = (uint8_t)~value; break;
        case WORD_KEY_READ: link_reply(board, key_columns(board)); break;
        case WORD_ADDRESS: board->lcd_address = word >> 16; break;
        case WORD_PIXELS: lcd_pixels_write(board, word >> 8); break;
        default: break;
    }
}

static void link_transmit(autopc_t *board, const autopc_host_t *host) {
    uint32_t start = board->link_address & LINK_OFFSET_MASK;
    uint32_t end = (board->link_control & LINK_OFFSET_MASK) + 4;
    for (uint32_t offset = start; offset < end && offset + 4 <= sizeof board->faceplate_memory; offset += 4)
        faceplate_word(board, bytes_read(board->faceplate_memory, offset, 4));
    board->link_control &= ~LINK_TRANSMIT;
    raise_faceplate(board, host, LINK_TX_DONE);
    link_next_reply(board, host);
}

static bool faceplate_io_read(autopc_t *board, const autopc_host_t *host, uint32_t offset, int size, uint32_t *value) {
    switch (offset) {
        case FACEPLATE_ENABLE: *value = board->faceplate_enable; return true;
        case FACEPLATE_PENDING: *value = board->faceplate_pending; return true;
        case LINK_ADDRESS: *value = board->link_address; return true;
        case LINK_CONTROL: *value = board->link_control; return true;
        case LINK_RECEIVE:
            *value = board->link_received;
            link_next_reply(board, host);
            return true;
        default:
            *value = bytes_read(board->faceplate_io, offset, size);
            return offset == LINK_CONFIG;
    }
}

static bool faceplate_io_write(autopc_t *board, const autopc_host_t *host, uint32_t offset, int size, uint32_t value) {
    switch (offset) {
        case FACEPLATE_ENABLE:
            board->faceplate_enable = value;
            update_irl(board, host);
            return true;
        case FACEPLATE_PENDING:
            board->faceplate_pending &= ~value;
            update_irl(board, host);
            return true;
        case LINK_ADDRESS: board->link_address = value; return true;
        case LINK_CONTROL:
            board->link_control = value;
            if (value & LINK_TRANSMIT) link_transmit(board, host);
            return true;
        default:
            bytes_write(board->faceplate_io, offset, size, value);
            return offset == LINK_CONFIG;
    }
}

static bool socket_register(uint32_t pa, int *socket, uint32_t *reg) {
    uint32_t offset = pa - SOCKET_PA;
    if (offset >= SOCKET_SIZE) return false;
    *socket = (int)(offset / SOCKET_STRIDE);
    *reg = offset % SOCKET_STRIDE;
    return offset == SOCKET_SHARED || offset == SOCKET_SHARED - 4 || (*socket < AUTOPC_SOCKETS && (*reg == SOCKET_CONTROL || *reg == SOCKET_STATUS));
}

static uint32_t socket_read(const autopc_t *board, const autopc_host_t *host, uint32_t pa, int socket, uint32_t reg) {
    if (pa - SOCKET_PA == SOCKET_SHARED) return board->socket_shared;
    if (pa - SOCKET_PA == SOCKET_SHARED - 4) return SHARED_BATTERY_OK;
    return reg == SOCKET_CONTROL ? board->socket_control[socket] : socket_status(board, host, socket);
}

static void socket_write(autopc_t *board, const autopc_host_t *host, uint32_t pa, int socket, uint32_t reg, uint32_t value) {
    if (pa - SOCKET_PA == SOCKET_SHARED) {
        board->socket_shared = (uint16_t)value;
        return;
    }
    if (reg == SOCKET_STATUS) {
        board->socket_events[socket] &= (uint16_t)~(value & STATUS_CHANGED);
    } else {
        uint16_t old = board->socket_control[socket];
        board->socket_control[socket] = (uint16_t)value;
        if (card_present(host, socket) && (old & CONTROL_RESET) && !(value & CONTROL_RESET)) cfcard_reset(host->card);
    }
    update_irl(board, host);
}

static bool card_access(autopc_t *board, const autopc_host_t *host, uint32_t pa, int size, bool write, uint32_t *value) {
    if (pa - CARD_PA >= CARD_END - CARD_PA) return false;
    uint32_t offset = pa & CARD_OFFSET_MASK, window = pa & 0xFF000000u;
    int socket = (int)((pa >> CARD_SOCKET_SHIFT) & 1);
    uint32_t empty = size == 1 ? 0xFFu : size == 2 ? 0xFFFFu : 0xFFFFFFFFu;
    if (!card_present(host, socket)) {
        if (!write) *value = empty;
        return true;
    }
    cfcard_slot_t *slot = host->card;
    switch (window) {
        case 0x14000000u:
        case 0x18000000u:
            if (write) cfcard_attribute_write(slot, offset, size, *value);
            else *value = cfcard_attribute_read(slot, offset, size);
            break;
        case 0x15000000u:
        case 0x19000000u:
            if (write) cfcard_common_write(slot, offset, size, *value);
            else *value = cfcard_common_read(slot, offset, size);
            break;
        case 0x1A000000u:
            if (write) cfcard_io_write(slot, offset, size, *value);
            else *value = cfcard_io_read(slot, offset, size);
            break;
        default:
            if (!write) *value = empty;
            if (host->trace) host->trace(host->context, write, pa, size, *value);
            break;
    }
    update_irl(board, host);
    return true;
}

void autopc_card_changed(autopc_t *board, const autopc_host_t *host) {
    board->socket_events[0] |= STATUS_CHANGED;
    update_irl(board, host);
}

bool autopc_read(autopc_t *board, const autopc_host_t *host, uint32_t pa, int size, uint32_t *value) {
    uint32_t offset;
    int socket;
    uint32_t reg;
    if (socket_register(pa, &socket, &reg)) {
        *value = socket_read(board, host, pa, socket, reg);
        return true;
    }
    if (card_access(board, host, pa, size, false, value)) return true;
    if (pa - BRIDGE_PA < BRIDGE_SIZE) {
        *value = bridge_read(board, pa - BRIDGE_PA, size);
        return true;
    }
    if (config_selected(board, pa)) {
        *value = sized(config_read(board), pa, size);
        return true;
    }
    if (io_hit(&board->faceplate, pa, &offset)) {
        if (!faceplate_io_read(board, host, offset, size, value) && host->trace) host->trace(host->context, false, pa, size, *value);
        return true;
    }
    if (memory_hit(&board->faceplate, pa, &offset)) {
        *value = bytes_read(board->faceplate_memory, offset, size);
        return true;
    }
    if (pa - FPGA_PA < FPGA_SIZE) {
        uint32_t index = (pa - FPGA_PA) / 4;
        *value = sized(index == FPGA_PENDING / 4 ? fpga_pending(board, host) : board->fpga[index], pa, size);
        return true;
    }
    if (pa - DEBUG_UART_PA < UART_SIZE) {
        *value = uart_read(&board->debug_uart, (pa - DEBUG_UART_PA) / UART_STRIDE);
        return true;
    }
    return false;
}

bool autopc_write(autopc_t *board, const autopc_host_t *host, uint32_t pa, int size, uint32_t value) {
    uint32_t offset;
    int socket;
    uint32_t reg;
    if (socket_register(pa, &socket, &reg)) {
        socket_write(board, host, pa, socket, reg, value);
        return true;
    }
    if (card_access(board, host, pa, size, true, &value)) return true;
    if (pa - BRIDGE_PA < BRIDGE_SIZE) {
        bridge_write(board, pa - BRIDGE_PA, size, value);
        return true;
    }
    if (config_selected(board, pa)) {
        if (size == 4) config_write(board, value);
        return true;
    }
    if (io_hit(&board->faceplate, pa, &offset)) {
        if (!faceplate_io_write(board, host, offset, size, value) && host->trace) host->trace(host->context, true, pa, size, value);
        return true;
    }
    if (memory_hit(&board->faceplate, pa, &offset)) {
        bytes_write(board->faceplate_memory, offset, size, value);
        return true;
    }
    if (pa - FPGA_PA < FPGA_SIZE) {
        uint32_t index = (pa - FPGA_PA) / 4;
        if (index == FPGA_PENDING / 4) board->fpga[index] &= ~value;
        else if (index != FPGA_INPUTS / 4 && index != FPGA_STATUS / 4) sized_store(&board->fpga[index], pa, size, value);
        update_irl(board, host);
        return true;
    }
    if (pa - DEBUG_UART_PA < UART_SIZE) {
        uart_write(&board->debug_uart, host, (pa - DEBUG_UART_PA) / UART_STRIDE, (uint8_t)value);
        return true;
    }
    return false;
}

void autopc_screen(const autopc_t *board, uint8_t *pixels) {
    memcpy(pixels, board->lcd_pixels, sizeof board->lcd_pixels);
}

int autopc_palette(uint32_t *palette) {
    for (uint32_t value = 0; value < 8; value++) {
        uint32_t red = value & PIXEL_RED ? 0xFFu : 0, green = value & PIXEL_GREEN ? 0xFFu : 0, blue = value & PIXEL_BLUE ? 0xFFu : 0;
        palette[value] = red | green << 8 | blue << 16;
    }
    return 8;
}

void autopc_key(autopc_t *board, const autopc_host_t *host, uint8_t scancode, bool up) {
    for (size_t i = 0; i < sizeof faceplate_keys / sizeof faceplate_keys[0]; i++) {
        const faceplate_key_t *key = &faceplate_keys[i];
        if (key->scancode != scancode) continue;
        uint8_t bit = (uint8_t)(1u << key->column);
        if (up) board->keys_down[key->row] &= (uint8_t)~bit;
        else board->keys_down[key->row] |= bit;
        raise_faceplate(board, host, FACEPLATE_MESSAGE);
        return;
    }
}
