#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MACHINE_CLOCK_HZ      36864000u
#define MACHINE_SCREEN_WIDTH  480
#define MACHINE_SCREEN_HEIGHT 240

typedef struct machine machine_t;

typedef void (*machine_log_fn)(const char *message);

machine_t *machine_create(const uint8_t *rom, size_t rom_size, char *error, size_t error_size);
void       machine_destroy(machine_t *machine);
void       machine_set_log(machine_t *machine, machine_log_fn log);
void       machine_run(machine_t *machine, uint64_t cycles);
uint64_t   machine_cycles(machine_t *machine);
uint32_t   machine_pc(machine_t *machine);
bool       machine_halted(machine_t *machine);
const char *machine_halt_reason(machine_t *machine);

bool machine_lcd_enabled(machine_t *machine);
bool machine_backlight(machine_t *machine);
void machine_backlight_button(machine_t *machine);
bool machine_screen(machine_t *machine, uint8_t *levels);

void machine_key(machine_t *machine, uint8_t scancode, bool up);
void machine_touch(machine_t *machine, bool down, int x, int y);
void machine_power_button(machine_t *machine, bool down);
bool machine_suspended(machine_t *machine);

size_t machine_audio(machine_t *machine, int16_t *samples, size_t max, uint32_t *rate);

void machine_reset(machine_t *machine);
bool machine_save(machine_t *machine, const char *path, int64_t host_time);
bool machine_load(machine_t *machine, const char *path, int64_t *host_time);
void machine_advance_clock(machine_t *machine, int64_t seconds);

void machine_dump_state(machine_t *machine);
bool machine_read_virtual(machine_t *machine, uint32_t va, uint32_t *value);
