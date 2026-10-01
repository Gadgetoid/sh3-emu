#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "desktop.h"
#include "lcd.h"
#include "machine.h"
#include "menu.h"
#include "netgw.h"
#include "options.h"
#include "png.h"
#include "typer.h"
#include "view.h"
#include "rapi.h"

#include <dirent.h>
#include <fcntl.h>
#include <strings.h>
#include <sys/stat.h>
#include <spawn.h>
#include <termios.h>
#include <unistd.h>

#define WINDOW_SCALE     2
#define MAX_FRAME_SLICE  0.1
#define AUTOSAVE_SECONDS 60
#define NOTICE_SECONDS   2
#define POWER_PRESS_SECONDS 0.2
#define BACKLIGHT_PRESS_SECONDS 0.1
#define AUDIO_CHUNK 8192
#define WINDOW_TITLE     "Philips Velo 1"

typedef struct {
    SDL_Keycode key;
    uint8_t     scancode;
} key_binding_t;

static const key_binding_t key_bindings[] = {
    { SDLK_A, 0x14 }, { SDLK_B, 0x2B }, { SDLK_C, 0x2A }, { SDLK_D, 0x2C }, { SDLK_E, 0x28 },
    { SDLK_F, 0x34 }, { SDLK_G, 0x38 }, { SDLK_H, 0x40 }, { SDLK_I, 0x45 }, { SDLK_J, 0x3C },
    { SDLK_K, 0x44 }, { SDLK_L, 0x36 }, { SDLK_M, 0x3B }, { SDLK_N, 0x33 }, { SDLK_O, 0x3E },
    { SDLK_P, 0x4D }, { SDLK_Q, 0x26 }, { SDLK_R, 0x30 }, { SDLK_S, 0x24 }, { SDLK_T, 0x2D },
    { SDLK_U, 0x3D }, { SDLK_V, 0x23 }, { SDLK_W, 0x18 }, { SDLK_X, 0x22 }, { SDLK_Y, 0x35 },
    { SDLK_Z, 0x12 },
    { SDLK_0, 0x47 }, { SDLK_1, 0x13 }, { SDLK_2, 0x16 }, { SDLK_3, 0x15 }, { SDLK_4, 0x25 },
    { SDLK_5, 0x17 }, { SDLK_6, 0x27 }, { SDLK_7, 0x2F }, { SDLK_8, 0x37 }, { SDLK_9, 0x3F },
    { SDLK_SPACE, 0x21 }, { SDLK_TAB, 0x11 }, { SDLK_BACKSPACE, 0x39 }, { SDLK_RETURN, 0x4B },
    { SDLK_ESCAPE, 0x29 }, { SDLK_LSHIFT, 0x51 }, { SDLK_RSHIFT, 0x51 }, { SDLK_LCTRL, 0x01 },
    { SDLK_RCTRL, 0x01 }, { SDLK_LALT, 0x19 }, { SDLK_RALT, 0x09 },
    { SDLK_LEFT, 0x41 }, { SDLK_UP, 0x4A }, { SDLK_RIGHT, 0x32 }, { SDLK_DOWN, 0x49 },
    { SDLK_SEMICOLON, 0x4C }, { SDLK_EQUALS, 0x4F }, { SDLK_COMMA, 0x43 }, { SDLK_MINUS, 0x4E },
    { SDLK_PERIOD, 0x3A }, { SDLK_SLASH, 0x42 }, { SDLK_GRAVE, 0x31 }, { SDLK_LEFTBRACKET, 0x48 },
    { SDLK_BACKSLASH, 0x50 }, { SDLK_RIGHTBRACKET, 0x46 }, { SDLK_APOSTROPHE, 0x2E },
};

static bool find_scancode(SDL_Keycode key, uint8_t *scancode) {
    for (size_t i = 0; i < sizeof key_bindings / sizeof key_bindings[0]; i++) {
        if (key_bindings[i].key == key) { *scancode = key_bindings[i].scancode; return true; }
    }
    return false;
}

static uint8_t *read_file(const char *path, size_t *size) {
    FILE *file = fopen(path, "rb");
    if (!file) return NULL;
    fseek(file, 0, SEEK_END);
    long length = ftell(file);
    fseek(file, 0, SEEK_SET);
    uint8_t *data = malloc((size_t)length);
    if (fread(data, 1, (size_t)length, file) != (size_t)length) { free(data); fclose(file); return NULL; }
    fclose(file);
    *size = (size_t)length;
    return data;
}

static bool verbose = false;

static void release_keys(machine_t *machine, bool *held, int only_modifiers_up) {
    static const struct { uint8_t scancode; int modifier; } modifiers[] = {
        { 0x51, MENU_MOD_SHIFT }, { 0x01, MENU_MOD_CONTROL }, { 0x19, MENU_MOD_ALT }, { 0x09, MENU_MOD_ALT },
    };
    if (only_modifiers_up < 0) {
        for (int i = 0; i < 256; i++) {
            if (!held[i]) continue;
            held[i] = false;
            machine_key(machine, (uint8_t)i, true);
        }
        return;
    }
    if (!(only_modifiers_up & MENU_MOD_KNOWN)) return;
    for (size_t i = 0; i < sizeof modifiers / sizeof modifiers[0]; i++) {
        uint8_t scancode = modifiers[i].scancode;
        if (held[scancode] && !(only_modifiers_up & modifiers[i].modifier)) {
            held[scancode] = false;
            machine_key(machine, scancode, true);
        }
    }
}

typedef enum { SERIAL_OFF, SERIAL_NETWORK, SERIAL_PTY, SERIAL_DEVICE } serial_mode_t;

#define SERIAL_QUEUE 65536

typedef struct {
    serial_mode_t mode;
    netgw_t *gateway;
    int      pty;
    int      pty_slave;
    char     pty_name[128];
    const char *user_agent;
    const char *device;
    uint32_t baud;
    uint8_t  queue[SERIAL_QUEUE];
    size_t   queued;
} serial_t;

#define SERIAL_PORT_MAX   16
#define PORT_SCAN_SECONDS 2.0

static bool is_serial_port(const char *name) {
    return !strncmp(name, "cu.", 3) || !strncmp(name, "ttyUSB", 6) || !strncmp(name, "ttyACM", 6);
}

static int compare_names(const void *a, const void *b) {
    return strcmp((const char *)a, (const char *)b);
}

static int list_serial_ports(char ports[][64], int max) {
    DIR *dev = opendir("/dev");
    if (!dev) return 0;
    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(dev)) && count < max) {
        if (!is_serial_port(entry->d_name) || strlen(entry->d_name) + 6 > 64) continue;
        snprintf(ports[count++], 64, "/dev/%s", entry->d_name);
    }
    closedir(dev);
    qsort(ports, (size_t)count, 64, compare_names);
    return count;
}

static speed_t speed_for(uint32_t baud) {
    static const struct { uint32_t baud; speed_t speed; } speeds[] = {
        { 300, B300 }, { 1200, B1200 }, { 2400, B2400 }, { 4800, B4800 }, { 9600, B9600 },
        { 19200, B19200 }, { 38400, B38400 }, { 57600, B57600 }, { 115200, B115200 },
    };
    speed_t best = B9600;
    uint32_t best_error = UINT32_MAX;
    for (size_t i = 0; i < sizeof speeds / sizeof speeds[0]; i++) {
        uint32_t error = speeds[i].baud > baud ? speeds[i].baud - baud : baud - speeds[i].baud;
        if (error < best_error) { best_error = error; best = speeds[i].speed; }
    }
    return best;
}

static void device_follow_baud(serial_t *serial, machine_t *machine) {
    uint32_t baud = machine_serial_baud(machine);
    if (serial->mode != SERIAL_DEVICE || !baud || baud == serial->baud) return;
    struct termios settings;
    if (tcgetattr(serial->pty, &settings) != 0) return;
    cfsetispeed(&settings, speed_for(baud));
    cfsetospeed(&settings, speed_for(baud));
    tcsetattr(serial->pty, TCSANOW, &settings);
    serial->baud = baud;
    if (verbose) fprintf(stderr, "serial: %s at %u baud\n", serial->device, baud);
}

static void serial_log(const char *message) {
    if (verbose) fputs(message, stderr);
}

static void serial_close(serial_t *serial, machine_t *machine) {
    if (serial->gateway) netgw_destroy(serial->gateway);
    if (serial->pty >= 0) close(serial->pty);
    if (serial->pty_slave >= 0) close(serial->pty_slave);
    serial->gateway = NULL;
    serial->pty = -1;
    serial->pty_slave = -1;
    serial->queued = 0;
    serial->baud = 0;
    serial->mode = SERIAL_OFF;
    machine_serial_connect(machine, false);
}

static const char *serial_open(serial_t *serial, machine_t *machine, serial_mode_t mode) {
    serial_close(serial, machine);
    if (mode == SERIAL_NETWORK) {
        char rapi_socket[1024];
        rapi_data_path("rapi.sock", rapi_socket, sizeof rapi_socket);
        netgw_options_t options = { serial->user_agent, rapi_socket };
        serial->gateway = netgw_create(serial_log, &options);
        if (!serial->gateway) return "built without libslirp";
    } else if (mode == SERIAL_PTY) {
        int fd = posix_openpt(O_RDWR | O_NOCTTY);
        if (fd < 0 || grantpt(fd) != 0 || unlockpt(fd) != 0) {
            if (fd >= 0) close(fd);
            return "could not open a pseudo-terminal";
        }
        snprintf(serial->pty_name, sizeof serial->pty_name, "%s", ptsname(fd));
        int slave = open(serial->pty_name, O_RDWR | O_NOCTTY);
        struct termios settings;
        tcgetattr(slave, &settings);
        cfmakeraw(&settings);
        tcsetattr(slave, TCSANOW, &settings);
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
        serial->pty = fd;
        serial->pty_slave = slave;
        fprintf(stderr, "serial: COM1 on %s\n", serial->pty_name);
    } else if (mode == SERIAL_DEVICE) {
        if (!serial->device || !serial->device[0]) return "choose a host serial port first";
        int fd = open(serial->device, O_RDWR | O_NOCTTY | O_NONBLOCK);
        struct termios settings;
        if (fd < 0 || tcgetattr(fd, &settings) != 0) {
            if (fd >= 0) close(fd);
            snprintf(serial->pty_name, sizeof serial->pty_name, "could not open %s", serial->device);
            return serial->pty_name;
        }
        cfmakeraw(&settings);
        settings.c_cflag |= CLOCAL | CREAD;
        settings.c_cflag &= ~(tcflag_t)CRTSCTS;
        cfsetispeed(&settings, B19200);
        cfsetospeed(&settings, B19200);
        tcsetattr(fd, TCSANOW, &settings);
        serial->pty = fd;
        snprintf(serial->pty_name, sizeof serial->pty_name, "COM1 on %s", serial->device);
    }
    serial->mode = mode;
    machine_set_serial_tag(machine, (uint32_t)mode);
    if (mode != SERIAL_OFF) machine_serial_connect(machine, true);
    if (mode == SERIAL_DEVICE) device_follow_baud(serial, machine);
    return mode == SERIAL_NETWORK ? "network cable connected" : mode != SERIAL_OFF ? serial->pty_name : "serial disconnected";
}

static void serial_restored(serial_t *serial, machine_t *machine, uint64_t *reconnect_at, serial_mode_t *reconnect_mode) {
    bool was_connected = machine_serial_connected(machine);
    serial_mode_t mode = (serial_mode_t)machine_serial_tag(machine);
    serial_close(serial, machine);
    *reconnect_at = 0;
    if (was_connected && (mode == SERIAL_NETWORK || mode == SERIAL_PTY || (mode == SERIAL_DEVICE && serial->device && serial->device[0]))) {
        *reconnect_mode = mode;
        *reconnect_at = machine_cycles(machine) + 2ull * MACHINE_CLOCK_HZ;
    }
}

static void serial_pump(serial_t *serial, machine_t *machine) {
    uint8_t buffer[4096];
    size_t count;
    device_follow_baud(serial, machine);
    while ((count = machine_serial_take(machine, buffer, sizeof buffer)) > 0) {
        if (serial->gateway) netgw_from_guest(serial->gateway, buffer, count);
        else if (serial->mode == SERIAL_DEVICE) {
            size_t room = sizeof serial->queue - serial->queued;
            if (count > room) count = room;
            memcpy(serial->queue + serial->queued, buffer, count);
            serial->queued += count;
        }
        else if (serial->pty >= 0 && write(serial->pty, buffer, count) < 0) break;
    }
    if (serial->mode == SERIAL_DEVICE && serial->queued) {
        ssize_t written = write(serial->pty, serial->queue, serial->queued);
        if (written > 0) {
            memmove(serial->queue, serial->queue + written, serial->queued - (size_t)written);
            serial->queued -= (size_t)written;
        }
    }
    if (serial->gateway) {
        netgw_poll(serial->gateway, machine_cycles(machine) / (MACHINE_CLOCK_HZ / 1000));
        while ((count = netgw_to_guest(serial->gateway, buffer, sizeof buffer)) > 0) machine_serial_send(machine, buffer, count);
    } else if (serial->pty >= 0) {
        ssize_t got;
        while ((got = read(serial->pty, buffer, sizeof buffer)) > 0) machine_serial_send(machine, buffer, (size_t)got);
    }
}
static char chosen_card[1024];
static bool card_chosen = false;

static void card_dialog_done(void *userdata, const char *const *files, int filter) {
    (void)userdata;
    (void)filter;
    if (!files || !files[0]) return;
    snprintf(chosen_card, sizeof chosen_card, "%s", files[0]);
    card_chosen = true;
}

typedef enum { PICK_SEND = 1, PICK_FETCH, PICK_SHARED, PICK_SAVE_SNAPSHOT, PICK_LOAD_SNAPSHOT } pick_kind_t;

#define PICK_MAX 64

static char picked[PICK_MAX][1024];
static int picked_count = 0;
static pick_kind_t picked_kind;
static bool picked_ready = false;

static void pick_done(void *userdata, const char *const *files, int filter) {
    (void)filter;
    if (!files || !files[0]) return;
    picked_count = 0;
    while (files[picked_count] && picked_count < PICK_MAX) {
        snprintf(picked[picked_count], sizeof picked[0], "%s", files[picked_count]);
        picked_count++;
    }
    picked_kind = (pick_kind_t)(intptr_t)userdata;
    picked_ready = true;
}

static const char *leaf_name(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash && slash[1] ? slash + 1 : path;
}

typedef struct {
    char paths[PICK_MAX][1024];
    int  count;
} dropped_t;

static bool has_extension(const char *path, const char *extension) {
    const char *dot = strrchr(path, '.');
    return dot && !strcasecmp(dot, extension);
}

static bool is_directory(const char *path) {
    struct stat info;
    return stat(path, &info) == 0 && S_ISDIR(info.st_mode);
}

static const char *handle_drop(dropped_t *dropped, machine_t *machine, desktop_t *desktop, bool online) {
    static char message[1200];
    int files = 0, scripts = -1, cards = -1;
    const char *list[PICK_MAX + 1];
    for (int i = 0; i < dropped->count; i++) {
        const char *path = dropped->paths[i];
        if (is_directory(path)) continue;
        if (has_extension(path, ".img") && cards < 0) cards = i;
        else if (has_extension(path, ".load") && scripts < 0) scripts = i;
        list[files++] = path;
    }
    list[files] = NULL;
    dropped->count = 0;
    if (cards >= 0 && files == 1) {
        snprintf(message, sizeof message, machine_insert_card(machine, list[0]) ? "inserted %s" : "could not open %s", leaf_name(list[0]));
        return message;
    }
    if (!files) return "drop files, a .load script or a card image";
    if (!online) return "connect Serial > Network (PPP) to send files to the Velo";
    if (scripts >= 0) {
        snprintf(message, sizeof message, "installing %s", leaf_name(dropped->paths[scripts]));
        return desktop_load(desktop, dropped->paths[scripts]) ? message : "busy with the last transfer";
    }
    return desktop_send(desktop, list) ? "sending to \\My Documents" : "busy with the last transfer";
}

static void log_message(const char *message) {
    if (verbose) fputs(message, stderr);
}

static void data_folder(char *path, size_t size) {
    rapi_data_path("", path, size);
    size_t length = strlen(path);
    if (length > 1 && path[length - 1] == '/') path[length - 1] = 0;
    SDL_CreateDirectory(path);
}

static void migrate_old_folders(void) {
#ifdef __APPLE__
    if (getenv("XDG_DATA_HOME") || getenv("XDG_CONFIG_HOME")) return;
    const char *home = getenv("HOME");
    if (!home) return;
    char folder[1024], old_data[1024], old_settings[1024], settings[1100];
    rapi_data_path("", folder, sizeof folder);
    folder[strlen(folder) - 1] = 0;
    snprintf(old_data, sizeof old_data, "%s/.local/share/velo-emu", home);
    snprintf(old_settings, sizeof old_settings, "%s/.config/velo-emu/emu.ini", home);
    struct stat info;
    if (stat(folder, &info) != 0 && stat(old_data, &info) == 0) {
        char parent[1100];
        snprintf(parent, sizeof parent, "%s/Library/Application Support", home);
        SDL_CreateDirectory(parent);
        if (rename(old_data, folder) == 0) fprintf(stderr, "moved %s to %s\n", old_data, folder);
    }
    SDL_CreateDirectory(folder);
    snprintf(settings, sizeof settings, "%s/emu.ini", folder);
    if (stat(settings, &info) != 0 && stat(old_settings, &info) == 0 && rename(old_settings, settings) == 0) {
        fprintf(stderr, "moved %s to %s\n", old_settings, settings);
    }
#endif
}

static void snapshot_folder(char *path, size_t size) {
    char base[1024];
    data_folder(base, sizeof base);
    snprintf(path, size, "%s/snapshots", base);
    SDL_CreateDirectory(path);
}

static void snapshot_default_name(char *path, size_t size) {
    char folder[1100];
    snapshot_folder(folder, sizeof folder);
    time_t now = time(NULL);
    struct tm local;
    localtime_r(&now, &local);
    char stamp[64];
    strftime(stamp, sizeof stamp, "%Y-%m-%d at %H.%M.%S", &local);
    snprintf(path, size, "%s/Snapshot %s.state", folder, stamp);
}

static void state_path(char *path, size_t size, machine_t *machine, const char *rom_path) {
    char base[1024];
    data_folder(base, sizeof base);
    char rom_name[256];
    snprintf(rom_name, sizeof rom_name, "%s", leaf_name(rom_path));
    char *extension = strrchr(rom_name, '.');
    if (extension && extension != rom_name) *extension = 0;
    snprintf(path, size, "%s/state-%s-%08x.bin", base, rom_name, (uint32_t)machine_rom_hash(machine));
    FILE *existing = fopen(path, "rb");
    if (existing) { fclose(existing); return; }
    char legacy[1100];
    snprintf(legacy, sizeof legacy, "%s/state.bin", base);
    if (machine_state_matches(machine, legacy)) rename(legacy, path);
}

typedef struct {
    uint32_t memory;
    uint32_t speed;
    uint32_t host_time;
    uint32_t scale;
    uint32_t connect_at_launch;
    uint32_t system;
    char     serial_device[1024];
    uint32_t display;
    char     user_agent[256];
    char     shared_folder[1024];
} settings_t;

static void settings_path(char *path, size_t size) {
    const char *config_home = getenv("XDG_CONFIG_HOME");
    char base[1024];
    if (config_home && config_home[0] == '/') snprintf(base, sizeof base, "%s/velo-emu", config_home);
#ifdef __APPLE__
    else data_folder(base, sizeof base);
#else
    else snprintf(base, sizeof base, "%s/.config/velo-emu", getenv("HOME") ? getenv("HOME") : ".");
#endif
    SDL_CreateDirectory(base);
    snprintf(path, size, "%s/emu.ini", base);
}

static const uint32_t SCALES[] = { 50, 75, 100, 150, 200 };
#define SCALE_COUNT (int)(sizeof SCALES / sizeof SCALES[0])

static int scale_index(uint32_t scale) {
    for (int i = 0; i < SCALE_COUNT; i++) {
        if (SCALES[i] == scale) return i;
    }
    return -1;
}

static settings_t settings_load(void) {
    settings_t settings = { .memory = 4, .speed = 1, .host_time = 1, .scale = 100, .display = VIEW_SIMULATED, .user_agent = NETGW_DEFAULT_USER_AGENT };
    char path[1100];
    settings_path(path, sizeof path);
    FILE *file = fopen(path, "r");
    if (!file) return settings;
    char line[1200];
    unsigned value;
    while (fgets(line, sizeof line, file)) {
        if (sscanf(line, "memory=%u", &value) == 1) settings.memory = value;
        else if (sscanf(line, "speed=%u", &value) == 1) settings.speed = value;
        else if (sscanf(line, "host_time=%u", &value) == 1) settings.host_time = value;
        else if (sscanf(line, "scale=%u", &value) == 1 && scale_index(value) >= 0) settings.scale = value;
        else if (sscanf(line, "connect_at_launch=%u", &value) == 1) settings.connect_at_launch = value;
        else if (sscanf(line, "system=%u", &value) == 1) settings.system = value;
        else if (sscanf(line, "display=%u", &value) == 1 && value <= VIEW_SHARP) settings.display = value;
        else if (!strncmp(line, "user_agent=", 11)) {
            line[strcspn(line, "\r\n")] = 0;
            snprintf(settings.user_agent, sizeof settings.user_agent, "%s", line + 11);
        } else if (!strncmp(line, "serial_device=", 14)) {
            line[strcspn(line, "\r\n")] = 0;
            snprintf(settings.serial_device, sizeof settings.serial_device, "%s", line + 14);
        } else if (!strncmp(line, "shared_folder=", 14)) {
            line[strcspn(line, "\r\n")] = 0;
            snprintf(settings.shared_folder, sizeof settings.shared_folder, "%s", line + 14);
        }
    }
    fclose(file);
    return settings;
}

static void settings_save(const settings_t *settings) {
    char path[1100];
    settings_path(path, sizeof path);
    FILE *file = fopen(path, "w");
    if (!file) return;
    fprintf(file, "memory=%u\nspeed=%u\nhost_time=%u\nscale=%u\ndisplay=%u\nconnect_at_launch=%u\nsystem=%u\nserial_device=%s\nuser_agent=%s\nshared_folder=%s\n", settings->memory,
            settings->speed, settings->host_time, settings->scale, settings->display, settings->connect_at_launch, settings->system, settings->serial_device, settings->user_agent,
            settings->shared_folder);
    fclose(file);
}

static bool confirm_reset(SDL_Window *window) {
    const SDL_MessageBoxButtonData buttons[] = {
        { SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "Cancel" },
        { SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "Reset" },
    };
    const SDL_MessageBoxData dialog = {
        SDL_MESSAGEBOX_WARNING, window, "Reset the Velo?",
        "A reset is a cold boot: it clears RAM, including files, settings and installed programs. Soft Reset keeps them.",
        (int)(sizeof buttons / sizeof buttons[0]), buttons, NULL,
    };
    int chosen = 0;
    return SDL_ShowMessageBox(&dialog, &chosen) && chosen == 1;
}

static void open_in_finder(const char *path) {
    extern char **environ;
    char *arguments[] = { "open", (char *)path, NULL };
    pid_t pid;
    posix_spawnp(&pid, "open", NULL, NULL, arguments, environ);
}

static void reveal_in_finder(const char *path) {
    extern char **environ;
    char *arguments[] = { "open", "-R", (char *)path, NULL };
    pid_t pid;
    posix_spawnp(&pid, "open", NULL, NULL, arguments, environ);
}

static const char *SYSTEM_NAMES[] = { "", "CE 1.0", "CE 2.0" };

static void set_title(SDL_Window *window, int system, const char *notice, bool paused, bool suspended) {
    char base[64], title[1400];
    snprintf(base, sizeof base, "%s (%s)", WINDOW_TITLE, SYSTEM_NAMES[system]);
    if (notice) snprintf(title, sizeof title, "%s: %s", base, notice);
    else if (paused) snprintf(title, sizeof title, "%s, paused", base);
    else if (suspended) snprintf(title, sizeof title, "%s, suspended", base);
    else snprintf(title, sizeof title, "%s", base);
    if (strcmp(SDL_GetWindowTitle(window), title)) SDL_SetWindowTitle(window, title);
}

typedef struct {
    char   path[3][1024];
    size_t size[3];
} rom_set_t;

static void rom_folder(char *path, size_t size) {
    char base[1024];
    data_folder(base, sizeof base);
    snprintf(path, size, "%s/roms", base);
    SDL_CreateDirectory(path);
}

static int rom_system(const char *path, size_t *size) {
    uint8_t *rom = read_file(path, size);
    if (!rom) return 0;
    char error[256];
    machine_t *machine = machine_create(rom, *size, error, sizeof error);
    free(rom);
    if (!machine) return 0;
    int system = machine_rom_system(machine);
    machine_destroy(machine);
    return system;
}

static void find_roms(rom_set_t *roms) {
    memset(roms, 0, sizeof *roms);
    char folder[1100];
    rom_folder(folder, sizeof folder);
    DIR *dir = opendir(folder);
    if (!dir) return;
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        if (entry->d_name[0] == '.') continue;
        char path[1400];
        snprintf(path, sizeof path, "%s/%s", folder, entry->d_name);
        struct stat info;
        if (stat(path, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size < 1024 * 1024) continue;
        size_t size;
        int system = rom_system(path, &size);
        if (!system || size <= roms->size[system]) continue;
        snprintf(roms->path[system], sizeof roms->path[system], "%s", path);
        roms->size[system] = size;
    }
    closedir(dir);
}

static bool no_roms_dialog(void) {
    char folder[1100], message[1400];
    rom_folder(folder, sizeof folder);
    snprintf(message, sizeof message, "Put a Velo 1 ROM in %s: the CE 1.0 nk.bin, the merged CE 2.0 image, or both.", folder);
    const SDL_MessageBoxButtonData buttons[] = {
        { SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "Quit" },
        { SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "Show ROM Folder" },
    };
    const SDL_MessageBoxData dialog = { SDL_MESSAGEBOX_INFORMATION, NULL, "No Velo ROM found", message, 2, buttons, NULL };
    int chosen = 0;
    if (SDL_ShowMessageBox(&dialog, &chosen) && chosen == 1) open_in_finder(folder);
    return false;
}

static machine_t *start_machine(const char *rom_path, const settings_t *settings, const char *state_file, bool fresh,
                                char *state, size_t state_size, const char **notice) {
    static char message[1400];
    *notice = NULL;
    size_t rom_size;
    uint8_t *rom = read_file(rom_path, &rom_size);
    if (!rom) {
        snprintf(message, sizeof message, "cannot read %s", rom_path);
        *notice = message;
        return NULL;
    }
    char error[256];
    machine_t *machine = machine_create(rom, rom_size, error, sizeof error);
    free(rom);
    if (!machine) {
        snprintf(message, sizeof message, "%s", error);
        *notice = message;
        return NULL;
    }
    machine_set_log(machine, log_message);
    machine_set_memory(machine, settings->memory);
    machine_set_speed(machine, settings->speed);
    machine_set_host_clock(machine, settings->host_time != 0);
    if (state_file) snprintf(state, state_size, "%s", state_file);
    else state_path(state, state_size, machine, rom_path);
    if (fresh) return machine;
    int64_t saved_at;
    if (machine_load(machine, state, &saved_at)) {
        machine_advance_clock(machine, (int64_t)time(NULL) - saved_at);
        return machine;
    }
    FILE *existing = fopen(state, "rb");
    if (existing) {
        fclose(existing);
        char backup[1200];
        snprintf(backup, sizeof backup, "%s.old", state);
        rename(state, backup);
        snprintf(message, sizeof message, "saved state unreadable, moved to %s", leaf_name(backup));
        *notice = message;
    }
    return machine;
}

static const void *clipboard_png(void *userdata, const char *mime_type, size_t *size) {
    const size_t *stored = userdata;
    if (strcmp(mime_type, "image/png")) { *size = 0; return NULL; }
    *size = stored[0];
    return stored + 1;
}

static bool copy_screen(view_t *view) {
    int width, height;
    const uint32_t *pixels = view_image(view, &width, &height);
    uint8_t *png;
    size_t length;
    if (!pixels || !png_encode(pixels, width, height, &png, &length)) return false;
    size_t *stored = malloc(sizeof(size_t) + length);
    if (!stored) { free(png); return false; }
    stored[0] = length;
    memcpy(stored + 1, png, length);
    free(png);
    const char *types[] = { "image/png" };
    if (SDL_SetClipboardData(clipboard_png, free, stored, types, 1)) return true;
    free(stored);
    return false;
}

static bool save_screenshot(view_t *view, char *path, size_t size) {
    int width, height;
    const uint32_t *pixels = view_image(view, &width, &height);
    uint8_t *png;
    size_t length;
    if (!pixels || !png_encode(pixels, width, height, &png, &length)) return false;
    time_t now = time(NULL);
    struct tm local;
    localtime_r(&now, &local);
    char stamp[64];
    strftime(stamp, sizeof stamp, "%Y-%m-%d at %H.%M.%S", &local);
    snprintf(path, size, "%s/Desktop/Velo Screenshot %s.png", getenv("HOME") ? getenv("HOME") : ".", stamp);
    FILE *file = fopen(path, "wb");
    bool saved = file && fwrite(png, 1, length, file) == length;
    if (file) fclose(file);
    free(png);
    return saved;
}

static void window_size(view_display_t display, uint32_t scale, int *width, int *height) {
    view_source_size(display, width, height);
    *width = *width * WINDOW_SCALE * (int)scale / 100;
    *height = *height * WINDOW_SCALE * (int)scale / 100;
}

static void fit_window(SDL_Window *window, view_t *view, uint32_t scale) {
    if (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) SDL_SetWindowFullscreen(window, false);
    int width, height;
    window_size(view_display(view), scale, &width, &height);
    SDL_SetWindowSize(window, width, height);
}

typedef struct {
    settings_t   *settings;
    serial_mode_t serial_mode;
    const char   *card, *state_file;
    bool          fresh;
} launch_t;

enum {
    LAUNCH_HEADING_MACHINE, LAUNCH_STATE, LAUNCH_FRESH, LAUNCH_CARD, LAUNCH_MEMORY, LAUNCH_SPEED,
    LAUNCH_HEADING_CONNECTIONS, LAUNCH_SERIAL, LAUNCH_USER_AGENT,
    LAUNCH_HEADING_DEBUGGING, LAUNCH_VERBOSE,
};

static const option_t LAUNCH_OPTIONS[] = {
    [LAUNCH_HEADING_MACHINE] = { NULL, NULL, "Machine", 0 },
    [LAUNCH_STATE] = { "state", "FILE", "load, save and autosave FILE instead of the ROM's own state", 0 },
    [LAUNCH_FRESH] = { "fresh", NULL, "ignore the saved state and cold boot", 0 },
    [LAUNCH_CARD] = { "card", "IMAGE", "insert a PC Card image", 0 },
    [LAUNCH_MEMORY] = { "memory", "MB", "RAM for the next cold boot: 4, 8, 16, 20 or 32", 0 },
    [LAUNCH_SPEED] = { "speed", "N", "CPU speed multiple: 1, 2, 4 or 8", 0 },
    [LAUNCH_HEADING_CONNECTIONS] = { NULL, NULL, "Connections", 0 },
    [LAUNCH_SERIAL] = { "serial", "net|pty|off|PORT", "COM1 on the PPP network, a pseudo-terminal, nothing, or a host serial port such as /dev/cu.usbserial-1", 0 },
    [LAUNCH_USER_AGENT] = { "user-agent", "TEXT", "the web proxy's user agent", 0 },
    [LAUNCH_HEADING_DEBUGGING] = { NULL, NULL, "Debugging", 0 },
    [LAUNCH_VERBOSE] = { "verbose", NULL, "log hardware, network and proxy activity to stderr", 0 },
};

static bool launch_option(void *context, int option, const char *value, char *error, size_t error_size) {
    launch_t *launch = context;
    settings_t *settings = launch->settings;
    long integer;
    (void)error;
    (void)error_size;
    switch (option) {
    case LAUNCH_STATE: launch->state_file = value; return true;
    case LAUNCH_FRESH: launch->fresh = true; return true;
    case LAUNCH_CARD: launch->card = value; return true;
    case LAUNCH_MEMORY:
        if (!option_integer(value, 10, &integer) || (integer != 4 && integer != 8 && integer != 16 && integer != 20 && integer != 32)) return false;
        settings->memory = (uint32_t)integer;
        return true;
    case LAUNCH_SPEED:
        if (!option_integer(value, 10, &integer) || (integer != 1 && integer != 2 && integer != 4 && integer != 8)) return false;
        settings->speed = (uint32_t)integer;
        return true;
    case LAUNCH_SERIAL:
        if (!strcmp(value, "net")) launch->serial_mode = SERIAL_NETWORK;
        else if (!strcmp(value, "pty")) launch->serial_mode = SERIAL_PTY;
        else if (!strcmp(value, "off")) launch->serial_mode = SERIAL_OFF;
        else if (value[0] == '/') {
            snprintf(settings->serial_device, sizeof settings->serial_device, "%s", value);
            launch->serial_mode = SERIAL_DEVICE;
        } else return false;
        return true;
    case LAUNCH_USER_AGENT: snprintf(settings->user_agent, sizeof settings->user_agent, "%s", value); return true;
    case LAUNCH_VERBOSE: verbose = true; return true;
    }
    return false;
}

static const option_spec_t LAUNCH_SPEC = {
    "velo", "[OPTIONS] [ROM]",
    "Emulates a Philips Velo 1. With no ROM it opens the last system used from the roms folder in its data folder; Run > System switches between Windows CE 1.0 and 2.0.",
    LAUNCH_OPTIONS, (int)(sizeof LAUNCH_OPTIONS / sizeof LAUNCH_OPTIONS[0]),
    "headless runs the machine without a window, for tests and scripts, and velo-rapi talks to a running Velo.",
};

int main(int argc, char **argv) {
    const char *rom_path = NULL;
    migrate_old_folders();
    settings_t settings = settings_load();
    launch_t launch = { &settings, settings.connect_at_launch ? SERIAL_NETWORK : SERIAL_OFF, NULL, NULL, false };
    const char *positional[1];
    int positional_count;
    options_result_t parsed = options_parse(&LAUNCH_SPEC, argc, argv, launch_option, &launch, positional, 1, &positional_count);
    if (parsed == OPTIONS_EXIT) return 0;
    if (parsed == OPTIONS_ERROR) return 2;
    if (positional_count) rom_path = positional[0];
    serial_mode_t serial_mode = launch.serial_mode;
    const char *card = launch.card, *state_file = launch.state_file;
    bool fresh = launch.fresh;
    static rom_set_t roms;
    find_roms(&roms);
    if (!rom_path) {
        int system = settings.system == 1 || settings.system == 2 ? (int)settings.system : 0;
        if (!system || !roms.path[system][0]) system = roms.path[2][0] ? 2 : roms.path[1][0] ? 1 : 0;
        if (!system) return no_roms_dialog() ? 0 : 1;
        rom_path = roms.path[system];
    }
    char state[1100];
    const char *startup_notice = NULL;
    machine_t *machine = start_machine(rom_path, &settings, state_file, fresh, state, sizeof state, &startup_notice);
    if (!machine) { fprintf(stderr, "%s\n", startup_notice); return 1; }
    int system = machine_rom_system(machine);

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) { fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 1; }
    int window_width, window_height;
    window_size((view_display_t)settings.display, settings.scale, &window_width, &window_height);
    SDL_Window *window = SDL_CreateWindow("Philips Velo 1", window_width, window_height, SDL_WINDOW_HIGH_PIXEL_DENSITY);
    SDL_Renderer *renderer = window ? SDL_CreateRenderer(window, NULL) : NULL;
    if (!renderer) { fprintf(stderr, "SDL: %s\n", SDL_GetError()); return 1; }
    SDL_SetRenderVSync(renderer, 1);

    view_t *view = view_create(window, renderer, (view_display_t)settings.display);


    if (card && !machine_insert_card(machine, card)) fprintf(stderr, "cannot open card image %s\n", card);

    menu_install();

    SDL_AudioSpec audio_spec = { SDL_AUDIO_S16, 1, 11025 };
    SDL_AudioStream *audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &audio_spec, NULL, NULL);
    if (audio) SDL_ResumeAudioStreamDevice(audio);
    else if (verbose) fprintf(stderr, "audio: %s\n", SDL_GetError());
    bool sound = true;
    static int16_t samples[AUDIO_CHUNK];

    bool running = true, pen_down = false, paused = false;
    bool held[256] = { false };
    uint64_t last = SDL_GetPerformanceCounter();
    double frequency = (double)SDL_GetPerformanceFrequency();
    double owed = 0, since_autosave = 0, notice_left = 0;
    uint64_t power_release_at = 0, backlight_release_at = 0;
    const char *notice = startup_notice;
    if (notice) notice_left = 6;
    static serial_t serial;
    serial = (serial_t){ SERIAL_OFF, NULL, -1, -1, "", settings.user_agent, settings.serial_device, 0, { 0 }, 0 };
    char rapi_socket[1024], sync_manifest[1024], desktop_notice[256], shared_notice[1200], paste_notice[64];
    static typer_t typer;
    static char ports[SERIAL_PORT_MAX][64];
    int port_count = 0;
    double since_port_scan = 0;
    static dropped_t dropped;
    rapi_data_path("rapi.sock", rapi_socket, sizeof rapi_socket);
    rapi_data_path("sync-manifest.txt", sync_manifest, sizeof sync_manifest);
    desktop_t *desktop = desktop_create(rapi_socket, sync_manifest);
    uint64_t serial_reconnect_at = 0;
    serial_mode_t serial_reconnect_mode = SERIAL_OFF;
    serial_restored(&serial, machine, &serial_reconnect_at, &serial_reconnect_mode);
    if (serial_mode != SERIAL_OFF && serial_reconnect_at) {
        serial_reconnect_mode = serial_mode;
    } else if (serial_mode != SERIAL_OFF) {
        const char *result = serial_open(&serial, machine, serial_mode);
        if (!notice) { notice = result; notice_left = NOTICE_SECONDS * 2; }
    }

    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            switch (event.type) {
            case SDL_EVENT_QUIT:
                running = false;
                break;
            case SDL_EVENT_KEY_DOWN:
            case SDL_EVENT_KEY_UP: {
                bool down = event.type == SDL_EVENT_KEY_DOWN;
                uint8_t scancode;
                if (!find_scancode(event.key.key, &scancode)) break;
                if (down) {
                    if (event.key.repeat || (event.key.mod & SDL_KMOD_GUI) || held[scancode]) break;
                    held[scancode] = true;
                    machine_key(machine, scancode, false);
                } else if (held[scancode]) {
                    held[scancode] = false;
                    machine_key(machine, scancode, true);
                }
                break;
            }
            case SDL_EVENT_DROP_FILE:
                if (event.drop.data && dropped.count < PICK_MAX) snprintf(dropped.paths[dropped.count++], sizeof dropped.paths[0], "%s", event.drop.data);
                break;
            case SDL_EVENT_DROP_COMPLETE:
                if (dropped.count) {
                    bool online = serial.gateway && netgw_online(serial.gateway);
                    notice = handle_drop(&dropped, machine, desktop, online && !desktop_busy(desktop));
                    notice_left = NOTICE_SECONDS * 2;
                }
                break;
            case SDL_EVENT_WINDOW_FOCUS_GAINED:
                find_roms(&roms);
                break;
            case SDL_EVENT_WINDOW_FOCUS_LOST:
                release_keys(machine, held, -1);
                break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
                if (event.button.button == SDL_BUTTON_LEFT) {
                    int x, y;
                    if (view_screen_position(view, event.button.x, event.button.y, &x, &y)) {
                        pen_down = true;
                        machine_touch(machine, true, x, y);
                    }
                }
                break;
            case SDL_EVENT_MOUSE_MOTION:
                if (pen_down) {
                    int x, y;
                    view_screen_position(view, event.motion.x, event.motion.y, &x, &y);
                    machine_touch(machine, true, x, y);
                }
                break;
            case SDL_EVENT_MOUSE_BUTTON_UP:
                if (event.button.button == SDL_BUTTON_LEFT && pen_down) {
                    int x, y;
                    view_screen_position(view, event.button.x, event.button.y, &x, &y);
                    pen_down = false;
                    machine_touch(machine, false, x, y);
                }
                break;
            default:
                break;
            }
        }

        release_keys(machine, held, menu_modifiers());
        for (int item = menu_poll(); item >= 0; item = menu_poll()) {
            release_keys(machine, held, -1);
            switch (item) {
            case MENU_POWER:
                machine_power_button(machine, true);
                power_release_at = machine_cycles(machine) + (uint64_t)(POWER_PRESS_SECONDS * MACHINE_CLOCK_HZ);
                break;
            case MENU_PAUSE: paused = !paused; break;
            case MENU_SOFT_RESET: machine_soft_reset(machine); break;
            case MENU_SYSTEM_CE1:
            case MENU_SYSTEM_CE2: {
                int wanted = item == MENU_SYSTEM_CE1 ? 1 : 2;
                if (wanted == system || !roms.path[wanted][0]) break;
                if (desktop_busy(desktop)) {
                    notice = "busy with a desktop transfer";
                    notice_left = NOTICE_SECONDS;
                    break;
                }
                const char *switch_notice = NULL;
                char next_state[sizeof state];
                machine_t *next = start_machine(roms.path[wanted], &settings, NULL, false, next_state, sizeof next_state, &switch_notice);
                if (!next) {
                    notice = switch_notice;
                    notice_left = NOTICE_SECONDS * 2;
                    break;
                }
                serial_mode_t mode = serial.mode;
                serial_close(&serial, machine);
                if (pen_down) machine_touch(machine, false, 0, 0);
                pen_down = false;
                typer.length = typer.position = 0;
                machine_save(machine, state, (int64_t)time(NULL));
                machine_destroy(machine);
                machine = next;
                memcpy(state, next_state, sizeof state);
                system = wanted;
                settings.system = (uint32_t)wanted;
                settings_save(&settings);
                serial_reconnect_at = 0;
                if (mode != SERIAL_OFF) {
                    serial_reconnect_mode = mode;
                    serial_reconnect_at = machine_cycles(machine) + 2ull * MACHINE_CLOCK_HZ;
                }
                power_release_at = backlight_release_at = 0;
                owed = 0;
                notice = switch_notice ? switch_notice : wanted == 1 ? "switched to CE 1.0" : "switched to CE 2.0";
                notice_left = NOTICE_SECONDS * 2;
                break;
            }
            case MENU_SCALE_50:
            case MENU_SCALE_75:
            case MENU_SCALE_100:
            case MENU_SCALE_150:
            case MENU_SCALE_200:
            case MENU_ZOOM_IN:
            case MENU_ZOOM_OUT: {
                int index = scale_index(settings.scale);
                if (item == MENU_ZOOM_IN) index = index + 1 < SCALE_COUNT ? index + 1 : index;
                else if (item == MENU_ZOOM_OUT) index = index > 0 ? index - 1 : index;
                else index = item - MENU_SCALE_50;
                settings.scale = SCALES[index];
                settings_save(&settings);
                fit_window(window, view, settings.scale);
                break;
            }
            case MENU_FULL_SCREEN:
                SDL_SetWindowFullscreen(window, !(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN));
                break;
            case MENU_DISPLAY_SIMULATED:
            case MENU_DISPLAY_SHARP:
                settings.display = item == MENU_DISPLAY_SHARP ? VIEW_SHARP : VIEW_SIMULATED;
                settings_save(&settings);
                view_set_display(view, (view_display_t)settings.display);
                fit_window(window, view, settings.scale);
                break;
            case MENU_COPY_SCREEN:
                notice = copy_screen(view) ? "screen copied" : "could not copy the screen";
                notice_left = NOTICE_SECONDS;
                break;
            case MENU_SAVE_SCREENSHOT: {
                static char screenshot_notice[1200];
                char path[1100];
                if (save_screenshot(view, path, sizeof path)) snprintf(screenshot_notice, sizeof screenshot_notice, "saved %s", leaf_name(path));
                else snprintf(screenshot_notice, sizeof screenshot_notice, "could not save the screenshot");
                notice = screenshot_notice;
                notice_left = NOTICE_SECONDS * 2;
                break;
            }
            case MENU_CONNECT_AT_LAUNCH:
                settings.connect_at_launch = !settings.connect_at_launch;
                settings_save(&settings);
                break;
            case MENU_PASTE: {
                char *clipboard = SDL_GetClipboardText();
                size_t typed = clipboard ? typer_start(&typer, clipboard) : 0;
                SDL_free(clipboard);
                snprintf(paste_notice, sizeof paste_notice, typed ? "typing %zu characters" : "nothing to type", typed);
                notice = paste_notice;
                notice_left = NOTICE_SECONDS;
                break;
            }
            case MENU_RESET:
                if (confirm_reset(window)) machine_reset(machine);
                break;
            case MENU_SAVE_STATE:
                notice = machine_save(machine, state, (int64_t)time(NULL)) ? "state saved" : "could not save state";
                notice_left = NOTICE_SECONDS;
                break;
            case MENU_LOAD_STATE:
                if (machine_load(machine, state, NULL)) {
                    serial_restored(&serial, machine, &serial_reconnect_at, &serial_reconnect_mode);
                    notice = "state loaded";
                } else {
                    notice = "no saved state";
                }
                notice_left = NOTICE_SECONDS;
                break;
            case MENU_BACKLIGHT:
                machine_backlight_button(machine, true);
                backlight_release_at = machine_cycles(machine) + (uint64_t)(BACKLIGHT_PRESS_SECONDS * MACHINE_CLOCK_HZ);
                break;
            case MENU_SOUND: sound = !sound; break;
            case MENU_SHOW_STATE: reveal_in_finder(state); break;
            case MENU_SAVE_SNAPSHOT: {
                static const SDL_DialogFileFilter filters[] = { { "Velo snapshot", "state" } };
                static char default_snapshot[1200];
                snapshot_default_name(default_snapshot, sizeof default_snapshot);
                SDL_ShowSaveFileDialog(pick_done, (void *)(intptr_t)PICK_SAVE_SNAPSHOT, window, filters, 1, default_snapshot);
                break;
            }
            case MENU_LOAD_SNAPSHOT: {
                static const SDL_DialogFileFilter filters[] = { { "Velo snapshot", "state;bin" } };
                static char folder[1100];
                snapshot_folder(folder, sizeof folder);
                SDL_ShowOpenFileDialog(pick_done, (void *)(intptr_t)PICK_LOAD_SNAPSHOT, window, filters, 1, folder, false);
                break;
            }
            case MENU_SHOW_SNAPSHOTS: {
                char folder[1100];
                snapshot_folder(folder, sizeof folder);
                open_in_finder(folder);
                break;
            }
            case MENU_MEMORY_4:
            case MENU_MEMORY_8:
            case MENU_MEMORY_16:
            case MENU_MEMORY_20:
            case MENU_MEMORY_32:
                settings.memory = item == MENU_MEMORY_4 ? 4 : item == MENU_MEMORY_8 ? 8 : item == MENU_MEMORY_16 ? 16 : item == MENU_MEMORY_20 ? 20 : 32;
                machine_set_memory(machine, settings.memory);
                settings_save(&settings);
                notice = machine_memory(machine) == settings.memory ? "memory unchanged" : "memory changes after Run > Reset (clears the machine)";
                notice_left = NOTICE_SECONDS * 3;
                break;
            case MENU_HOST_TIME:
                settings.host_time = !settings.host_time;
                machine_set_host_clock(machine, settings.host_time != 0);
                settings_save(&settings);
                break;
            case MENU_SPEED_1:
            case MENU_SPEED_2:
            case MENU_SPEED_4:
            case MENU_SPEED_8:
                settings.speed = item == MENU_SPEED_1 ? 1 : item == MENU_SPEED_2 ? 2 : item == MENU_SPEED_4 ? 4 : 8;
                machine_set_speed(machine, settings.speed);
                settings_save(&settings);
                break;
            case MENU_INSERT_CARD: {
                static const SDL_DialogFileFilter filters[] = { { "Card images", "img;bin;raw" }, { "All files", "*" } };
                SDL_ShowOpenFileDialog(card_dialog_done, NULL, window, filters, 2, NULL, false);
                break;
            }
            default:
                if (item >= MENU_SERIAL_PORT_FIRST && item <= MENU_SERIAL_PORT_LAST && item - MENU_SERIAL_PORT_FIRST < port_count) {
                    snprintf(settings.serial_device, sizeof settings.serial_device, "%s", ports[item - MENU_SERIAL_PORT_FIRST]);
                    settings_save(&settings);
                    serial_reconnect_at = 0;
                    notice = serial_open(&serial, machine, SERIAL_DEVICE);
                    notice_left = NOTICE_SECONDS * 3;
                }
                break;
            case MENU_SERIAL_NETWORK:
            case MENU_SERIAL_PTY:
            case MENU_SERIAL_OFF:
                serial_reconnect_at = 0;
                notice = serial_open(&serial, machine, item == MENU_SERIAL_NETWORK ? SERIAL_NETWORK : item == MENU_SERIAL_PTY ? SERIAL_PTY : SERIAL_OFF);
                notice_left = NOTICE_SECONDS * 3;
                break;
            case MENU_SEND_FILES:
                SDL_ShowOpenFileDialog(pick_done, (void *)(intptr_t)PICK_SEND, window, NULL, 0, NULL, true);
                break;
            case MENU_FETCH_DOCUMENTS:
                SDL_ShowOpenFolderDialog(pick_done, (void *)(intptr_t)PICK_FETCH, window, NULL, false);
                break;
            case MENU_SHARED_FOLDER:
                SDL_ShowOpenFolderDialog(pick_done, (void *)(intptr_t)PICK_SHARED, window, settings.shared_folder[0] ? settings.shared_folder : NULL, false);
                break;
            case MENU_SYNC_NOW:
                desktop_sync(desktop, settings.shared_folder);
                break;
            case MENU_SET_PROXY:
                desktop_set_proxy(desktop);
                break;
            case MENU_BAUD_19200:
            case MENU_BAUD_38400:
            case MENU_BAUD_57600:
            case MENU_BAUD_115200:
                desktop_set_baud(desktop, item == MENU_BAUD_19200 ? 19200 : item == MENU_BAUD_38400 ? 38400 : item == MENU_BAUD_57600 ? 57600 : 115200);
                break;
            case MENU_STOP_SHARING:
                snprintf(shared_notice, sizeof shared_notice, "stopped sharing %s", leaf_name(settings.shared_folder));
                settings.shared_folder[0] = 0;
                settings_save(&settings);
                notice = shared_notice;
                notice_left = NOTICE_SECONDS;
                break;
            case MENU_EJECT_CARD:
                machine_eject_card(machine);
                notice = "card ejected";
                notice_left = NOTICE_SECONDS;
                break;
            }
        }
        if (card_chosen) {
            card_chosen = false;
            notice = machine_insert_card(machine, chosen_card) ? "card inserted" : "could not open card image";
            notice_left = NOTICE_SECONDS;
        }
        bool velo_online = serial.gateway && netgw_online(serial.gateway);
        if (picked_ready) {
            picked_ready = false;
            if (picked_kind == PICK_SEND) {
                const char *files[PICK_MAX + 1];
                for (int i = 0; i < picked_count; i++) files[i] = picked[i];
                files[picked_count] = NULL;
                desktop_send(desktop, files);
            } else if (picked_kind == PICK_SAVE_SNAPSHOT) {
                static char snapshot_notice[1200];
                char path[1100];
                snprintf(path, sizeof path, "%s%s", picked[0], has_extension(picked[0], ".state") ? "" : ".state");
                bool saved = machine_save(machine, path, (int64_t)time(NULL));
                snprintf(snapshot_notice, sizeof snapshot_notice, saved ? "saved snapshot %s" : "could not save %s", leaf_name(path));
                notice = snapshot_notice;
                notice_left = NOTICE_SECONDS * 2;
            } else if (picked_kind == PICK_LOAD_SNAPSHOT) {
                static char snapshot_notice[1200];
                if (machine_load(machine, picked[0], NULL)) {
                    serial_restored(&serial, machine, &serial_reconnect_at, &serial_reconnect_mode);
                    snprintf(snapshot_notice, sizeof snapshot_notice, "loaded snapshot %s", leaf_name(picked[0]));
                } else {
                    snprintf(snapshot_notice, sizeof snapshot_notice, "%s isn't a snapshot of this ROM", leaf_name(picked[0]));
                }
                notice = snapshot_notice;
                notice_left = NOTICE_SECONDS * 2;
            } else if (picked_kind == PICK_FETCH) {
                desktop_fetch(desktop, picked[0]);
            } else if (picked_kind == PICK_SHARED) {
                snprintf(settings.shared_folder, sizeof settings.shared_folder, "%s", picked[0]);
                settings_save(&settings);
                snprintf(shared_notice, sizeof shared_notice, "sharing %s with \\My Documents", leaf_name(settings.shared_folder));
                notice = shared_notice;
                notice_left = NOTICE_SECONDS * 2;
                if (velo_online) desktop_sync(desktop, settings.shared_folder);
            }
        }
        if (serial.gateway && netgw_take_desktop_connected(serial.gateway) && settings.shared_folder[0]) {
            desktop_sync(desktop, settings.shared_folder);
        }
        if (desktop_take_reconnect(desktop) && serial.mode == SERIAL_NETWORK) {
            serial_open(&serial, machine, SERIAL_OFF);
            serial_reconnect_mode = SERIAL_NETWORK;
            serial_reconnect_at = machine_cycles(machine) + 2ull * MACHINE_CLOCK_HZ;
        }
        if (desktop_take_status(desktop, desktop_notice, sizeof desktop_notice)) {
            notice = desktop_notice;
            notice_left = NOTICE_SECONDS * 2;
        }
        bool desktop_free = velo_online && !desktop_busy(desktop);
        menu_ensure();
        menu_set_enabled(MENU_SEND_FILES, desktop_free);
        menu_set_enabled(MENU_FETCH_DOCUMENTS, desktop_free);
        menu_set_enabled(MENU_SYNC_NOW, desktop_free && settings.shared_folder[0]);
        menu_set_enabled(MENU_STOP_SHARING, settings.shared_folder[0] != 0);
        menu_set_enabled(MENU_SET_PROXY, desktop_free);
        for (int baud_item = MENU_BAUD_19200; baud_item <= MENU_BAUD_115200; baud_item++) menu_set_enabled(baud_item, desktop_free);
        menu_set_enabled(MENU_EJECT_CARD, machine_card_inserted(machine));
        menu_set_checked(MENU_SERIAL_NETWORK, serial.mode == SERIAL_NETWORK);
        menu_set_checked(MENU_SERIAL_PTY, serial.mode == SERIAL_PTY);
        if (since_port_scan <= 0) {
            since_port_scan = PORT_SCAN_SECONDS;
            port_count = list_serial_ports(ports, SERIAL_PORT_MAX);
        }
        for (int i = 0; i < SERIAL_PORT_MAX; i++) {
            int port_item = MENU_SERIAL_PORT_FIRST + i;
            bool shown = i < port_count || (i == 0 && port_count == 0);
            menu_set_hidden(port_item, !shown);
            if (!shown) continue;
            menu_set_title(port_item, port_count ? ports[i] + 5 : "No serial ports found");
            menu_set_enabled(port_item, port_count > 0);
            menu_set_checked(port_item, port_count && serial.mode == SERIAL_DEVICE && !strcmp(settings.serial_device, ports[i]));
        }
        menu_set_enabled(MENU_SERIAL_OFF, serial.mode != SERIAL_OFF);
        menu_set_checked(MENU_PAUSE, paused);
        menu_set_checked(MENU_BACKLIGHT, machine_backlight(machine));
        menu_set_checked(MENU_SOUND, sound);
        menu_set_checked(MENU_MEMORY_4, machine_memory_next(machine) == 4);
        menu_set_checked(MENU_MEMORY_8, machine_memory_next(machine) == 8);
        menu_set_checked(MENU_MEMORY_16, machine_memory_next(machine) == 16);
        menu_set_checked(MENU_MEMORY_20, machine_memory_next(machine) == 20);
        menu_set_checked(MENU_MEMORY_32, machine_memory_next(machine) == 32);
        menu_set_checked(MENU_HOST_TIME, settings.host_time != 0);
        menu_set_checked(MENU_SYSTEM_CE1, system == 1);
        menu_set_checked(MENU_SYSTEM_CE2, system == 2);
        menu_set_enabled(MENU_SYSTEM_CE1, system == 1 || roms.path[1][0]);
        menu_set_enabled(MENU_SYSTEM_CE2, system == 2 || roms.path[2][0]);
        menu_set_checked(MENU_CONNECT_AT_LAUNCH, settings.connect_at_launch != 0);
        for (int scale_item = MENU_SCALE_50; scale_item <= MENU_SCALE_200; scale_item++) menu_set_checked(scale_item, settings.scale == SCALES[scale_item - MENU_SCALE_50]);
        menu_set_enabled(MENU_ZOOM_IN, settings.scale < SCALES[SCALE_COUNT - 1]);
        menu_set_enabled(MENU_ZOOM_OUT, settings.scale > SCALES[0]);
        menu_set_checked(MENU_FULL_SCREEN, (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) != 0);
        menu_set_checked(MENU_DISPLAY_SIMULATED, settings.display == VIEW_SIMULATED);
        menu_set_checked(MENU_DISPLAY_SHARP, settings.display == VIEW_SHARP);
        menu_set_checked(MENU_SPEED_1, machine_speed(machine) == 1);
        menu_set_checked(MENU_SPEED_2, machine_speed(machine) == 2);
        menu_set_checked(MENU_SPEED_4, machine_speed(machine) == 4);
        menu_set_checked(MENU_SPEED_8, machine_speed(machine) == 8);

        uint64_t now = SDL_GetPerformanceCounter();
        double elapsed = (double)(now - last) / frequency;
        last = now;
        if (elapsed > MAX_FRAME_SLICE) elapsed = MAX_FRAME_SLICE;
        if (notice_left > 0) {
            notice_left -= elapsed;
            if (notice_left <= 0) notice = NULL;
        }
        set_title(window, system, notice, paused, machine_suspended(machine));
        since_autosave += elapsed;
        since_port_scan -= elapsed;
        if (since_autosave >= AUTOSAVE_SECONDS) {
            since_autosave = 0;
            machine_save(machine, state, (int64_t)time(NULL));
        }
        if (!paused && !machine_halted(machine)) {
            owed += elapsed * MACHINE_CLOCK_HZ;
            uint64_t cycles = (uint64_t)owed;
            owed -= (double)cycles;
            machine_run(machine, cycles);
        }
        typer_step(&typer, machine);
        serial_pump(&serial, machine);
        if (serial_reconnect_at && machine_cycles(machine) >= serial_reconnect_at) {
            serial_reconnect_at = 0;
            notice = serial_open(&serial, machine, serial_reconnect_mode);
            notice_left = NOTICE_SECONDS * 2;
        }
        if (backlight_release_at && machine_cycles(machine) >= backlight_release_at) {
            backlight_release_at = 0;
            machine_backlight_button(machine, false);
        }
        if (power_release_at && machine_cycles(machine) >= power_release_at) {
            power_release_at = 0;
            machine_power_button(machine, false);
        }
        uint32_t rate;
        for (size_t count; (count = machine_audio(machine, samples, AUDIO_CHUNK, &rate)) > 0;) {
            if (!audio || !sound) continue;
            if ((int)rate != audio_spec.freq) {
                audio_spec.freq = (int)rate;
                SDL_SetAudioStreamFormat(audio, &audio_spec, NULL);
            }
            SDL_PutAudioStreamData(audio, samples, (int)(count * sizeof samples[0]));
        }

        lcd_set_power(machine_lcd_enabled(machine));
        lcd_set_backlight(machine_backlight(machine));
        machine_screen(machine, lcd_framebuffer);
        view_draw(view, (float)elapsed, machine_lcd_enabled(machine));
    }

    machine_save(machine, state, (int64_t)time(NULL));
    serial_close(&serial, machine);
    desktop_destroy(desktop);
    if (verbose) machine_dump_state(machine);
    SDL_DestroyAudioStream(audio);
    view_destroy(view);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    machine_destroy(machine);
    return 0;
}
