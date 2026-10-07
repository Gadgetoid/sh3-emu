#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "app/dialog.h"
#include "app/menu.h"
#include "app/profiles.h"
#include "app/typer.h"
#include "app/view.h"
#include "core/agent.h"
#include "core/gdb.h"
#include "core/key_text.h"
#include "core/lcd.h"
#include "core/machine.h"
#include "net/serial_link.h"
#include "util/file.h"
#include "util/options.h"
#include "util/png.h"

#include <dirent.h>
#include <fcntl.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <spawn.h>
#include <unistd.h>

#define WINDOW_SCALE     2
#define IDLE_FRAME_NS    (SDL_NS_PER_SECOND / 60)
#define NETWORK_REPLUG_MS 2000
#define RUN_HOLD_NS      (4 * SDL_NS_PER_MS)
#define RUN_SLICE_CYCLES (MACHINE_CLOCK_HZ / 1000)
#define RUN_MAX_BEHIND   (MACHINE_CLOCK_HZ / 10)
#define MAX_FRAME_SLICE  0.1
#define AUTOSAVE_SECONDS 60
#define BACKUP_SECONDS   600
#define BACKUP_KEEP      10
#define NOTICE_SECONDS   2
#define WINDOW_TITLE     "SH3Emu"
#define POWER_PRESS_SECONDS 0.2
#define SERIAL_PORT_MAX  16
#define PORT_SCAN_MS     2000

#ifdef __APPLE__
#define SCREENSHOT_FOLDER SDL_FOLDER_DESKTOP
#else
#define SCREENSHOT_FOLDER SDL_FOLDER_PICTURES
#endif

typedef struct {
    SDL_Keycode key;
    uint8_t     scancode;
} key_binding_t;

static const key_binding_t key_bindings[] = {
    { SDLK_TAB, 0x0D }, { SDLK_BACKSPACE, 0x66 }, { SDLK_RETURN, 0x5A }, { SDLK_ESCAPE, 0x76 },
    { SDLK_LSHIFT, 0x12 }, { SDLK_RSHIFT, 0x59 }, { SDLK_LCTRL, 0x14 }, { SDLK_RCTRL, 0x94 },
    { SDLK_LALT, 0x11 }, { SDLK_RALT, 0x91 }, { SDLK_CAPSLOCK, 0x58 },
    { SDLK_LEFT, 0xEB }, { SDLK_UP, 0xF5 }, { SDLK_RIGHT, 0xF4 }, { SDLK_DOWN, 0xF2 },
    { SDLK_DELETE, 0xF1 }, { SDLK_INSERT, 0xF0 }, { SDLK_HOME, 0xEC }, { SDLK_END, 0xE9 },
    { SDLK_PAGEUP, 0xFD }, { SDLK_PAGEDOWN, 0xFA },
    { SDLK_F1, 0x05 }, { SDLK_F2, 0x06 }, { SDLK_F3, 0x04 }, { SDLK_F4, 0x0C }, { SDLK_F5, 0x03 }, { SDLK_F6, 0x0B },
    { SDLK_F7, 0x83 }, { SDLK_F8, 0x0A }, { SDLK_F9, 0x01 }, { SDLK_F10, 0x09 }, { SDLK_F11, 0x78 }, { SDLK_F12, 0x07 },
};

static bool find_scancode(key_layout_t layout, SDL_Keycode key, uint8_t *scancode) {
    if (key >= 0x20 && key < 0x7F) {
        bool shifted;
        return key_text_find(layout, (char)key, scancode, &shifted);
    }
    for (size_t i = 0; i < sizeof key_bindings / sizeof key_bindings[0]; i++) {
        if (key_bindings[i].key == key) { *scancode = key_bindings[i].scancode; return true; }
    }
    return false;
}

static bool verbose = false;

#define SCROLL_STEP    (MACHINE_CLOCK_HZ / 50)
#define SCROLL_PENDING 8

typedef struct {
    float    vertical, horizontal;
    int      pending;
    uint8_t  scancode;
    bool     pressed;
    uint64_t next_at;
} scroller_t;

static void scroller_add(scroller_t *scroller, float vertical, float horizontal) {
    scroller->vertical += vertical;
    scroller->horizontal += horizontal;
    float *axis = fabsf(scroller->vertical) >= fabsf(scroller->horizontal) ? &scroller->vertical : &scroller->horizontal;
    if (fabsf(*axis) < 1.0f) return;
    uint8_t scancode = axis == &scroller->vertical ? (*axis > 0 ? 0xF5 : 0xF2) : (*axis > 0 ? 0xF4 : 0xEB);
    int steps = (int)fabsf(*axis);
    *axis -= *axis > 0 ? (float)steps : -(float)steps;
    if (scancode != scroller->scancode) {
        if (scroller->pressed) return;
        scroller->scancode = scancode;
        scroller->pending = 0;
    }
    scroller->pending += steps;
    if (scroller->pending > SCROLL_PENDING) scroller->pending = SCROLL_PENDING;
}

static void scroller_step(scroller_t *scroller, machine_t *machine) {
    uint64_t now = machine_cycles(machine);
    if (scroller->next_at > now + SCROLL_STEP) scroller->next_at = now;
    if (!scroller->pending || now < scroller->next_at) return;
    machine_key(machine, scroller->scancode, scroller->pressed);
    if (scroller->pressed) scroller->pending--;
    scroller->pressed = !scroller->pressed;
    scroller->next_at = now + SCROLL_STEP;
}

#define INPUT_QUEUE    64
#define PEN_MIN_CYCLES (MACHINE_CLOCK_HZ * 6 / 100)
#define KEY_MIN_CYCLES (MACHINE_CLOCK_HZ / 50)

typedef enum { INPUT_PEN, INPUT_KEY } input_kind_t;

typedef struct {
    struct { uint64_t at; input_kind_t kind; bool down; int x, y; uint8_t scancode; } events[INPUT_QUEUE];
    int      count;
    uint64_t last_at, seen;
} input_queue_t;

static void input_rebase(input_queue_t *input, uint64_t now) {
    if (now < input->seen) {
        for (int i = 0; i < input->count; i++) input->events[i].at = now;
        input->last_at = now;
    }
    input->seen = now;
}

static void input_add(input_queue_t *input, machine_t *machine, input_kind_t kind, bool down, int x, int y, uint8_t scancode) {
    if (input->count == INPUT_QUEUE) return;
    uint64_t now = machine_cycles(machine);
    input_rebase(input, now);
    uint64_t spacing = kind == INPUT_PEN ? PEN_MIN_CYCLES : KEY_MIN_CYCLES;
    uint64_t at = input->last_at + spacing > now ? input->last_at + spacing : now;
    input->events[input->count].at = at;
    input->events[input->count].kind = kind;
    input->events[input->count].down = down;
    input->events[input->count].x = x;
    input->events[input->count].y = y;
    input->events[input->count].scancode = scancode;
    input->count++;
    input->last_at = at;
}

static void pen_move(input_queue_t *input, machine_t *machine, int x, int y) {
    if (input->count) return;
    machine_touch(machine, true, x, y);
}

static void input_step(input_queue_t *input, machine_t *machine) {
    uint64_t now = machine_cycles(machine);
    input_rebase(input, now);
    int done = 0;
    while (done < input->count && input->events[done].at <= now) {
        if (input->events[done].kind == INPUT_PEN) machine_touch(machine, input->events[done].down, input->events[done].x, input->events[done].y);
        else machine_key(machine, input->events[done].scancode, !input->events[done].down);
        done++;
    }
    for (int i = done; i < input->count; i++) input->events[i - done] = input->events[i];
    input->count -= done;
}

static void input_clear(input_queue_t *input) {
    input->count = 0;
    input->last_at = input->seen = 0;
}

static gdb_t *debugger;
static agent_t *agent;
static serial_link_t serial;

static const char *set_serial(machine_t *machine, serial_mode_t mode, const char *device, uint64_t *plug_at) {
    machine_serial_connect(machine, false);
    *plug_at = 0;
    bool same = serial.mode == mode && (mode != SERIAL_DEVICE || !strcmp(serial.name, device));
    if (!same) {
        const char *failure = serial_link_open(&serial, mode, device);
        if (failure) return failure;
        if (mode == SERIAL_PTY) fprintf(stderr, "serial: COM1 on %s\n", serial.name);
    }
    if (mode != SERIAL_OFF) *plug_at = SDL_GetTicks() + NETWORK_REPLUG_MS;
    return NULL;
}

static void log_gdb(const char *message) {
    fputs(message, stderr);
}

typedef struct {
    SDL_Mutex  *lock;
    SDL_Thread *thread;
    machine_t  *machine;
    input_queue_t *input;
    bool        paused, stop, restart;
    SDL_AtomicInt waiting;
} runner_t;

static int run_machine(void *context) {
    runner_t *runner = context;
    double owed = 0;
    uint64_t last = SDL_GetTicksNS();
    for (;;) {
        SDL_LockMutex(runner->lock);
        if (runner->stop) {
            SDL_UnlockMutex(runner->lock);
            return 0;
        }
        uint64_t now = SDL_GetTicksNS();
        if (debugger) gdb_service(debugger);
        if (runner->restart || runner->paused || (debugger && gdb_halted(debugger))) {
            owed = 0;
            runner->restart = false;
        } else {
            owed += (double)(now - last) * MACHINE_CLOCK_HZ / SDL_NS_PER_SECOND;
            if (owed > RUN_MAX_BEHIND) owed = RUN_MAX_BEHIND;
            uint64_t hold_until = now + RUN_HOLD_NS;
            while (owed >= RUN_SLICE_CYCLES && SDL_GetTicksNS() < hold_until && !SDL_GetAtomicInt(&runner->waiting)) {
                input_step(runner->input, runner->machine);
                machine_run(runner->machine, RUN_SLICE_CYCLES);
                owed -= RUN_SLICE_CYCLES;
                if (agent) agent_poll(agent, machine_mailbox(runner->machine));
                serial_link_pump(&serial, runner->machine);
                if (!debugger) continue;
                gdb_after_run(debugger);
                if (gdb_halted(debugger)) break;
            }
        }
        last = now;
        bool caught_up = owed < RUN_SLICE_CYCLES;
        SDL_UnlockMutex(runner->lock);
        if (caught_up) SDL_DelayNS(SDL_NS_PER_MS / 2);
        while (SDL_GetAtomicInt(&runner->waiting)) SDL_DelayNS(SDL_NS_PER_MS / 10);
    }
}

static void release_keys(input_queue_t *input, machine_t *machine, bool *held, int only_modifiers_up) {
    static const struct { uint8_t scancode; int modifier; } modifiers[] = {
        { 0x12, MENU_MOD_SHIFT }, { 0x59, MENU_MOD_SHIFT }, { 0x14, MENU_MOD_CONTROL }, { 0x94, MENU_MOD_CONTROL }, { 0x11, MENU_MOD_ALT }, { 0x91, MENU_MOD_ALT },
    };
    if (only_modifiers_up < 0) {
        for (int i = 0; i < 256; i++) {
            if (!held[i]) continue;
            held[i] = false;
            input_add(input, machine, INPUT_KEY, false, 0, 0, (uint8_t)i);
        }
        return;
    }
    if (!(only_modifiers_up & MENU_MOD_KNOWN)) return;
    for (size_t i = 0; i < sizeof modifiers / sizeof modifiers[0]; i++) {
        uint8_t scancode = modifiers[i].scancode;
        if (held[scancode] && !(only_modifiers_up & modifiers[i].modifier)) {
            held[scancode] = false;
            input_add(input, machine, INPUT_KEY, false, 0, 0, scancode);
        }
    }
}

typedef enum { PICK_SAVE_SNAPSHOT = 1, PICK_LOAD_SNAPSHOT, PICK_CARD } pick_kind_t;

#define PICK_MAX 64

typedef struct {
    pick_kind_t kind;
    int         count;
    char        paths[PICK_MAX][1024];
} picked_t;

static Uint32 pick_event_type = 0;

static void pick_done(void *userdata, const char *const *files, int filter) {
    (void)filter;
    if (!pick_event_type || !files || !files[0]) return;
    picked_t *picked = malloc(sizeof *picked);
    if (!picked) return;
    picked->kind = (pick_kind_t)(intptr_t)userdata;
    picked->count = 0;
    while (files[picked->count] && picked->count < PICK_MAX) {
        snprintf(picked->paths[picked->count], sizeof picked->paths[0], "%s", files[picked->count]);
        picked->count++;
    }
    SDL_Event event;
    SDL_zero(event);
    event.type = pick_event_type;
    event.user.data1 = picked;
    if (!SDL_PushEvent(&event)) free(picked);
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

static const char *handle_drop(dropped_t *dropped, machine_t *machine) {
    static char message[1200];
    const char *card = NULL;
    int files = 0;
    for (int i = 0; i < dropped->count; i++) {
        if (is_directory(dropped->paths[i])) continue;
        files++;
        if (!card && has_extension(dropped->paths[i], ".img")) card = dropped->paths[i];
    }
    dropped->count = 0;
    if (!card || files != 1) return "drop a card image (.img)";
    snprintf(message, sizeof message, machine_insert_card(machine, card) ? "inserted %s" : "could not open %s", file_leaf_name(card));
    return message;
}

static void log_message(const char *message) {
    if (verbose) fputs(message, stderr);
}

#define DEBUG_LOG_MAX (1024 * 1024)

static FILE *debug_log;
static bool  debug_to_stderr;

static void data_folder(char *path, size_t size);

static void debug_log_path(char *path, size_t size) {
    char base[1024];
    data_folder(base, sizeof base);
    snprintf(path, size, "%s/debug.log", base);
}

static void print_debug_line(void *context, const char *line) {
    (void)context;
    if (debugger) gdb_debug_line(debugger, line);
    if (debug_to_stderr) fprintf(stderr, "debug: %s\n", line);
    if (!debug_log) return;
    time_t now = time(NULL);
    struct tm local;
    localtime_r(&now, &local);
    char stamp[16];
    strftime(stamp, sizeof stamp, "%H:%M:%S", &local);
    fprintf(debug_log, "%s %s\n", stamp, line);
    fflush(debug_log);
}

static void start_debug_log(const char *rom_path) {
    if (!debug_log) {
        char path[1100], old[1110];
        debug_log_path(path, sizeof path);
        struct stat info;
        if (stat(path, &info) == 0 && info.st_size > DEBUG_LOG_MAX) {
            snprintf(old, sizeof old, "%s.old", path);
            rename(path, old);
        }
        debug_log = fopen(path, "a");
        if (!debug_log) return;
    }
    time_t now = time(NULL);
    struct tm local;
    localtime_r(&now, &local);
    char stamp[32];
    strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M:%S", &local);
    fprintf(debug_log, "--- %s %s\n", stamp, file_leaf_name(rom_path));
    fflush(debug_log);
}

static void data_folder(char *path, size_t size) {
    const char *data_home = getenv("XDG_DATA_HOME");
    const char *home = getenv("HOME") ? getenv("HOME") : ".";
    if (data_home && data_home[0] == '/') snprintf(path, size, "%s/sh3-emu", data_home);
#ifdef __APPLE__
    else snprintf(path, size, "%s/Library/Application Support/sh3-emu", home);
#else
    else snprintf(path, size, "%s/.local/share/sh3-emu", home);
#endif
    SDL_CreateDirectory(path);
}

static void snapshot_folder(char *path, size_t size) {
    char base[1024];
    data_folder(base, sizeof base);
    snprintf(path, size, "%s/snapshots", base);
    SDL_CreateDirectory(path);
}

static void backup_path(const char *state, char *prefix, size_t prefix_size, char *path, size_t size) {
    char folder[1100];
    snapshot_folder(folder, sizeof folder);
    snprintf(folder + strlen(folder), sizeof folder - strlen(folder), "/Backups");
    SDL_CreateDirectory(folder);
    char name[256];
    snprintf(name, sizeof name, "%s", file_leaf_name(state));
    char *extension = strrchr(name, '.');
    if (extension && extension != name) *extension = 0;
    time_t now = time(NULL);
    struct tm local;
    localtime_r(&now, &local);
    char stamp[64];
    strftime(stamp, sizeof stamp, "%Y-%m-%d at %H.%M.%S", &local);
    snprintf(prefix, prefix_size, "%s ", name);
    snprintf(path, size, "%s/%s%s.state", folder, prefix, stamp);
}

static int compare_name_pointers(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static void prune_backups(const char *path, const char *prefix) {
    char folder[1100];
    snprintf(folder, sizeof folder, "%s", path);
    char *slash = strrchr(folder, '/');
    if (!slash) return;
    *slash = 0;
    DIR *dir = opendir(folder);
    if (!dir) return;
    char *names[256];
    int count = 0;
    size_t prefix_length = strlen(prefix);
    struct dirent *entry;
    while ((entry = readdir(dir)) && count < 256) {
        if (strncmp(entry->d_name, prefix, prefix_length) || !has_extension(entry->d_name, ".state")) continue;
        names[count] = strdup(entry->d_name);
        if (names[count]) count++;
    }
    closedir(dir);
    qsort(names, (size_t)count, sizeof names[0], compare_name_pointers);
    for (int i = 0; i < count; i++) {
        if (i < count - BACKUP_KEEP) {
            char old[1400];
            snprintf(old, sizeof old, "%s/%s", folder, names[i]);
            remove(old);
        }
        free(names[i]);
    }
}

static bool backup_machine(machine_t *machine, const char *state) {
    char prefix[300], path[1400];
    backup_path(state, prefix, sizeof prefix, path, sizeof path);
    bool saved = machine_save(machine, path, (int64_t)time(NULL));
    if (saved) prune_backups(path, prefix);
    return saved;
}

static bool backup_file(const char *state) {
    size_t size;
    uint8_t *contents = file_read(state, &size);
    if (!contents) return false;
    char prefix[300], path[1400];
    backup_path(state, prefix, sizeof prefix, path, sizeof path);
    FILE *file = fopen(path, "wb");
    bool written = file && fwrite(contents, 1, size, file) == size;
    if (file && fclose(file) != 0) written = false;
    free(contents);
    if (written) prune_backups(path, prefix);
    return written;
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
    snprintf(rom_name, sizeof rom_name, "%s", file_leaf_name(rom_path));
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
    screen_size_t screen;
    uint32_t speed;
    uint32_t host_time;
    uint32_t scale;
    uint32_t system;
    char     machine[64];
    uint32_t display;
    uint32_t serial;
    char     serial_device[SERIAL_LINK_PORT_NAME];
} settings_t;

static void settings_path(char *path, size_t size) {
    const char *config_home = getenv("XDG_CONFIG_HOME");
    char base[1024];
    if (config_home && config_home[0] == '/') snprintf(base, sizeof base, "%s/sh3-emu", config_home);
#ifdef __APPLE__
    else data_folder(base, sizeof base);
#else
    else snprintf(base, sizeof base, "%s/.config/sh3-emu", getenv("HOME") ? getenv("HOME") : ".");
#endif
    SDL_CreateDirectory(base);
    snprintf(path, size, "%s/sh3emu.ini", base);
}

static const uint32_t SCALES[] = { 50, 75, 100, 150, 200 };
#define SCALE_COUNT (int)(sizeof SCALES / sizeof SCALES[0])

static int scale_index(uint32_t scale) {
    for (int i = 0; i < SCALE_COUNT; i++) {
        if (SCALES[i] == scale) return i;
    }
    return -1;
}

static void copy_setting(char *destination, size_t size, const char *value) {
    size_t length = strcspn(value, "\r\n");
    if (length >= size) length = size - 1;
    memcpy(destination, value, length);
    destination[length] = 0;
}

static settings_t settings_load(void) {
    settings_t settings = { .memory = 16, .screen = { SCREEN_STOCK_WIDTH, SCREEN_STOCK_HEIGHT }, .speed = 1, .host_time = 1, .scale = 100, .display = VIEW_SIMULATED };
    char path[1100];
    settings_path(path, sizeof path);
    FILE *file = fopen(path, "r");
    if (!file) return settings;
    char line[1200];
    unsigned value;
    while (fgets(line, sizeof line, file)) {
        if (sscanf(line, "memory=%u", &value) == 1) settings.memory = value;
        else if (!strncmp(line, "screen=", 7)) {
            char size[32];
            copy_setting(size, sizeof size, line + 7);
            screen_parse(size, &settings.screen);
        }
        else if (sscanf(line, "speed=%u", &value) == 1) settings.speed = value;
        else if (sscanf(line, "host_time=%u", &value) == 1) settings.host_time = value;
        else if (sscanf(line, "scale=%u", &value) == 1 && scale_index(value) >= 0) settings.scale = value;
        else if (sscanf(line, "system=%u", &value) == 1) settings.system = value;
        else if (!strncmp(line, "machine=", 8)) copy_setting(settings.machine, sizeof settings.machine, line + 8);
        else if (sscanf(line, "display=%u", &value) == 1 && value <= VIEW_SHARP) settings.display = value;
        else if (sscanf(line, "network=%u", &value) == 1) settings.serial = value ? SERIAL_NETWORK : SERIAL_OFF;
        else if (sscanf(line, "serial=%u", &value) == 1 && value <= SERIAL_DEVICE) settings.serial = value;
        else if (!strncmp(line, "serial_device=", 14)) copy_setting(settings.serial_device, sizeof settings.serial_device, line + 14);
    }
    fclose(file);
    return settings;
}

static void settings_save(const settings_t *settings) {
    char path[1100];
    settings_path(path, sizeof path);
    FILE *file = fopen(path, "w");
    if (!file) return;
    fprintf(file, "memory=%u\nscreen=%ux%u\nspeed=%u\nhost_time=%u\nscale=%u\ndisplay=%u\nsystem=%u\nmachine=%s\nserial=%u\nserial_device=%s\n", settings->memory,
            settings->screen.width, settings->screen.height, settings->speed, settings->host_time, settings->scale, settings->display, settings->system, settings->machine,
            settings->serial, settings->serial_device);
    fclose(file);
}

static const char *serial_choice(machine_t *machine, settings_t *settings, serial_mode_t mode, const char *device, uint64_t *plug_at) {
    static char notice[320];
    const char *failure = set_serial(machine, mode, device, plug_at);
    if (failure) return failure;
    settings->serial = mode;
    if (device) snprintf(settings->serial_device, sizeof settings->serial_device, "%s", device);
    settings_save(settings);
    if (mode == SERIAL_NETWORK) return "network cable plugged in; CE dials it";
    if (mode == SERIAL_OFF) return "serial cable unplugged";
    snprintf(notice, sizeof notice, "COM1 on %s", serial.name);
    return notice;
}

static bool confirm_action(SDL_Window *window, const char *title, const char *message, const char *action) {
    const SDL_MessageBoxButtonData buttons[] = {
        { SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "Cancel" },
        { SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, action },
    };
    const SDL_MessageBoxData dialog = { SDL_MESSAGEBOX_WARNING, window, title, message, (int)(sizeof buttons / sizeof buttons[0]), buttons, NULL };
    int chosen = 0;
    return SDL_ShowMessageBox(&dialog, &chosen) && chosen == 1;
}

static bool confirm_reset(SDL_Window *window, const char *name) {
    char title[160];
    snprintf(title, sizeof title, "Reset %s?", name);
    return confirm_action(window, title,
                          "A reset is a cold boot back to the factory state: it clears RAM, including files, settings and installed programs. A backup of the machine goes in Snapshots/Backups first. Soft Reset keeps them.",
                          "Reset");
}

static void open_path(const char *path) {
    char url[4096] = "file://";
    size_t length = strlen(url);
    for (const unsigned char *at = (const unsigned char *)path; *at && length + 4 < sizeof url; at++) {
        if (isalnum(*at) || strchr("/-_.~", *at)) url[length++] = (char)*at;
        else length += (size_t)snprintf(url + length, sizeof url - length, "%%%02X", *at);
    }
    url[length] = 0;
    SDL_OpenURL(url);
}

#define REVEAL_CHILDREN_MAX 16

static pid_t reveal_children[REVEAL_CHILDREN_MAX];
static int reveal_child_count = 0;

static void reap_reveal_children(void) {
    int kept = 0;
    for (int i = 0; i < reveal_child_count; i++) {
        if (waitpid(reveal_children[i], NULL, WNOHANG) == 0) reveal_children[kept++] = reveal_children[i];
    }
    reveal_child_count = kept;
}

static void reveal_file(const char *path) {
#ifdef __APPLE__
    extern char **environ;
    char *arguments[] = { "open", "-R", (char *)path, NULL };
    pid_t pid;
    if (posix_spawnp(&pid, "open", NULL, NULL, arguments, environ) != 0) return;
    if (reveal_child_count < REVEAL_CHILDREN_MAX) reveal_children[reveal_child_count++] = pid;
    else waitpid(pid, NULL, 0);
#else
    char folder[1100];
    snprintf(folder, sizeof folder, "%s", path);
    char *slash = strrchr(folder, '/');
    if (slash && slash != folder) *slash = 0;
    open_path(folder);
#endif
}

static void set_title(SDL_Window *window, const char *name, const char *notice, bool paused, bool suspended) {
    char base[200], title[1400];
    snprintf(base, sizeof base, "%s (%s)", WINDOW_TITLE, name);
    if (notice) snprintf(title, sizeof title, "%s: %s", base, notice);
    else if (paused) snprintf(title, sizeof title, "%s, paused", base);
    else if (suspended) snprintf(title, sizeof title, "%s, suspended", base);
    else snprintf(title, sizeof title, "%s", base);
    if (strcmp(SDL_GetWindowTitle(window), title)) SDL_SetWindowTitle(window, title);
}

typedef struct {
    char   path[MACHINE_BOARD_COUNT][1024];
    size_t size[MACHINE_BOARD_COUNT];
} rom_set_t;

static void rom_folder(char *path, size_t size) {
    char base[1024];
    data_folder(base, sizeof base);
    snprintf(path, size, "%s/roms", base);
    SDL_CreateDirectory(path);
}

#define ROM_MIN_BYTES   (1024 * 1024)
#define ROM_MAX_BYTES   (64 * 1024 * 1024)
#define ROM_PROBE_CACHE 32

typedef struct {
    char     path[1024];
    off_t    size;
    time_t   modified;
    int      system;
    uint32_t screens;
} rom_probe_t;

static rom_probe_t rom_probes[ROM_PROBE_CACHE];
static int rom_probe_count = 0;
static int rom_probe_next = 0;

static int rom_system(const char *path, uint32_t *screens) {
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
    probe->system = rom_system(path, &probe->screens);
    *screens = probe->screens;
    return probe->system;
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

static bool no_roms_dialog(void) {
    char folder[1100], message[1400];
    rom_folder(folder, sizeof folder);
    snprintf(message, sizeof message, "Put a ROM in %s: a Casio Cassiopeia A-51 or HP 320LX ROM image.", folder);
    const SDL_MessageBoxButtonData buttons[] = {
        { SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "Quit" },
        { SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "Show ROM Folder" },
    };
    const SDL_MessageBoxData dialog = { SDL_MESSAGEBOX_INFORMATION, NULL, "No ROM found", message, 2, buttons, NULL };
    int chosen = 0;
    if (SDL_ShowMessageBox(&dialog, &chosen) && chosen == 1) open_path(folder);
    return false;
}

const uint32_t DIALOG_MEMORY_SIZES[DIALOG_MEMORY_COUNT] = { 16, 32, 64 };

static void machines_folder(char *path, size_t size) {
    char base[1024];
    data_folder(base, sizeof base);
    snprintf(path, size, "%s/machines", base);
    SDL_CreateDirectory(path);
}

static uint32_t probe_rom(const char *path, char *label, size_t label_size) {
    struct stat info;
    if (stat(path, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size < ROM_MIN_BYTES || info.st_size > ROM_MAX_BYTES) return 0;
    uint32_t screens;
    int system = cached_rom_system(path, &info, &screens);
    if (!system) return 0;
    snprintf(label, label_size, "%s: %s", machine_board_name(system), file_leaf_name(path));
    return screens;
}

static int list_roms(dialog_rom_t *roms, int max) {
    char folder[1100];
    rom_folder(folder, sizeof folder);
    DIR *dir = opendir(folder);
    if (!dir) return 0;
    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) && count < max) {
        if (entry->d_name[0] == '.') continue;
        dialog_rom_t *rom = &roms[count];
        if (snprintf(rom->path, sizeof rom->path, "%s/%s", folder, entry->d_name) >= (int)sizeof rom->path) continue;
        rom->screens = probe_rom(rom->path, rom->label, sizeof rom->label);
        if (rom->screens) count++;
    }
    closedir(dir);
    return count;
}

static int profile_system(const profile_t *profile) {
    struct stat info;
    uint32_t screens;
    if (stat(profile->rom, &info) != 0) return 0;
    return cached_rom_system(profile->rom, &info, &screens);
}

static bool legacy_state_path(const char *rom_path, char *state, size_t size) {
    size_t rom_size;
    uint8_t *rom = file_read(rom_path, &rom_size);
    if (!rom) return false;
    char error[256];
    machine_t *machine = machine_create(rom, rom_size, error, sizeof error);
    free(rom);
    if (!machine) return false;
    state_path(state, size, machine, rom_path);
    machine_destroy(machine);
    return true;
}

static screen_size_t rom_screen(const char *path, screen_size_t preferred) {
    struct stat info;
    uint32_t screens = 0;
    if (stat(path, &info) != 0 || !cached_rom_system(path, &info, &screens)) return preferred;
    for (int i = 0; i < SCREEN_PRESET_COUNT; i++) {
        if (SCREEN_PRESETS[i].width == preferred.width && SCREEN_PRESETS[i].height == preferred.height && (screens & (1u << i))) return preferred;
    }
    for (int i = 0; i < SCREEN_PRESET_COUNT; i++) {
        if (screens & (1u << i)) return SCREEN_PRESETS[i];
    }
    return preferred;
}

static void migrate_profiles(profiles_t *profiles, const rom_set_t *roms, const settings_t *settings, const char *folder) {
    for (int system = MACHINE_BOARD_CASIO; system < MACHINE_BOARD_COUNT; system++) {
        if (!roms->path[system][0]) continue;
        profile_t profile = { .memory = settings->memory, .screen = settings->screen, .host_time = settings->host_time != 0 };
        snprintf(profile.name, sizeof profile.name, "%s", machine_board_name(system));
        snprintf(profile.rom, sizeof profile.rom, "%s", roms->path[system]);
        profile.screen = rom_screen(profile.rom, settings->screen);
        if (!legacy_state_path(profile.rom, profile.state, sizeof profile.state)) continue;
        profile_make_unique(profiles, &profile, folder);
        profile_save(&profile, folder);
        profiles_load(profiles, folder);
    }
}

static machine_t *start_machine(const profile_t *profile, uint32_t speed, const char *state_file, bool fresh,
                                char *state, size_t state_size, const char **notice) {
    const char *rom_path = profile->rom;
    static char message[1400];
    *notice = NULL;
    size_t rom_size;
    uint8_t *rom = file_read(rom_path, &rom_size);
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
    machine_set_memory(machine, profile->memory);
    machine_set_screen(machine, profile->screen);
    machine_set_speed(machine, speed);
    machine_set_host_clock(machine, profile->host_time);
    machine_set_debug_output(machine, print_debug_line, NULL);
    start_debug_log(rom_path);
    if (state_file) snprintf(state, state_size, "%s", state_file);
    else if (profile->state[0]) snprintf(state, state_size, "%s", profile->state);
    else state_path(state, state_size, machine, rom_path);
    if (fresh) {
        backup_file(state);
        return machine;
    }
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
        snprintf(message, sizeof message, "saved state unreadable, moved to %s", file_leaf_name(backup));
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
    const char *folder = SDL_GetUserFolder(SCREENSHOT_FOLDER);
    if (folder) snprintf(path, size, "%sSH3Emu Screenshot %s.png", folder, stamp);
    else snprintf(path, size, "%s/SH3Emu Screenshot %s.png", getenv("HOME") ? getenv("HOME") : ".", stamp);
    FILE *file = fopen(path, "wb");
    bool saved = file && fwrite(png, 1, length, file) == length;
    if (file) fclose(file);
    free(png);
    return saved;
}

static void window_size(view_display_t display, uint32_t scale, int *width, int *height) {
    view_source_size(display, width, height);
    *width = *width * WINDOW_SCALE * (int)scale / 100;
    *height = *height * WINDOW_SCALE * (int)scale / 100 + menu_bar_height();
}

static void fit_window(SDL_Window *window, view_t *view, uint32_t scale) {
    if (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) SDL_SetWindowFullscreen(window, false);
    int width, height;
    window_size(view_display(view), scale, &width, &height);
    SDL_SetWindowSize(window, width, height);
}

typedef struct {
    settings_t   *settings;
    const char   *card, *state_file, *machine;
    bool          fresh;
    int           gdb_port;
    const char   *gdb_process;
    const char   *agent_socket;
} launch_t;

enum {
    LAUNCH_HEADING_MACHINE, LAUNCH_MACHINE, LAUNCH_STATE, LAUNCH_FRESH, LAUNCH_CARD, LAUNCH_MEMORY, LAUNCH_SPEED,
    LAUNCH_HEADING_CONNECTIONS, LAUNCH_NET, LAUNCH_AGENT,
    LAUNCH_HEADING_DEBUGGING, LAUNCH_VERBOSE, LAUNCH_DEBUG_OUTPUT, LAUNCH_GDB, LAUNCH_GDB_PROCESS,
};

static const option_t LAUNCH_OPTIONS[] = {
    [LAUNCH_HEADING_MACHINE] = { NULL, NULL, "Machine", 0 },
    [LAUNCH_MACHINE] = { "machine", "NAME", "open the machine with this name (from Machine > Machines)", 0 },
    [LAUNCH_STATE] = { "state", "FILE", "load, save and autosave FILE instead of the ROM's own state", 0 },
    [LAUNCH_FRESH] = { "fresh", NULL, "ignore the saved state and cold boot", 0 },
    [LAUNCH_CARD] = { "card", "IMAGE", "insert a CompactFlash card backed by a raw disk image", 0 },
    [LAUNCH_MEMORY] = { "memory", "MB", "RAM for a ROM given on the command line: 16, 32 or 64", 0 },
    [LAUNCH_SPEED] = { "speed", "N", "CPU speed multiple: 1, 2, 4 or 8", 0 },
    [LAUNCH_HEADING_CONNECTIONS] = { NULL, NULL, "Connections", 0 },
    [LAUNCH_NET] = { "net", NULL, "plug COM1 into the PPP network (Devices > Serial Port), and remember that", 0 },
    [LAUNCH_AGENT] = { "agent", "SOCKET", "pass messages between a guest agent's trapa #0xCE mailbox and one client on this Unix socket", 0 },
    [LAUNCH_HEADING_DEBUGGING] = { NULL, NULL, "Debugging", 0 },
    [LAUNCH_VERBOSE] = { "verbose", NULL, "log unmodelled hardware accesses to stderr", 0 },
    [LAUNCH_DEBUG_OUTPUT] = { "debug-output", NULL, "print CE's debug output (OutputDebugString, kernel messages) to stderr as well as debug.log", 0 },
    [LAUNCH_GDB] = { "gdb", "PORT", "listen for GDB on 127.0.0.1:PORT; it can attach and detach while the machine runs", 0 },
    [LAUNCH_GDB_PROCESS] = { "gdb-process", "NAME", "debug one process, e.g. maths.exe: breakpoints below 0x02000000 only stop there, and GDB stops when it starts", 0 },
};

static bool launch_option(void *context, int option, const char *value, char *error, size_t error_size) {
    launch_t *launch = context;
    settings_t *settings = launch->settings;
    long integer;
    (void)error;
    (void)error_size;
    switch (option) {
    case LAUNCH_MACHINE: launch->machine = value; return true;
    case LAUNCH_STATE: launch->state_file = value; return true;
    case LAUNCH_FRESH: launch->fresh = true; return true;
    case LAUNCH_CARD: launch->card = value; return true;
    case LAUNCH_MEMORY:
        if (!option_integer(value, 10, &integer) || (integer != 16 && integer != 32 && integer != 64)) return false;
        settings->memory = (uint32_t)integer;
        return true;
    case LAUNCH_SPEED:
        if (!option_integer(value, 10, &integer) || (integer != 1 && integer != 2 && integer != 4 && integer != 8)) return false;
        settings->speed = (uint32_t)integer;
        return true;
    case LAUNCH_VERBOSE: verbose = true; return true;
    case LAUNCH_DEBUG_OUTPUT: debug_to_stderr = true; return true;
    case LAUNCH_GDB:
        if (!option_integer(value, 10, &integer) || integer < 1 || integer > 65535) return false;
        launch->gdb_port = (int)integer;
        return true;
    case LAUNCH_GDB_PROCESS: launch->gdb_process = value; return true;
    case LAUNCH_NET: settings->serial = SERIAL_NETWORK; return true;
    case LAUNCH_AGENT: launch->agent_socket = value; return true;
    }
    return false;
}

static const option_spec_t LAUNCH_SPEC = {
    "sh3emu", "[OPTIONS] [ROM]",
    "Emulates the Casio Cassiopeia A-51 and HP 320LX Windows CE handhelds. With no ROM it opens the last machine used; machines are made with Machine > New Machine from the ROMs in the roms folder in its data folder. With a ROM it runs that ROM with its own saved state, outside the machine list.",
    LAUNCH_OPTIONS, (int)(sizeof LAUNCH_OPTIONS / sizeof LAUNCH_OPTIONS[0]),
    "headless runs the machine without a window, for tests and scripts.",
};

int main(int argc, char **argv) {
    const char *rom_path = NULL;
    settings_t settings = settings_load();
    launch_t launch = { &settings, NULL, NULL, NULL, false, 0, NULL, NULL };
    const char *positional[1];
    int positional_count;
    options_result_t parsed = options_parse(&LAUNCH_SPEC, argc, argv, launch_option, &launch, positional, 1, &positional_count);
    if (parsed == OPTIONS_EXIT) return 0;
    if (parsed == OPTIONS_ERROR) return 2;
    if (positional_count) rom_path = positional[0];
    const char *card = launch.card, *state_file = launch.state_file;
    bool fresh = launch.fresh;
    static rom_set_t roms;
    find_roms(&roms);
    char profiles_folder[1100];
    machines_folder(profiles_folder, sizeof profiles_folder);
    static profiles_t profiles;
    profiles_load(&profiles, profiles_folder);
    if (!profiles.count) migrate_profiles(&profiles, &roms, &settings, profiles_folder);
    static profile_t current;
    int current_index = -1;
    if (rom_path) {
        current = (profile_t){ .memory = settings.memory, .screen = settings.screen, .host_time = settings.host_time != 0 };
        snprintf(current.rom, sizeof current.rom, "%s", rom_path);
        snprintf(current.name, sizeof current.name, "%s", file_leaf_name(rom_path));
    } else {
        if (launch.machine) {
            current_index = profile_find(&profiles, launch.machine);
            if (current_index < 0) { fprintf(stderr, "no machine called %s\n", launch.machine); return 2; }
        } else {
            current_index = settings.machine[0] ? profile_find(&profiles, settings.machine) : -1;
            if (current_index < 0) current_index = profiles.count ? 0 : -1;
        }
        if (current_index < 0) return no_roms_dialog() ? 0 : 1;
        current = profiles.entries[current_index];
    }
    char state[1100];
    const char *startup_notice = NULL;
    machine_t *machine = start_machine(&current, settings.speed, state_file, fresh, state, sizeof state, &startup_notice);
    if (!machine) { fprintf(stderr, "%s\n", startup_notice); return 1; }
    if (current_index >= 0) {
        snprintf(settings.machine, sizeof settings.machine, "%s", current.id);
        settings_save(&settings);
    }
    key_layout_t key_layout = machine_key_layout(machine);
    screen_size_t screen = machine_screen_size(machine);
    lcd_set_size(screen.width, screen.height);

    SDL_SetAppMetadata("SH3Emu", options_version(), "sh3-emu");
    if (!SDL_Init(SDL_INIT_VIDEO)) { fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 1; }
    pick_event_type = SDL_RegisterEvents(1);
    int window_width, window_height;
    window_size((view_display_t)settings.display, settings.scale, &window_width, &window_height);
    SDL_Window *window = SDL_CreateWindow(WINDOW_TITLE, window_width, window_height, SDL_WINDOW_HIGH_PIXEL_DENSITY);
    SDL_Renderer *renderer = window ? SDL_CreateRenderer(window, NULL) : NULL;
    if (!renderer) { fprintf(stderr, "SDL: %s\n", SDL_GetError()); return 1; }
    SDL_SetRenderVSync(renderer, 1);

    view_t *view = view_create(window, renderer, (view_display_t)settings.display, menu_bar_height());


    if (card && !machine_insert_card(machine, card)) fprintf(stderr, "cannot open card image %s\n", card);

    menu_install(window);

    bool running = true, pen_down = false, paused = false;
    bool held[256] = { false };
    uint64_t last = SDL_GetPerformanceCounter();
    double frequency = (double)SDL_GetPerformanceFrequency();
    double since_autosave = 0, since_backup = 0, notice_left = 0;
    const char *notice = startup_notice;
    if (notice) notice_left = 6;
    char paste_notice[64];
    static typer_t typer;
    static scroller_t scroller;
    static input_queue_t input;
    static dropped_t dropped;
    picked_t *picked = NULL;


    if (launch.gdb_process && !launch.gdb_port) {
        fprintf(stderr, "sh3emu: --gdb-process needs --gdb\n");
        return 2;
    }
    if (launch.gdb_port) {
        debugger = gdb_create(machine, launch.gdb_port, log_gdb);
        if (!debugger) {
            fprintf(stderr, "sh3emu: cannot listen for GDB on port %d\n", launch.gdb_port);
            return 1;
        }
        if (launch.gdb_process) gdb_set_process(debugger, launch.gdb_process);
    }
    uint64_t serial_plug_at = 0, power_release_at = 0, port_scan_at = 0;
    static char ports[SERIAL_PORT_MAX][SERIAL_LINK_PORT_NAME];
    int port_count = 0;
    serial_link_init(&serial, NULL);
    const char *serial_failure = set_serial(machine, (serial_mode_t)settings.serial, settings.serial_device, &serial_plug_at);
    if (serial_failure) {
        fprintf(stderr, "sh3emu: %s\n", serial_failure);
        settings.serial = SERIAL_OFF;
    }
    if (launch.agent_socket && !(agent = agent_create(launch.agent_socket, log_gdb))) {
        fprintf(stderr, "sh3emu: cannot listen on agent socket %s\n", launch.agent_socket);
        return 1;
    }
    static runner_t runner;
    runner = (runner_t){ SDL_CreateMutex(), NULL, machine, &input, false, false, true, { 0 } };
    runner.thread = SDL_CreateThread(run_machine, "machine", &runner);
    while (running) {
        SDL_Event event;
        uint64_t frame_start = SDL_GetTicksNS();
        bool events_seen = false;
        SDL_SetAtomicInt(&runner.waiting, 1);
        SDL_LockMutex(runner.lock);
        SDL_SetAtomicInt(&runner.waiting, 0);
        while (SDL_PollEvent(&event)) {
            events_seen = true;
            if (menu_event(&event)) {
                if (menu_active()) {
                    release_keys(&input, machine, held, -1);
                    if (pen_down) input_add(&input, machine, INPUT_PEN, false, 0, 0, 0);
                    pen_down = false;
                }
                continue;
            }
            switch (event.type) {
            case SDL_EVENT_QUIT:
                running = false;
                break;
            case SDL_EVENT_KEY_DOWN:
            case SDL_EVENT_KEY_UP: {
                bool down = event.type == SDL_EVENT_KEY_DOWN;
                uint8_t scancode;
                if (!find_scancode(key_layout, event.key.key, &scancode)) break;
                if (down) {
                    if (event.key.repeat || (event.key.mod & SDL_KMOD_GUI) || held[scancode]) break;
                    held[scancode] = true;
                    input_add(&input, machine, INPUT_KEY, true, 0, 0, scancode);
                } else if (held[scancode]) {
                    held[scancode] = false;
                    input_add(&input, machine, INPUT_KEY, false, 0, 0, scancode);
                }
                break;
            }
            case SDL_EVENT_MOUSE_WHEEL:
                scroller_add(&scroller, event.wheel.y, event.wheel.x);
                break;
            case SDL_EVENT_DROP_FILE:
                if (event.drop.data && dropped.count < PICK_MAX) snprintf(dropped.paths[dropped.count++], sizeof dropped.paths[0], "%s", event.drop.data);
                break;
            case SDL_EVENT_DROP_COMPLETE:
                if (dropped.count) {
                    notice = handle_drop(&dropped, machine);
                    notice_left = NOTICE_SECONDS * 2;
                }
                break;
            case SDL_EVENT_WINDOW_FOCUS_GAINED:
                find_roms(&roms);
                break;
            case SDL_EVENT_WINDOW_FOCUS_LOST:
                release_keys(&input, machine, held, -1);
                break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
                if (event.button.button == SDL_BUTTON_LEFT) {
                    int x, y;
                    if (view_screen_position(view, event.button.x, event.button.y, &x, &y)) {
                        pen_down = true;
                        input_add(&input, machine, INPUT_PEN, true, x, y, 0);
                    }
                }
                break;
            case SDL_EVENT_MOUSE_MOTION:
                if (pen_down) {
                    int x, y;
                    view_screen_position(view, event.motion.x, event.motion.y, &x, &y);
                    pen_move(&input, machine, x, y);
                }
                break;
            case SDL_EVENT_MOUSE_BUTTON_UP:
                if (event.button.button == SDL_BUTTON_LEFT && pen_down) {
                    int x, y;
                    view_screen_position(view, event.button.x, event.button.y, &x, &y);
                    pen_down = false;
                    input_add(&input, machine, INPUT_PEN, false, x, y, 0);
                }
                break;
            default:
                if (pick_event_type && event.type == pick_event_type) {
                    free(picked);
                    picked = event.user.data1;
                }
                break;
            }
        }

        release_keys(&input, machine, held, menu_modifiers());
        int switch_to = -1;
        for (int item = menu_poll(); item >= 0; item = menu_poll()) {
            release_keys(&input, machine, held, -1);
            switch (item) {
            case MENU_POWER:
                machine_power_button(machine, true);
                power_release_at = machine_cycles(machine) + (uint64_t)(POWER_PRESS_SECONDS * MACHINE_CLOCK_HZ);
                break;
            case MENU_PAUSE: paused = !paused; break;
            case MENU_SOFT_RESET: machine_soft_reset(machine); break;
            case MENU_NEW_MACHINE: {
                static dialog_rom_t rom_list[32];
                int rom_count = list_roms(rom_list, 32);
                dialog_machine_t chosen = { .memory = 16, .screen = { SCREEN_STOCK_WIDTH, SCREEN_STOCK_HEIGHT }, .host_time = settings.host_time != 0 };
                if (rom_count) snprintf(chosen.rom, sizeof chosen.rom, "%s", current.rom);
                events_seen = true;
                if (!dialog_new_machine(window, rom_list, rom_count, probe_rom, &chosen)) break;
                profile_t made = { .screen = chosen.screen, .memory = chosen.memory, .host_time = chosen.host_time };
                snprintf(made.rom, sizeof made.rom, "%s", chosen.rom);
                if (chosen.name[0]) snprintf(made.name, sizeof made.name, "%s", chosen.name);
                else profile_default_name(&made, profile_system(&made), made.name, sizeof made.name);
                profile_make_unique(&profiles, &made, profiles_folder);
                if (!profile_save(&made, profiles_folder)) {
                    notice = "could not save the new machine";
                    notice_left = NOTICE_SECONDS * 2;
                    break;
                }
                profiles_load(&profiles, profiles_folder);
                if (current.id[0]) current_index = profile_find(&profiles, current.id);
                switch_to = profile_find(&profiles, made.id);
                break;
            }
            case MENU_MANAGE_MACHINES: {
                const char *names[PROFILES_MAX];
                for (int i = 0; i < profiles.count; i++) names[i] = profiles.entries[i].name;
                int chosen = current_index >= 0 ? current_index : 0;
                events_seen = true;
                dialog_manage_t action = profiles.count ? dialog_manage_machines(window, names, profiles.count, current_index, &chosen) : DIALOG_MANAGE_CLOSE;
                if (action == DIALOG_MANAGE_CLOSE || chosen < 0 || chosen >= profiles.count) break;
                profile_t picked_profile = profiles.entries[chosen];
                static char manage_notice[300];
                if (action == DIALOG_MANAGE_RESET) {
                    if (!confirm_reset(window, picked_profile.name)) break;
                    if (chosen == current_index) {
                        backup_machine(machine, state);
                        since_backup = 0;
                        machine_reset(machine);
                    } else {
                        backup_file(picked_profile.state);
                        remove(picked_profile.state);
                    }
                    snprintf(manage_notice, sizeof manage_notice, "reset %s; the machine before it is in Snapshots/Backups", picked_profile.name);
                } else {
                    if (chosen == current_index) {
                        notice = "switch to another machine before deleting this one";
                        notice_left = NOTICE_SECONDS * 3;
                        break;
                    }
                    char title[160];
                    snprintf(title, sizeof title, "Delete %s?", picked_profile.name);
                    if (!confirm_action(window, title, "This removes the machine and its saved state. A backup of the state goes in Snapshots/Backups first.", "Delete")) break;
                    backup_file(picked_profile.state);
                    profile_delete(&picked_profile, profiles_folder);
                    char current_id[sizeof current.id];
                    snprintf(current_id, sizeof current_id, "%s", current.id);
                    profiles_load(&profiles, profiles_folder);
                    current_index = current_id[0] ? profile_find(&profiles, current_id) : -1;
                    snprintf(manage_notice, sizeof manage_notice, "deleted %s", picked_profile.name);
                }
                notice = manage_notice;
                notice_left = NOTICE_SECONDS * 3;
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
                if (save_screenshot(view, path, sizeof path)) snprintf(screenshot_notice, sizeof screenshot_notice, "saved %s", file_leaf_name(path));
                else snprintf(screenshot_notice, sizeof screenshot_notice, "could not save the screenshot");
                notice = screenshot_notice;
                notice_left = NOTICE_SECONDS * 2;
                break;
            }
            case MENU_PASTE: {
                char *clipboard = SDL_GetClipboardText();
                size_t typed = clipboard ? typer_start(&typer, key_layout, clipboard) : 0;
                SDL_free(clipboard);
                snprintf(paste_notice, sizeof paste_notice, typed ? "typing %zu characters" : "nothing to type", typed);
                notice = paste_notice;
                notice_left = NOTICE_SECONDS;
                break;
            }
            case MENU_SAVE_STATE:
                notice = machine_save(machine, state, (int64_t)time(NULL)) ? "state saved" : "could not save state";
                notice_left = NOTICE_SECONDS;
                break;
            case MENU_LOAD_STATE:
                backup_machine(machine, state);
                notice = machine_load(machine, state, NULL) ? "state loaded" : "no saved state";
                set_serial(machine, (serial_mode_t)settings.serial, settings.serial_device, &serial_plug_at);
                notice_left = NOTICE_SECONDS;
                break;
            case MENU_SHOW_STATE: reveal_file(state); break;
            case MENU_SAVE_SNAPSHOT: {
                static const SDL_DialogFileFilter filters[] = { { "Snapshot", "state" } };
                static char default_snapshot[1200];
                snapshot_default_name(default_snapshot, sizeof default_snapshot);
                SDL_ShowSaveFileDialog(pick_done, (void *)(intptr_t)PICK_SAVE_SNAPSHOT, window, filters, 1, default_snapshot);
                break;
            }
            case MENU_LOAD_SNAPSHOT: {
                static const SDL_DialogFileFilter filters[] = { { "Snapshot", "state;bin" } };
                static char folder[1100];
                snapshot_folder(folder, sizeof folder);
                SDL_ShowOpenFileDialog(pick_done, (void *)(intptr_t)PICK_LOAD_SNAPSHOT, window, filters, 1, folder, false);
                break;
            }
            case MENU_SHOW_DEBUG_OUTPUT: {
                char path[1100];
                debug_log_path(path, sizeof path);
                if (debug_log) fflush(debug_log);
                open_path(path);
                break;
            }
            case MENU_QUIT:
                running = false;
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
                SDL_ShowOpenFileDialog(pick_done, (void *)(intptr_t)PICK_CARD, window, filters, 2, NULL, false);
                break;
            }
            default:
                if (item >= MENU_SERIAL_PORT_FIRST && item <= MENU_SERIAL_PORT_LAST && item - MENU_SERIAL_PORT_FIRST < port_count) {
                    notice = serial_choice(machine, &settings, SERIAL_DEVICE, ports[item - MENU_SERIAL_PORT_FIRST], &serial_plug_at);
                    notice_left = NOTICE_SECONDS * 3;
                }
                if (item >= MENU_MACHINE_FIRST && item <= MENU_MACHINE_LAST && item - MENU_MACHINE_FIRST < profiles.count && item - MENU_MACHINE_FIRST != current_index) {
                    switch_to = item - MENU_MACHINE_FIRST;
                }
                break;
            case MENU_SERIAL_OFF:
            case MENU_SERIAL_NETWORK:
            case MENU_SERIAL_PTY: {
                serial_mode_t mode = item == MENU_SERIAL_NETWORK ? SERIAL_NETWORK : item == MENU_SERIAL_PTY ? SERIAL_PTY : SERIAL_OFF;
                notice = serial_choice(machine, &settings, mode, NULL, &serial_plug_at);
                notice_left = NOTICE_SECONDS * 3;
                break;
            }
            case MENU_EJECT_CARD:
                machine_eject_card(machine);
                notice = "card ejected";
                notice_left = NOTICE_SECONDS;
                break;
            }
        }
        if (switch_to >= 0 && switch_to < profiles.count && switch_to != current_index) {
            {
                const char *switch_notice = NULL;
                char next_state[sizeof state];
                profile_t next_profile = profiles.entries[switch_to];
                machine_t *next = start_machine(&next_profile, settings.speed, NULL, false, next_state, sizeof next_state, &switch_notice);
                if (!next) {
                    notice = switch_notice;
                    notice_left = NOTICE_SECONDS * 2;
                } else {
                    if (pen_down) machine_touch(machine, false, 0, 0);
                    input_clear(&input);
                    pen_down = false;
                    typer.length = typer.position = 0;
                    machine_save(machine, state, (int64_t)time(NULL));
                    machine_destroy(machine);
                    machine = next;
                    memcpy(state, next_state, sizeof state);
                    current = next_profile;
                    current_index = switch_to;
                    key_layout = machine_key_layout(machine);
                    snprintf(settings.machine, sizeof settings.machine, "%s", current.id);
                    settings_save(&settings);
                    set_serial(machine, (serial_mode_t)settings.serial, settings.serial_device, &serial_plug_at);
                    since_backup = 0;
                    runner.machine = machine;
                    runner.restart = true;
                    if (debugger) gdb_set_machine(debugger, machine);
                    static char switched_notice[160];
                    snprintf(switched_notice, sizeof switched_notice, "switched to %s", current.name);
                    notice = switch_notice ? switch_notice : switched_notice;
                    notice_left = NOTICE_SECONDS * 2;
                }
            }
        }
        if (picked) {
            if (picked->kind == PICK_CARD) {
                notice = machine_insert_card(machine, picked->paths[0]) ? "card inserted" : "could not open card image";
                notice_left = NOTICE_SECONDS;
            } else if (picked->kind == PICK_SAVE_SNAPSHOT) {
                static char snapshot_notice[1200];
                char path[1100];
                snprintf(path, sizeof path, "%s%s", picked->paths[0], has_extension(picked->paths[0], ".state") ? "" : ".state");
                bool saved = machine_save(machine, path, (int64_t)time(NULL));
                snprintf(snapshot_notice, sizeof snapshot_notice, saved ? "saved snapshot %s" : "could not save %s", file_leaf_name(path));
                notice = snapshot_notice;
                notice_left = NOTICE_SECONDS * 2;
            } else if (picked->kind == PICK_LOAD_SNAPSHOT) {
                static char snapshot_notice[1200];
                if (machine_state_matches(machine, picked->paths[0])) backup_machine(machine, state);
                if (machine_load(machine, picked->paths[0], NULL)) {
                    set_serial(machine, (serial_mode_t)settings.serial, settings.serial_device, &serial_plug_at);
                    snprintf(snapshot_notice, sizeof snapshot_notice, "loaded snapshot %s", file_leaf_name(picked->paths[0]));
                } else {
                    snprintf(snapshot_notice, sizeof snapshot_notice, "%s isn't a snapshot of this ROM", file_leaf_name(picked->paths[0]));
                }
                notice = snapshot_notice;
                notice_left = NOTICE_SECONDS * 2;
            }
            free(picked);
            picked = NULL;
        }
        menu_ensure();
        menu_set_enabled(MENU_EJECT_CARD, machine_card_inserted(machine));
        menu_set_enabled(MENU_SERIAL_NETWORK, net_gateway_available());
        menu_set_checked(MENU_SERIAL_OFF, serial.mode == SERIAL_OFF);
        menu_set_checked(MENU_SERIAL_NETWORK, serial.mode == SERIAL_NETWORK);
        menu_set_checked(MENU_SERIAL_PTY, serial.mode == SERIAL_PTY);
        if (SDL_GetTicks() >= port_scan_at) {
            port_scan_at = SDL_GetTicks() + PORT_SCAN_MS;
            port_count = serial_link_ports(ports, SERIAL_PORT_MAX);
        }
        for (int i = 0; i < SERIAL_PORT_MAX; i++) {
            int port_item = MENU_SERIAL_PORT_FIRST + i;
            bool shown = i < port_count || (i == 0 && port_count == 0);
            menu_set_hidden(port_item, !shown);
            if (!shown) continue;
            menu_set_title(port_item, port_count ? ports[i] + 5 : "No serial ports found");
            menu_set_enabled(port_item, port_count > 0);
            menu_set_checked(port_item, port_count && serial.mode == SERIAL_DEVICE && !strcmp(serial.name, ports[i]));
        }
        if (power_release_at && machine_cycles(machine) >= power_release_at) {
            power_release_at = 0;
            machine_power_button(machine, false);
        }
        if (serial_plug_at && SDL_GetTicks() >= serial_plug_at) {
            serial_plug_at = 0;
            machine_serial_connect(machine, true);
        }
        reap_reveal_children();
        menu_set_checked(MENU_PAUSE, paused);
        for (int i = 0; i < PROFILES_MAX; i++) {
            int machine_item = MENU_MACHINE_FIRST + i;
            menu_set_hidden(machine_item, i >= profiles.count);
            if (i >= profiles.count) continue;
            menu_set_title(machine_item, profiles.entries[i].name);
            menu_set_checked(machine_item, i == current_index);
        }
        menu_set_enabled(MENU_NEW_MACHINE, profiles.count < PROFILES_MAX);
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
        set_title(window, current.name, notice, paused, machine_suspended(machine));
        since_autosave += elapsed;
        if (!paused) since_backup += elapsed;
        if (since_backup >= BACKUP_SECONDS) {
            since_backup = 0;
            backup_machine(machine, state);
        }
        if (since_autosave >= AUTOSAVE_SECONDS) {
            since_autosave = 0;
            machine_save(machine, state, (int64_t)time(NULL));
        }
        runner.paused = paused;
        typer_step(&typer, machine);
        scroller_step(&scroller, machine);


        screen = machine_screen_size(machine);
        if (screen.width != lcd_width() || screen.height != lcd_height()) {
            view_set_screen_size(view, screen.width, screen.height);
            if (!(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN)) fit_window(window, view, settings.scale);
        }
        lcd_set_power(machine_lcd_enabled(machine));
        lcd_set_backlight(machine_backlight(machine));
        uint32_t palette[LCD_PALETTE_MAX];
        lcd_set_palette(palette, machine_screen_palette(machine, palette));
        machine_screen(machine, lcd_framebuffer);
        bool lcd_on = machine_lcd_enabled(machine);
        SDL_UnlockMutex(runner.lock);
        bool screen_changed = view_update(view, (float)elapsed, lcd_on);
        if (screen_changed || events_seen || menu_active()) {
            view_render(view);
            menu_draw(renderer);
            SDL_RenderPresent(renderer);
        } else {
            uint64_t spent = SDL_GetTicksNS() - frame_start;
            if (spent < IDLE_FRAME_NS) SDL_DelayNS(IDLE_FRAME_NS - spent);
        }
    }

    SDL_SetAtomicInt(&runner.waiting, 1);
    SDL_LockMutex(runner.lock);
    runner.stop = true;
    SDL_SetAtomicInt(&runner.waiting, 0);
    SDL_UnlockMutex(runner.lock);
    SDL_WaitThread(runner.thread, NULL);
    gdb_destroy(debugger);
    debugger = NULL;
    agent_destroy(agent);
    serial_link_close(&serial);
    agent = NULL;
    SDL_DestroyMutex(runner.lock);
    free(picked);
    machine_save(machine, state, (int64_t)time(NULL));
    if (verbose) machine_dump_state(machine);
    view_destroy(view);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    machine_destroy(machine);
    return 0;
}
