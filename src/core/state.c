/* Read and write saved machine states: the file header, named records and their compatibility checks. */
#include "core/state.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#define STATE_MAGIC  "SH3ODO01"
#define HEADER_BYTES (sizeof STATE_MAGIC + 16)

typedef struct {
    const uint8_t *cursor, *end;
} state_reader_t;

typedef struct {
    char name[256];
    const uint8_t *data;
    uint32_t size;
} state_entry_t;

static bool write_bytes(gzFile file, const void *data, uint32_t size) {
    return size == 0 || gzwrite(file, data, size) == (int)size;
}

static bool write_record(gzFile file, const char *name, const void *data, uint32_t size) {
    uint8_t length = (uint8_t)strlen(name);
    return write_bytes(file, &length, 1) && write_bytes(file, name, length) && write_bytes(file, &size, sizeof size) && write_bytes(file, data, size);
}

bool state_save(const char *path, uint64_t rom_hash, int64_t host_time, const state_field_t *fields, size_t count) {
    char temporary[1100];
    int written = snprintf(temporary, sizeof temporary, "%s.tmp", path);
    if (written < 0 || (size_t)written >= sizeof temporary) return false;
    gzFile file = gzopen(temporary, "wb1");
    if (!file) return false;
    bool ok = write_bytes(file, STATE_MAGIC, sizeof STATE_MAGIC) && write_bytes(file, &rom_hash, sizeof rom_hash) &&
              write_bytes(file, &host_time, sizeof host_time);
    for (size_t i = 0; i < count; i++) ok = ok && write_record(file, fields[i].name, fields[i].data, fields[i].size);
    uint8_t end = 0;
    ok = ok && write_bytes(file, &end, 1);
    ok = gzclose(file) == Z_OK && ok;
    if (ok) ok = rename(temporary, path) == 0;
    else remove(temporary);
    return ok;
}

bool state_read(const char *path, state_file_t *file) {
    file->data = NULL;
    file->length = 0;
    gzFile input = gzopen(path, "rb");
    if (!input) return false;
    size_t capacity = 1 << 20, used = 0;
    uint8_t *data = malloc(capacity);
    while (data) {
        if (used == capacity) {
            uint8_t *grown = realloc(data, capacity * 2);
            if (!grown) {
                free(data);
                data = NULL;
                break;
            }
            data = grown;
            capacity *= 2;
        }
        int got = gzread(input, data + used, (unsigned)(capacity - used));
        if (got <= 0) break;
        used += (size_t)got;
    }
    gzclose(input);
    file->data = data;
    file->length = used;
    return data != NULL;
}

void state_free(state_file_t *file) {
    free(file->data);
    file->data = NULL;
    file->length = 0;
}

static state_reader_t records(const state_file_t *file) {
    return (state_reader_t){ file->data + HEADER_BYTES, file->data + file->length };
}

static int next_entry(state_reader_t *reader, state_entry_t *entry) {
    if (reader->cursor >= reader->end) return 0;
    uint8_t name_length = *reader->cursor++;
    if (!name_length || reader->end - reader->cursor < name_length + 4) return 0;
    memcpy(entry->name, reader->cursor, name_length);
    entry->name[name_length] = 0;
    reader->cursor += name_length;
    memcpy(&entry->size, reader->cursor, 4);
    reader->cursor += 4;
    if ((size_t)(reader->end - reader->cursor) < entry->size) return -1;
    entry->data = reader->cursor;
    reader->cursor += entry->size;
    return 1;
}

static bool size_accepted(const state_field_t *field, uint32_t size) {
    if (size == field->size) return true;
    for (size_t i = 0; i < field->older_count; i++) {
        if (size == field->older_sizes[i]) return true;
    }
    return false;
}

bool state_usable(const state_file_t *file, uint64_t rom_hash, const state_field_t *fields, size_t count) {
    if (!file->data || file->length < HEADER_BYTES || memcmp(file->data, STATE_MAGIC, sizeof STATE_MAGIC) ||
        memcmp(file->data + sizeof STATE_MAGIC, &rom_hash, 8)) return false;
    state_reader_t reader = records(file);
    state_entry_t entry;
    int found;
    while ((found = next_entry(&reader, &entry)) > 0) {
        for (size_t i = 0; i < count; i++) {
            if (!strcmp(entry.name, fields[i].name) && !size_accepted(&fields[i], entry.size)) return false;
        }
    }
    return found == 0;
}

int64_t state_host_time(const state_file_t *file) {
    int64_t host_time;
    memcpy(&host_time, file->data + sizeof STATE_MAGIC + 8, sizeof host_time);
    return host_time;
}

void state_apply(const state_file_t *file, const state_field_t *fields, size_t count) {
    state_reader_t reader = records(file);
    state_entry_t entry;
    while (next_entry(&reader, &entry) > 0) {
        for (size_t i = 0; i < count; i++) {
            if (strcmp(entry.name, fields[i].name) || !size_accepted(&fields[i], entry.size)) continue;
            if (entry.size != fields[i].size && fields[i].migrate) {
                fields[i].migrate(fields[i].data, entry.data, entry.size);
                continue;
            }
            memset(fields[i].data, 0, fields[i].size);
            memcpy(fields[i].data, entry.data, entry.size);
        }
    }
}

const uint8_t *state_record(const state_file_t *file, const char *name, uint32_t *size) {
    state_reader_t reader = records(file);
    state_entry_t entry;
    while (next_entry(&reader, &entry) > 0) {
        if (strcmp(entry.name, name)) continue;
        *size = entry.size;
        return entry.data;
    }
    return NULL;
}
