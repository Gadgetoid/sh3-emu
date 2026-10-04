#include "core/ppfs.h"

#include <dirent.h>
#include <fnmatch.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#define PAR_EN        0x80000000u
#define PAR_AUTOEN    0x20000000u
#define PAR_BUSY      0x10000000u
#define PAR_NACK      0x08000000u
#define PAR_SELECT    0x02000000u
#define PAR_NFAULT    0x01000000u
#define PAR_INTR      0x00100000u
#define PAR_AUTOFD    0x00020000u
#define PAR_DATA_SHIFT 8

#define SIGNATURE     0xAA5555AAu
#define TRAILER       0x1A0AA55Au
#define HEADER_SIZE   4
#define FRAME_EXTRA   (4 + 4)
#define FAILURE       0xFFFFFFFFu

#define OP_BOOT       0x0000
#define OP_INIT       0x0001
#define OP_OPEN       0x0002
#define OP_CLOSE      0x0003
#define OP_READ       0x0004
#define OP_WRITE      0x0005
#define OP_SEEK       0x0006
#define OP_DELETE     0x0007
#define OP_FINDFIRST  0x0008
#define OP_FINDNEXT   0x0009

#define MODE_ACCESS   0x0003u
#define MODE_CREATE   0x0100u
#define MODE_TRUNCATE 0x0200u
#define FIND_DATA_SIZE 280
#define FIND_NAME_MAX 260
#define READ_CHUNK_MAX (32 * 1024)

static void ppfs_logf(ppfs_t *ppfs, const char *format, ...) {
    if (!ppfs->log) return;
    char text[512];
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(text, sizeof text, format, arguments);
    va_end(arguments);
    ppfs->log(ppfs->log_context, text);
}

void ppfs_init(ppfs_t *ppfs) {
    ppfs_log_fn log = ppfs->log;
    void *context = ppfs->log_context;
    char root[PPFS_PATH_MAX];
    memcpy(root, ppfs->root, sizeof root);
    memset(ppfs, 0, sizeof *ppfs);
    ppfs->log = log;
    ppfs->log_context = context;
    memcpy(ppfs->root, root, sizeof root);
}

void ppfs_close_all(ppfs_t *ppfs) {
    for (int i = 0; i < PPFS_FILES; i++) {
        if (ppfs->files[i]) fclose(ppfs->files[i]);
        ppfs->files[i] = NULL;
    }
}

void ppfs_set_root(ppfs_t *ppfs, const char *root) {
    snprintf(ppfs->root, sizeof ppfs->root, "%s", root ? root : "");
}

static uint32_t get_u32(const uint8_t *data) {
    return (uint32_t)data[0] | (uint32_t)data[1] << 8 | (uint32_t)data[2] << 16 | (uint32_t)data[3] << 24;
}

static void put_u32(uint8_t *data, uint32_t value) {
    data[0] = (uint8_t)value;
    data[1] = (uint8_t)(value >> 8);
    data[2] = (uint8_t)(value >> 16);
    data[3] = (uint8_t)(value >> 24);
}

static void reply(ppfs_t *ppfs, uint16_t opcode, uint32_t value, const uint8_t *data, size_t length) {
    uint32_t total = (uint32_t)(HEADER_SIZE + 4 + length + 1);
    uint8_t *out = ppfs->output;
    put_u32(out, SIGNATURE);
    put_u32(out + 4, opcode | total << 16);
    put_u32(out + 8, value);
    if (length) memcpy(out + 12, data, length);
    uint32_t sum = 0;
    for (size_t i = 4; i < 12 + length; i++) sum += out[i];
    out[12 + length] = (uint8_t)sum;
    put_u32(out + 13 + length, TRAILER);
    ppfs->output_length = 17 + length;
    ppfs->output_position = 0;
}

static const char *leaf(const char *path) {
    const char *name = path;
    for (const char *c = path; *c; c++)
        if (*c == '\\' || *c == '/' || *c == ':') name = c + 1;
    return name;
}

static bool find_host_name(ppfs_t *ppfs, const char *name, char *path, size_t size) {
    if (!ppfs->root[0] || !name[0] || !strcmp(name, ".") || !strcmp(name, "..")) return false;
    DIR *directory = opendir(ppfs->root);
    if (!directory) return false;
    bool found = false;
    struct dirent *entry;
    while (!found && (entry = readdir(directory))) {
        if (strcasecmp(entry->d_name, name)) continue;
        snprintf(path, size, "%s/%s", ppfs->root, entry->d_name);
        found = true;
    }
    closedir(directory);
    return found;
}

static uint32_t open_file(ppfs_t *ppfs, uint32_t mode, const char *requested) {
    const char *name = leaf(requested);
    char path[PPFS_PATH_MAX + 260];
    bool exists = find_host_name(ppfs, name, path, sizeof path);
    bool writing = (mode & MODE_ACCESS) != 0;
    if (!exists) {
        if (ppfs->root[0] && strchr(name, '.') && !(mode & MODE_CREATE)) ppfs_logf(ppfs, "ppfs: no %s in %s\n", name, ppfs->root);
        if (!(mode & MODE_CREATE) || !ppfs->root[0] || !name[0]) return FAILURE;
        snprintf(path, sizeof path, "%s/%s", ppfs->root, name);
    }
    int slot = 0;
    while (slot < PPFS_FILES && ppfs->files[slot]) slot++;
    if (slot == PPFS_FILES) return FAILURE;
    const char *how = !writing ? "rb" : (!exists || (mode & MODE_TRUNCATE)) ? "w+b" : "r+b";
    ppfs->files[slot] = fopen(path, how);
    if (!ppfs->files[slot]) return FAILURE;
    ppfs_logf(ppfs, "ppfs: opened %s\n", path);
    return (uint32_t)slot;
}

static FILE *file_for(ppfs_t *ppfs, uint32_t handle) {
    return handle < PPFS_FILES ? ppfs->files[handle] : NULL;
}

static bool next_match(ppfs_t *ppfs, uint8_t *data) {
    if (!ppfs->root[0]) return false;
    DIR *directory = opendir(ppfs->root);
    if (!directory) return false;
    struct dirent *entry;
    int index = 0;
    bool found = false;
    while (!found && (entry = readdir(directory))) {
        if (entry->d_name[0] == '.' || fnmatch(ppfs->find_pattern, entry->d_name, FNM_CASEFOLD)) continue;
        if (index++ < ppfs->find_index) continue;
        char path[PPFS_PATH_MAX + 260];
        snprintf(path, sizeof path, "%s/%s", ppfs->root, entry->d_name);
        struct stat info;
        if (stat(path, &info) || !S_ISREG(info.st_mode)) continue;
        memset(data, 0, FIND_DATA_SIZE);
        put_u32(data + 12, (uint32_t)info.st_mtime);
        put_u32(data + 16, (uint32_t)info.st_size);
        snprintf((char *)data + 20, FIND_NAME_MAX, "%s", entry->d_name);
        found = true;
    }
    closedir(directory);
    ppfs->find_index++;
    return found;
}

static void handle_message(ppfs_t *ppfs, uint16_t opcode, const uint8_t *data, size_t length) {
    static uint8_t buffer[READ_CHUNK_MAX];
    uint8_t find_data[FIND_DATA_SIZE];
    switch (opcode) {
    case OP_BOOT:
    case OP_INIT:
        reply(ppfs, opcode, 0, NULL, 0);
        return;
    case OP_OPEN: {
        if (length < 5) break;
        char name[260];
        snprintf(name, sizeof name, "%.*s", (int)(length - 4), (const char *)data + 4);
        reply(ppfs, opcode, open_file(ppfs, get_u32(data), name), NULL, 0);
        return;
    }
    case OP_CLOSE: {
        if (length < 4) break;
        FILE *file = file_for(ppfs, get_u32(data));
        if (file) {
            fclose(file);
            ppfs->files[get_u32(data)] = NULL;
        }
        reply(ppfs, opcode, file ? 0 : FAILURE, NULL, 0);
        return;
    }
    case OP_READ: {
        if (length < 8) break;
        FILE *file = file_for(ppfs, get_u32(data));
        uint32_t count = get_u32(data + 4);
        if (count > READ_CHUNK_MAX) count = READ_CHUNK_MAX;
        size_t got = file ? fread(buffer, 1, count, file) : 0;
        reply(ppfs, opcode, file ? (uint32_t)got : FAILURE, buffer, got);
        return;
    }
    case OP_WRITE: {
        if (length < 8) break;
        FILE *file = file_for(ppfs, get_u32(data));
        uint32_t count = get_u32(data + 4);
        if (count > length - 8) count = (uint32_t)(length - 8);
        size_t wrote = file ? fwrite(data + 8, 1, count, file) : 0;
        if (file) fflush(file);
        reply(ppfs, opcode, file ? (uint32_t)wrote : FAILURE, NULL, 0);
        return;
    }
    case OP_SEEK: {
        if (length < 12) break;
        FILE *file = file_for(ppfs, get_u32(data));
        int whence = get_u32(data + 8) == 1 ? SEEK_CUR : get_u32(data + 8) == 2 ? SEEK_END : SEEK_SET;
        bool ok = file && fseek(file, (long)(int32_t)get_u32(data + 4), whence) == 0;
        reply(ppfs, opcode, ok ? (uint32_t)ftell(file) : FAILURE, NULL, 0);
        return;
    }
    case OP_FINDFIRST: {
        if (length < 4) break;
        if (length > 4) snprintf(ppfs->find_pattern, sizeof ppfs->find_pattern, "%s", leaf((const char *)data + 4));
        else snprintf(ppfs->find_pattern, sizeof ppfs->find_pattern, "*");
        if (!strcmp(ppfs->find_pattern, "*.*")) snprintf(ppfs->find_pattern, sizeof ppfs->find_pattern, "*");
        ppfs->find_handle = get_u32(data);
        ppfs->find_index = 0;
        bool found = next_match(ppfs, find_data);
        reply(ppfs, opcode, found ? 0 : FAILURE, find_data, found ? FIND_DATA_SIZE : 0);
        return;
    }
    case OP_FINDNEXT: {
        bool found = next_match(ppfs, find_data);
        reply(ppfs, opcode, found ? 0 : FAILURE, find_data, found ? FIND_DATA_SIZE : 0);
        return;
    }
    default:
        break;
    }
    reply(ppfs, (uint16_t)(opcode | 0x8000), FAILURE, NULL, 0);
}

static void receive_byte(ppfs_t *ppfs, uint8_t byte) {
    ppfs->output_length = 0;
    ppfs->output_position = 0;
    if (ppfs->input_length < 4) {
        static const uint8_t signature[4] = { 0xAA, 0x55, 0x55, 0xAA };
        if (byte != signature[ppfs->input_length]) {
            ppfs->input_length = byte == signature[0] ? 1 : 0;
            if (ppfs->input_length) ppfs->input[0] = byte;
            return;
        }
        ppfs->input[ppfs->input_length++] = byte;
        return;
    }
    if (ppfs->input_length >= sizeof ppfs->input) {
        ppfs->input_length = 0;
        return;
    }
    ppfs->input[ppfs->input_length++] = byte;
    if (ppfs->input_length < 8) return;
    uint32_t header = get_u32(ppfs->input + 4);
    size_t total = (header >> 16) + FRAME_EXTRA;
    if (total < 4 + 4 + 1 + 4) {
        ppfs->input_length = 0;
        return;
    }
    if (ppfs->input_length < total) return;
    size_t data_length = (header >> 16) - HEADER_SIZE - 1;
    ppfs->input_length = 0;
    handle_message(ppfs, (uint16_t)header, ppfs->input + 8, data_length);
}

uint32_t ppfs_read_register(ppfs_t *ppfs) {
    uint32_t value = PAR_BUSY | PAR_NFAULT | PAR_SELECT;
    if (ppfs->send_phase == 1) value |= PAR_AUTOFD;
    if (ppfs->output_position < ppfs->output_length) value |= PAR_INTR | (uint32_t)ppfs->output[ppfs->output_position] << PAR_DATA_SHIFT;
    return value;
}

void ppfs_write_register(ppfs_t *ppfs, uint32_t value) {
    if (value & PAR_NACK) {
        if (ppfs->send_phase == 1) {
            ppfs->send_phase = 2;
            receive_byte(ppfs, ppfs->latched);
        }
        return;
    }
    if (value & PAR_EN) return;
    if ((value & PAR_AUTOEN) && !(value & PAR_NFAULT)) {
        if (ppfs->output_position < ppfs->output_length) ppfs->output_position++;
        return;
    }
    if (value & PAR_AUTOEN) {
        ppfs->send_phase = 0;
        return;
    }
    if (value & PAR_BUSY) {
        ppfs->latched = (uint8_t)value;
        ppfs->send_phase = 1;
    }
}
