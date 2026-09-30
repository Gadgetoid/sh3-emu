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
#include "rapi.h"

#include <fcntl.h>
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

typedef enum { SERIAL_OFF, SERIAL_NETWORK, SERIAL_PTY } serial_mode_t;

typedef struct {
    serial_mode_t mode;
    netgw_t *gateway;
    int      pty;
    int      pty_slave;
    char     pty_name[128];
    const char *user_agent;
} serial_t;

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
    }
    serial->mode = mode;
    machine_set_serial_tag(machine, (uint32_t)mode);
    if (mode != SERIAL_OFF) machine_serial_connect(machine, true);
    return mode == SERIAL_NETWORK ? "network cable connected" : mode == SERIAL_PTY ? serial->pty_name : "serial disconnected";
}

static void serial_restored(serial_t *serial, machine_t *machine, uint64_t *reconnect_at, serial_mode_t *reconnect_mode) {
    bool was_connected = machine_serial_connected(machine);
    serial_mode_t mode = (serial_mode_t)machine_serial_tag(machine);
    serial_close(serial, machine);
    *reconnect_at = 0;
    if (was_connected && (mode == SERIAL_NETWORK || mode == SERIAL_PTY)) {
        *reconnect_mode = mode;
        *reconnect_at = machine_cycles(machine) + 2ull * MACHINE_CLOCK_HZ;
    }
}

static void serial_pump(serial_t *serial, machine_t *machine) {
    uint8_t buffer[4096];
    size_t count;
    while ((count = machine_serial_take(machine, buffer, sizeof buffer)) > 0) {
        if (serial->gateway) netgw_from_guest(serial->gateway, buffer, count);
        else if (serial->pty >= 0 && write(serial->pty, buffer, count) < 0) break;
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

typedef enum { PICK_SEND = 1, PICK_FETCH, PICK_SHARED } pick_kind_t;

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

static void log_message(const char *message) {
    if (verbose) fputs(message, stderr);
}

static void state_path(char *path, size_t size, machine_t *machine, const char *rom_path) {
    const char *data_home = getenv("XDG_DATA_HOME");
    char base[1024];
    if (data_home && data_home[0] == '/') snprintf(base, sizeof base, "%s/velo-emu", data_home);
    else snprintf(base, sizeof base, "%s/.local/share/velo-emu", getenv("HOME") ? getenv("HOME") : ".");
    SDL_CreateDirectory(base);
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
    char     user_agent[256];
    char     shared_folder[1024];
} settings_t;

static void settings_path(char *path, size_t size) {
    const char *config_home = getenv("XDG_CONFIG_HOME");
    char base[1024];
    if (config_home && config_home[0] == '/') snprintf(base, sizeof base, "%s/velo-emu", config_home);
    else snprintf(base, sizeof base, "%s/.config/velo-emu", getenv("HOME") ? getenv("HOME") : ".");
    SDL_CreateDirectory(base);
    snprintf(path, size, "%s/emu.ini", base);
}

static settings_t settings_load(void) {
    settings_t settings = { 4, 1, NETGW_DEFAULT_USER_AGENT, "" };
    char path[1100];
    settings_path(path, sizeof path);
    FILE *file = fopen(path, "r");
    if (!file) return settings;
    char line[1200];
    unsigned value;
    while (fgets(line, sizeof line, file)) {
        if (sscanf(line, "memory=%u", &value) == 1) settings.memory = value;
        else if (sscanf(line, "speed=%u", &value) == 1) settings.speed = value;
        else if (!strncmp(line, "user_agent=", 11)) {
            line[strcspn(line, "\r\n")] = 0;
            snprintf(settings.user_agent, sizeof settings.user_agent, "%s", line + 11);
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
    fprintf(file, "memory=%u\nspeed=%u\nuser_agent=%s\nshared_folder=%s\n", settings->memory, settings->speed, settings->user_agent, settings->shared_folder);
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

static void reveal_in_finder(const char *path) {
    extern char **environ;
    char *arguments[] = { "open", "-R", (char *)path, NULL };
    pid_t pid;
    posix_spawnp(&pid, "open", NULL, NULL, arguments, environ);
}

static void set_title(SDL_Window *window, const char *notice, bool paused, bool suspended) {
    char title[128];
    if (notice) snprintf(title, sizeof title, "%s: %s", WINDOW_TITLE, notice);
    else if (paused) snprintf(title, sizeof title, "%s (paused)", WINDOW_TITLE);
    else if (suspended) snprintf(title, sizeof title, "%s (suspended)", WINDOW_TITLE);
    else snprintf(title, sizeof title, "%s", WINDOW_TITLE);
    SDL_SetWindowTitle(window, title);
}

static bool screen_position(SDL_Window *window, float window_x, float window_y, int *x, int *y) {
    int width, height;
    SDL_GetWindowSize(window, &width, &height);
    float grid_x = window_x / width * (LCD_WIDTH + 2 * LCD_MARGIN_X) - LCD_MARGIN_X;
    float grid_y = window_y / height * (LCD_HEIGHT + 2 * LCD_MARGIN_Y) - LCD_MARGIN_Y;
    *x = (int)grid_x;
    *y = (int)grid_y;
    if (*x < 0) *x = 0;
    if (*y < 0) *y = 0;
    if (*x >= LCD_WIDTH) *x = LCD_WIDTH - 1;
    if (*y >= LCD_HEIGHT) *y = LCD_HEIGHT - 1;
    return grid_x >= 0 && grid_y >= 0 && grid_x < LCD_WIDTH && grid_y < LCD_HEIGHT;
}

int main(int argc, char **argv) {
    const char *rom_path = NULL, *screenshot = NULL;
    double screenshot_seconds = 12;
    bool fresh = false;
    const char *card = NULL;
    const char *state_file = NULL;
    serial_mode_t serial_mode = SERIAL_OFF;
    settings_t settings = settings_load();
    for (int i = 1; i < argc; i++) {
        if (!strncmp(argv[i], "--memory=", 9)) { settings.memory = (uint32_t)atoi(argv[i] + 9); continue; }
        if (!strncmp(argv[i], "--speed=", 8)) { settings.speed = (uint32_t)atoi(argv[i] + 8); continue; }
        if (!strncmp(argv[i], "--user-agent=", 13)) { snprintf(settings.user_agent, sizeof settings.user_agent, "%s", argv[i] + 13); continue; }
        if (!strcmp(argv[i], "--serial=net")) { serial_mode = SERIAL_NETWORK; continue; }
        if (!strcmp(argv[i], "--serial=pty")) { serial_mode = SERIAL_PTY; continue; }
        if (!strncmp(argv[i], "--card=", 7)) { card = argv[i] + 7; continue; }
        if (!strncmp(argv[i], "--state=", 8)) { state_file = argv[i] + 8; continue; }
        if (!strcmp(argv[i], "--verbose")) verbose = true;
        else if (!strcmp(argv[i], "--fresh")) fresh = true;
        else if (!strncmp(argv[i], "--screenshot=", 13)) screenshot = argv[i] + 13;
        else if (!strncmp(argv[i], "--seconds=", 10)) screenshot_seconds = atof(argv[i] + 10);
        else rom_path = argv[i];
    }
    if (!rom_path) {
        fprintf(stderr, "usage: velo [--verbose] [--fresh] [--state=FILE] [--card=IMAGE] [--serial=net|pty] [--memory=4|8|16|20|32] [--speed=1|2|4|8] [--user-agent=TEXT] [--screenshot=FILE.bmp [--seconds=N]] nk.bin\n");
        return 2;
    }
    size_t rom_size;
    uint8_t *rom = read_file(rom_path, &rom_size);
    if (!rom) { fprintf(stderr, "cannot read %s\n", rom_path); return 1; }
    char error[256];
    machine_t *machine = machine_create(rom, rom_size, error, sizeof error);
    free(rom);
    if (!machine) { fprintf(stderr, "%s\n", error); return 1; }
    machine_set_log(machine, log_message);
    machine_set_memory(machine, settings.memory);
    machine_set_speed(machine, settings.speed);

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) { fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 1; }
    int grid_width = LCD_WIDTH + 2 * LCD_MARGIN_X, grid_height = LCD_HEIGHT + 2 * LCD_MARGIN_Y;
    SDL_Window *window = SDL_CreateWindow("Philips Velo 1", grid_width * WINDOW_SCALE, grid_height * WINDOW_SCALE,
                                          SDL_WINDOW_HIGH_PIXEL_DENSITY);
    SDL_Renderer *renderer = window ? SDL_CreateRenderer(window, NULL) : NULL;
    if (!renderer) { fprintf(stderr, "SDL: %s\n", SDL_GetError()); return 1; }
    SDL_SetRenderVSync(renderer, 1);

    int cell = (int)(WINDOW_SCALE * SDL_GetWindowPixelDensity(window) + 0.5f);
    lcd_compose_setup(cell);
    SDL_Texture *texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING,
                                             lcd_compose_width(), lcd_compose_height());
    SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_NEAREST);

    if (screenshot) {
        machine_run(machine, (uint64_t)(screenshot_seconds * MACHINE_CLOCK_HZ));
        lcd_set_power(machine_lcd_enabled(machine));
        lcd_set_backlight(machine_backlight(machine));
        machine_screen(machine, lcd_framebuffer);
        lcd_compose(10.0f);
        SDL_Surface *surface = SDL_CreateSurfaceFrom(lcd_compose_width(), lcd_compose_height(), SDL_PIXELFORMAT_RGBA32,
                                                     lcd_compose_pixels(), lcd_compose_width() * 4);
        bool saved = surface && SDL_SaveBMP(surface, screenshot);
        if (!saved) fprintf(stderr, "screenshot: %s\n", SDL_GetError());
        SDL_DestroySurface(surface);
        SDL_Quit();
        machine_destroy(machine);
        return saved ? 0 : 1;
    }

    char state[1100];
    if (state_file) snprintf(state, sizeof state, "%s", state_file);
    else state_path(state, sizeof state, machine, rom_path);
    int64_t saved_at;
    const char *startup_notice = NULL;
    if (!fresh) {
        if (machine_load(machine, state, &saved_at)) {
            machine_advance_clock(machine, (int64_t)time(NULL) - saved_at);
        } else {
            FILE *existing = fopen(state, "rb");
            if (existing) {
                fclose(existing);
                char backup[sizeof state + 8];
                static char moved_notice[sizeof backup + 64];
                snprintf(backup, sizeof backup, "%s.old", state);
                rename(state, backup);
                snprintf(moved_notice, sizeof moved_notice, "saved state unreadable, moved to %s", leaf_name(backup));
                startup_notice = moved_notice;
            }
        }
    }
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
    serial_t serial = { SERIAL_OFF, NULL, -1, -1, "", settings.user_agent };
    char rapi_socket[1024], sync_manifest[1024], desktop_notice[256], shared_notice[1200];
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
            case SDL_EVENT_WINDOW_FOCUS_LOST:
                release_keys(machine, held, -1);
                break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
                if (event.button.button == SDL_BUTTON_LEFT) {
                    int x, y;
                    if (screen_position(window, event.button.x, event.button.y, &x, &y)) {
                        pen_down = true;
                        machine_touch(machine, true, x, y);
                    }
                }
                break;
            case SDL_EVENT_MOUSE_MOTION:
                if (pen_down) {
                    int x, y;
                    screen_position(window, event.motion.x, event.motion.y, &x, &y);
                    machine_touch(machine, true, x, y);
                }
                break;
            case SDL_EVENT_MOUSE_BUTTON_UP:
                if (event.button.button == SDL_BUTTON_LEFT && pen_down) {
                    int x, y;
                    screen_position(window, event.button.x, event.button.y, &x, &y);
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
            default: break;
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
        menu_set_enabled(MENU_SERIAL_OFF, serial.mode != SERIAL_OFF);
        menu_set_checked(MENU_PAUSE, paused);
        menu_set_checked(MENU_BACKLIGHT, machine_backlight(machine));
        menu_set_checked(MENU_SOUND, sound);
        menu_set_checked(MENU_MEMORY_4, machine_memory_next(machine) == 4);
        menu_set_checked(MENU_MEMORY_8, machine_memory_next(machine) == 8);
        menu_set_checked(MENU_MEMORY_16, machine_memory_next(machine) == 16);
        menu_set_checked(MENU_MEMORY_20, machine_memory_next(machine) == 20);
        menu_set_checked(MENU_MEMORY_32, machine_memory_next(machine) == 32);
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
        set_title(window, notice, paused, machine_suspended(machine));
        since_autosave += elapsed;
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
        if (lcd_compose((float)elapsed)) {
            SDL_UpdateTexture(texture, NULL, lcd_compose_pixels(), lcd_compose_width() * 4);
        }
        SDL_RenderClear(renderer);
        SDL_RenderTexture(renderer, texture, NULL, NULL);
        SDL_RenderPresent(renderer);
    }

    machine_save(machine, state, (int64_t)time(NULL));
    serial_close(&serial, machine);
    desktop_destroy(desktop);
    if (verbose) machine_dump_state(machine);
    SDL_DestroyAudioStream(audio);
    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    machine_destroy(machine);
    return 0;
}
