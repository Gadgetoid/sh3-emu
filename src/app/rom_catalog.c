#include "app/rom_catalog.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "core/screen.h"
#include "util/file.h"

#define ROM_MIN_BYTES   (1024 * 1024)
#define ROM_MAX_BYTES   (64 * 1024 * 1024)
#define ROM_PROBE_CACHE 32

typedef struct {
    char path[1024];
    off_t size;
    time_t modified;
    int system;
    uint32_t screens;
} rom_probe_t;

static rom_probe_t rom_probes[ROM_PROBE_CACHE];
static int rom_probe_count = 0;
static int rom_probe_next = 0;

static int inspect_rom(const char *path, uint32_t *screens) {
    *screens = 0;
    size_t size;
    uint8_t *rom = file_read(path, &size);
    if (!rom) return 0;
    char error[256];
    machine_t *machine = machine_create(rom, size, error, sizeof error);
    free(rom);
    if (!machine) return 0;
    int system = machine_rom_system(machine);
    for (int i = 0; i < SCREEN_PRESET_COUNT; i++) {
        if (machine_screen_supported(machine, SCREEN_PRESETS[i])) *screens |= 1u << i;
    }
    machine_destroy(machine);
    return system;
}

static int cached_rom_system(const char *path, const struct stat *info, uint32_t *screens) {
    rom_probe_t *probe = NULL;
    for (int i = 0; i < rom_probe_count && !probe; i++) {
        if (!strcmp(rom_probes[i].path, path)) probe = &rom_probes[i];
    }
    if (probe && probe->size == info->st_size && probe->modified == info->st_mtime) {
        *screens = probe->screens;
        return probe->system;
    }
    if (!probe) {
        if (rom_probe_count < ROM_PROBE_CACHE) {
            probe = &rom_probes[rom_probe_count++];
        } else {
            probe = &rom_probes[rom_probe_next];
            rom_probe_next = (rom_probe_next + 1) % ROM_PROBE_CACHE;
        }
        snprintf(probe->path, sizeof probe->path, "%s", path);
    }
    probe->size = info->st_size;
    probe->modified = info->st_mtime;
    probe->system = inspect_rom(path, &probe->screens);
    *screens = probe->screens;
    return probe->system;
}

int rom_catalog_probe(const char *path, uint32_t *screens) {
    struct stat info;
    uint32_t supported_screens = 0;
    int system = 0;
    if (stat(path, &info) == 0 && S_ISREG(info.st_mode) && info.st_size >= ROM_MIN_BYTES && info.st_size <= ROM_MAX_BYTES) system = cached_rom_system(path, &info, &supported_screens);
    if (screens) *screens = supported_screens;
    return system;
}

void rom_catalog_find(rom_set_t *roms, const char *folder) {
    memset(roms, 0, sizeof *roms);
    DIR *dir = opendir(folder);
    if (!dir) return;
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        if (entry->d_name[0] == '.') continue;
        char path[sizeof roms->path[0]];
        if (snprintf(path, sizeof path, "%s/%s", folder, entry->d_name) >= (int)sizeof path) continue;
        struct stat info;
        if (stat(path, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size < ROM_MIN_BYTES || info.st_size > ROM_MAX_BYTES) continue;
        uint32_t screens;
        int system = cached_rom_system(path, &info, &screens);
        size_t size = (size_t)info.st_size;
        if (!system || size <= roms->size[system]) continue;
        memcpy(roms->path[system], path, sizeof path);
        roms->size[system] = size;
    }
    closedir(dir);
}

uint32_t rom_catalog_label(const char *path, char *label, size_t label_size) {
    uint32_t screens;
    int system = rom_catalog_probe(path, &screens);
    if (!system) return 0;
    snprintf(label, label_size, "%s: %s", machine_board_name(system), file_leaf_name(path));
    return screens;
}

screen_size_t rom_catalog_screen(const char *path, screen_size_t preferred) {
    uint32_t screens = 0;
    if (!rom_catalog_probe(path, &screens)) return preferred;
    for (int i = 0; i < SCREEN_PRESET_COUNT; i++) {
        if (SCREEN_PRESETS[i].width == preferred.width && SCREEN_PRESETS[i].height == preferred.height && (screens & (1u << i))) return preferred;
    }
    for (int i = 0; i < SCREEN_PRESET_COUNT; i++) {
        if (screens & (1u << i)) return SCREEN_PRESETS[i];
    }
    return preferred;
}
