#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef bool (*state_migrate_fn)(void *data, const uint8_t *old, uint32_t size);

typedef struct {
    const char *name;
    void *data;
    uint32_t size;
    const uint32_t *older_sizes;
    size_t older_count;
    state_migrate_fn migrate;
} state_field_t;

typedef struct {
    uint8_t *data;
    size_t length;
} state_file_t;

bool           state_save(const char *path, uint64_t rom_hash, int64_t host_time, const state_field_t *fields, size_t count);
bool           state_read(const char *path, state_file_t *file);
void           state_free(state_file_t *file);
bool           state_usable(const state_file_t *file, uint64_t rom_hash, const state_field_t *fields, size_t count);
int64_t        state_host_time(const state_file_t *file);
void           state_apply(const state_file_t *file, const state_field_t *fields, size_t count);
const uint8_t *state_record(const state_file_t *file, const char *name, uint32_t *size);
