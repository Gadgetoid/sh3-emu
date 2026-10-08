#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "core/sh3.h"

typedef struct {
    void    *context;
    uint8_t *(*map)(void *context, uint32_t va, bool write);
} accel_memory_t;

typedef const uint8_t *(*accel_rom_fn)(void *context, uint32_t pa, uint32_t length);

typedef bool (*accel_handler_fn)(sh3_cpu_t *cpu, const accel_memory_t *memory);

typedef enum { ACCEL_UNCHECKED, ACCEL_MATCHED, ACCEL_MISMATCHED } accel_state_t;

typedef enum {
    ACCEL_DECODE, ACCEL_ENCODE, ACCEL_FILL, ACCEL_STRCMP, ACCEL_PURGE, ACCEL_WIDEN, ACCEL_RANGE, ACCEL_WCSLEN,
} accel_kind_t;

typedef struct {
    uint32_t va;
    accel_kind_t kind;
    const uint32_t *code;
    uint32_t words;
    accel_handler_fn run;
    accel_state_t state;
} accel_hook_t;

#define ACCEL_HOOKS_MAX 12

typedef struct {
    int system;
    int count;
    accel_hook_t hooks[ACCEL_HOOKS_MAX];
} accel_hooks_t;

bool accel_find(accel_rom_fn rom, void *context, accel_hooks_t *hooks);
bool accel_hooked(const accel_hooks_t *hooks, uint32_t pc);
bool accel_call(accel_hooks_t *hooks, sh3_cpu_t *cpu, const accel_memory_t *memory, uint32_t pc);
