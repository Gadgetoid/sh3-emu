#pragma once
#include <stdbool.h>
#include <stdint.h>

#define MAILBOX_TRAPA       0xCEu
#define MAILBOX_VERSION     1
#define MAILBOX_MESSAGE_MAX 65536u
#define MAILBOX_QUEUE       64

enum { MAILBOX_PROBE = 0, MAILBOX_RECV = 1, MAILBOX_SEND = 2 };

typedef struct {
    uint8_t *data;
    uint32_t length;
} mailbox_message_t;

typedef struct {
    mailbox_message_t messages[MAILBOX_QUEUE];
    int               first;
    int               count;
} mailbox_queue_t;

#define MAILBOX_EMULATOR_SEQUENCE 0x8000u

typedef struct {
    mailbox_queue_t to_guest;
    mailbox_queue_t to_host;
    mailbox_queue_t to_guest_from_emulator;
    mailbox_queue_t to_emulator;
    bool            connected;
} mailbox_t;

typedef struct {
    uint32_t operation;
    uint32_t buffer;
    uint32_t length;
    uint32_t result;
    uint32_t extra;
} mailbox_call_t;

typedef bool (*mailbox_copy_fn)(void *context, uint32_t va, uint8_t *data, uint32_t length, bool write);

void  mailbox_clear(mailbox_t *mailbox);
void  mailbox_clear_host(mailbox_t *mailbox);
void  mailbox_clear_queue(mailbox_queue_t *queue);
bool  mailbox_push(mailbox_queue_t *queue, const uint8_t *data, uint32_t length);
const mailbox_message_t *mailbox_peek(const mailbox_queue_t *queue);
void  mailbox_pop(mailbox_queue_t *queue);
bool  mailbox_trap(mailbox_t *mailbox, mailbox_call_t *call, mailbox_copy_fn copy, void *context, uint32_t *fault_va);
