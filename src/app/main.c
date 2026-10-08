#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "app/capture.h"
#include "app/desktop.h"
#include "app/host.h"
#include "app/input.h"
#include "app/launch.h"
#include "app/library.h"
#include "app/log.h"
#include "app/paths.h"
#include "app/picks.h"
#include "app/profiles.h"
#include "app/rom_catalog.h"
#include "app/settings.h"
#include "app/snapshot_store.h"
#include "app/typer.h"
#include "app/view.h"
#include "core/agent.h"
#include "core/gdb.h"
#include "core/key_text.h"
#include "core/lcd.h"
#include "core/machine.h"
#include "frontend/android/android.h"
#include "frontend/common/dialog.h"
#include "frontend/common/menu.h"
#include "net/net_gateway.h"
#include "net/serial_link.h"
#include "rapi/rapi.h"
#include "util/file.h"
#include "util/options.h"
#include "util/png.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <spawn.h>
#include <unistd.h>

#define WINDOW_SCALE     2
#define IDLE_FRAME_NS    (SDL_NS_PER_SECOND / 60)
#define CABLE_REPLUG_SECONDS 2
#define CABLE_BOOT_SECONDS   20
#define CABLE_RESET_SECONDS  30
#define SPEED_SETTLE_SECONDS 10
#define RUN_HOLD_NS      (4 * SDL_NS_PER_MS)
#define RUN_SLICE_CYCLES (MACHINE_CLOCK_HZ / 1000)
#define RUN_MAX_BEHIND   (MACHINE_CLOCK_HZ / 10)
#define MAX_FRAME_SLICE  0.1
#define AUTOSAVE_SECONDS 60
#define BACKUP_SECONDS   600
#define NOTICE_SECONDS   2
#define WINDOW_TITLE     "SH3Emu"
#define POWER_PRESS_SECONDS 0.2
#define BACKLIGHT_PRESS_SECONDS 0.1
#define AUDIO_CHUNK 8192
#define SERIAL_PORT_MAX  16
#define PORT_SCAN_MS     2000
#define ANDROID_UNLIT_LEVEL 0.5f

static gdb_t *debugger;
static snapshot_store_t snapshots;
static agent_t *agent;
static serial_link_t serial;

static uint64_t cable_plug_time(machine_t *machine) {
    uint64_t replug = machine_cycles(machine) + CABLE_REPLUG_SECONDS * (uint64_t)MACHINE_CLOCK_HZ;
    uint64_t booted = CABLE_BOOT_SECONDS * (uint64_t)MACHINE_CLOCK_HZ;
    return replug > booted ? replug : booted;
}

static const char *set_serial(machine_t *machine, serial_mode_t mode, const char *device, uint64_t *plug_at) {
    machine_serial_connect(machine, false);
    *plug_at = 0;
    bool same = serial.mode == mode && (mode != SERIAL_DEVICE || !strcmp(serial.name, device));
    if (!same) {
        const char *failure = serial_link_open(&serial, mode, device);
        if (failure) return failure;
        if (mode == SERIAL_PTY) fprintf(stderr, "serial: COM1 on %s\n", serial.name);
    }
    if (mode != SERIAL_OFF && mode != SERIAL_TCP) *plug_at = cable_plug_time(machine);
    return NULL;
}

typedef struct {
    SDL_Mutex  *lock;
    SDL_Thread *thread;
    machine_t  *machine;
    input_queue_t *input;
    bool paused, stop, restart;
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

static void print_debug_line(void *context, const char *line) {
    (void)context;
    if (debugger) gdb_debug_line(debugger, line);
    debug_log_line(line);
}

static void state_path(char *path, size_t size, machine_t *machine, const char *rom_path) {
    char base[1024];
    app_data_folder(base, sizeof base);
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

static gdb_t *start_network_gdb(machine_t *machine, uint32_t port, char *notice, size_t size) {
    gdb_t *gdb = gdb_create(machine, (int)port, true, app_log_always);
    char address[64];
    host_local_address(address, sizeof address);
    if (gdb) snprintf(notice, size, "GDB server at %s:%u", address, port);
    else snprintf(notice, size, "cannot listen for GDB on port %u", port);
    return gdb;
}

static const char *mount_dictionary(machine_t *machine, const settings_t *settings) {
    if (!settings->dictionary[0] || !machine_has_dictionary_slot(machine)) return NULL;
    return machine_mount_dictionary(machine, settings->dictionary) ? NULL : "could not open the dictionary image";
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
    if (mode == SERIAL_TCP) {
        char address[64];
        host_local_address(address, sizeof address);
        snprintf(notice, sizeof notice, "COM1 at %s:%d; the cable connects while a client is attached", address, serial.tcp_port);
        return notice;
    }
    snprintf(notice, sizeof notice, "COM1 on %s", serial.name);
    return notice;
}

static bool confirm_reset(SDL_Window *window, const char *name) {
    char title[160];
    snprintf(title, sizeof title, "Reset %s?", name);
    return host_confirm(window, title,
                        "A reset is a cold boot back to the factory state: it clears RAM, including files, settings and installed programs. A backup of the machine goes in Snapshots/Backups first. Soft Reset keeps them.",
                        "Reset");
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

static void migrate_profiles(profiles_t *profiles, const rom_set_t *roms, const settings_t *settings, const char *folder) {
    for (int system = MACHINE_BOARD_CASIO; system < MACHINE_BOARD_COUNT; system++) {
        if (!roms->path[system][0]) continue;
        profile_t profile = { .memory = settings->memory, .screen = settings->screen, .host_time = settings->host_time != 0 };
        snprintf(profile.name, sizeof profile.name, "%s", machine_board_name(system));
        snprintf(profile.rom, sizeof profile.rom, "%s", roms->path[system]);
        profile.screen = rom_catalog_screen(profile.rom, settings->screen);
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
    machine_set_log(machine, app_log);
    machine_set_memory(machine, profile->memory);
    machine_set_screen(machine, profile->screen);
    machine_set_speed(machine, speed);
    machine_set_host_clock(machine, profile->host_time);
    machine_set_debug_output(machine, print_debug_line, NULL);
    debug_log_start(rom_path);
    if (state_file) snprintf(state, state_size, "%s", state_file);
    else if (profile->state[0]) snprintf(state, state_size, "%s", profile->state);
    else state_path(state, state_size, machine, rom_path);
    if (fresh) {
        snapshot_store_backup_file(&snapshots, state);
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

static void window_size(view_display_t display, uint32_t scale, int *width, int *height) {
    view_source_size(display, width, height);
    *width = *width * WINDOW_SCALE * (int)scale / 100;
    *height = *height * WINDOW_SCALE * (int)scale / 100 + menu_bar_height();
}

static void fit_window(SDL_Window *window, view_t *view, uint32_t scale) {
#ifdef __ANDROID__
    (void)window;
    (void)view;
    (void)scale;
    return;
#endif
    if (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) SDL_SetWindowFullscreen(window, false);
    int width, height;
    window_size(view_display(view), scale, &width, &height);
    SDL_SetWindowSize(window, width, height);
}

int main(int argc, char **argv) {
    const char *rom_path = NULL;
#ifdef __ANDROID__
    const char *storage = SDL_GetAndroidExternalStoragePath();
    if (storage) {
        setenv("XDG_DATA_HOME", storage, 1);
        setenv("XDG_CONFIG_HOME", storage, 1);
    }
    const char *cache = SDL_GetAndroidCachePath();
    if (cache) setenv("TMPDIR", cache, 1);
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight Portrait");
#endif
    char base[1024];
    app_data_folder(base, sizeof base);
    snapshot_store_init(&snapshots, base);
    settings_t settings = settings_load();
    launch_t launch;
    options_result_t parsed = launch_parse(&launch, &settings, argc, argv);
    if (parsed == OPTIONS_EXIT) return 0;
    if (parsed == OPTIONS_ERROR) return 2;
    app_log_set_verbose(launch.verbose);
    debug_log_set_stderr(launch.debug_output);
    rom_path = launch.rom;
    const char *card = launch.card, *state_file = launch.state_file;
    bool fresh = launch.fresh;
    static rom_set_t roms;
    library_find_roms(&roms);
    char profiles_folder[1100];
    library_machines_folder(profiles_folder, sizeof profiles_folder);
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
#ifdef __ANDROID__
        while (current_index < 0 && library_first_run_import()) {
            library_find_roms(&roms);
            migrate_profiles(&profiles, &roms, &settings, profiles_folder);
            current_index = profiles.count ? 0 : -1;
        }
#endif
        if (current_index < 0) {
            library_show_no_roms();
            return 1;
        }
        current = profiles.entries[current_index];
    }
    char state[1100];
    const char *startup_notice = NULL;
    machine_t *machine = start_machine(&current, settings.speed, state_file, fresh, state, sizeof state, &startup_notice);
    if (!machine) { fprintf(stderr, "%s\n", startup_notice); return 1; }
    const char *dictionary_failure = mount_dictionary(machine, &settings);
    if (dictionary_failure && !startup_notice) startup_notice = dictionary_failure;
    if (current_index >= 0) {
        snprintf(settings.machine, sizeof settings.machine, "%s", current.id);
        settings_save(&settings);
    }
    key_layout_t key_layout = machine_key_layout(machine);
    screen_size_t screen = machine_screen_size(machine);
    lcd_set_size(screen.width, screen.height);

    SDL_SetAppMetadata("SH3Emu", options_version(), "sh3-emu");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) { fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 1; }
    picks_init();
    int window_width, window_height;
    window_size((view_display_t)settings.display, settings.scale, &window_width, &window_height);
    SDL_Window *window = SDL_CreateWindow(WINDOW_TITLE, window_width, window_height, SDL_WINDOW_HIGH_PIXEL_DENSITY);
    SDL_Renderer *renderer = window ? SDL_CreateRenderer(window, NULL) : NULL;
    if (!renderer) { fprintf(stderr, "SDL: %s\n", SDL_GetError()); return 1; }
    SDL_SetRenderVSync(renderer, 1);
#ifdef __ANDROID__
    SDL_SetWindowFullscreen(window, true);
    lcd_set_unlit_level(ANDROID_UNLIT_LEVEL);
#endif

    view_t *view = view_create(window, renderer, (view_display_t)settings.display, menu_bar_height());

    if (card && !machine_insert_card(machine, card)) fprintf(stderr, "cannot open card image %s\n", card);

    menu_install(window);

    SDL_AudioSpec audio_spec = { SDL_AUDIO_S16, 1, 22050 };
    SDL_AudioStream *audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &audio_spec, NULL, NULL);
    if (audio) SDL_ResumeAudioStreamDevice(audio);
    else fprintf(stderr, "audio: %s\n", SDL_GetError());
    bool sound = true;
    static int16_t samples[AUDIO_CHUNK];

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
        debugger = gdb_create(machine, launch.gdb_port, false, app_log_always);
        if (!debugger) {
            fprintf(stderr, "sh3emu: cannot listen for GDB on port %d\n", launch.gdb_port);
            return 1;
        }
        if (launch.gdb_process) gdb_set_process(debugger, launch.gdb_process);
    }
    static char gdb_notice[160];
    if (!debugger && settings.gdb_server) {
        debugger = start_network_gdb(machine, settings.gdb_port, gdb_notice, sizeof gdb_notice);
        if (!notice) {
            notice = gdb_notice;
            notice_left = NOTICE_SECONDS * 3;
        }
    }
    bool debugmgr_wanted = false, serial_tcp_attached = false;
    uint64_t serial_plug_at = 0, serial_unplug_at = 0, power_release_at = 0, backlight_release_at = 0, port_scan_at = 0;
    static char ports[SERIAL_PORT_MAX][SERIAL_LINK_PORT_NAME];
    int port_count = 0;
    serial_link_init(&serial, NULL);
    serial.tcp_port = (int)settings.serial_tcp_port;
    serial.options.user_agent = settings.user_agent;
    static char rapi_socket[1024], sync_manifest[1024];
    char desktop_notice[256], shared_notice[1200];
#ifdef __ANDROID__
    if (net_gateway_socket_path(rapi_socket, sizeof rapi_socket, "sh3emu-rapi")) serial.options.rapi_socket = rapi_socket;
#else
    if (rapi_data_path("rapi.sock", rapi_socket, sizeof rapi_socket)) serial.options.rapi_socket = rapi_socket;
#endif
    serial.options.rapi_port = settings.network_rapi ? (int)settings.rapi_port : 0;
    rapi_data_path("sync-manifest.txt", sync_manifest, sizeof sync_manifest);
    desktop_t *desktop = desktop_create(rapi_socket, sync_manifest);
    const char *serial_failure = set_serial(machine, (serial_mode_t)settings.serial, settings.serial_device, &serial_plug_at);
    if (serial_failure) {
        fprintf(stderr, "sh3emu: %s\n", serial_failure);
        settings.serial = SERIAL_OFF;
    }
    if (launch.agent_socket && !(agent = agent_create(launch.agent_socket, app_log_always))) {
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
            case SDL_EVENT_TERMINATING:
                running = false;
                break;
            case SDL_EVENT_WILL_ENTER_BACKGROUND:
                machine_save(machine, state, (int64_t)time(NULL));
                break;
            case SDL_EVENT_KEY_DOWN:
            case SDL_EVENT_KEY_UP: {
                bool down = event.type == SDL_EVENT_KEY_DOWN;
                uint8_t scancode;
                if (!input_find_scancode(key_layout, event.key.key, &scancode)) break;
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
                    static char drop_notice[1200];
                    picks_handle_drop(&dropped, machine, desktop, serial.gateway && net_gateway_online(serial.gateway) && !desktop_busy(desktop), drop_notice, sizeof drop_notice);
                    notice = drop_notice;
                    notice_left = NOTICE_SECONDS * 2;
                }
                break;
            case SDL_EVENT_WINDOW_FOCUS_GAINED:
                library_find_roms(&roms);
#ifdef __ANDROID__
                SDL_SetWindowFullscreen(window, false);
                SDL_SetWindowFullscreen(window, true);
#endif
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
            {
                picked_t *taken = picks_take(&event);
                if (taken) {
                    free(picked);
                    picked = taken;
                }
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
            case MENU_BACKLIGHT:
                machine_backlight_button(machine, true);
                backlight_release_at = machine_cycles(machine) + (uint64_t)(BACKLIGHT_PRESS_SECONDS * MACHINE_CLOCK_HZ);
                break;
            case MENU_PAUSE: paused = !paused; break;
            case MENU_SOUND: sound = !sound; break;
            case MENU_SOFT_RESET:
                machine_soft_reset(machine);
                set_serial(machine, (serial_mode_t)settings.serial, settings.serial_device, &serial_plug_at);
                if (serial_plug_at) serial_plug_at = machine_cycles(machine) + CABLE_RESET_SECONDS * (uint64_t)MACHINE_CLOCK_HZ;
                break;
            case MENU_NEW_MACHINE: {
                static dialog_rom_t rom_list[32];
                int rom_count = library_list_roms(rom_list, 32);
                dialog_machine_t chosen = { .memory = 16, .screen = { SCREEN_STOCK_WIDTH, SCREEN_STOCK_HEIGHT }, .host_time = settings.host_time != 0 };
                if (rom_count) snprintf(chosen.rom, sizeof chosen.rom, "%s", current.rom);
                events_seen = true;
                if (!dialog_new_machine(window, rom_list, rom_count, rom_catalog_label, &chosen)) break;
                profile_t made = { .screen = chosen.screen, .memory = chosen.memory, .host_time = chosen.host_time };
                snprintf(made.rom, sizeof made.rom, "%s", chosen.rom);
                if (chosen.name[0]) snprintf(made.name, sizeof made.name, "%s", chosen.name);
                else profile_default_name(&made, rom_catalog_probe(made.rom, NULL), made.name, sizeof made.name);
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
                        snapshot_store_backup_machine(&snapshots, machine, state);
                        since_backup = 0;
                        machine_reset(machine);
                    } else {
                        snapshot_store_backup_file(&snapshots, picked_profile.state);
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
                    if (!host_confirm(window, title, "This removes the machine and its saved state. A backup of the state goes in Snapshots/Backups first.", "Delete")) break;
                    snapshot_store_backup_file(&snapshots, picked_profile.state);
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
                int index = settings_scale_index(settings.scale);
                if (item == MENU_ZOOM_IN) index = index + 1 < SETTINGS_SCALE_COUNT ? index + 1 : index;
                else if (item == MENU_ZOOM_OUT) index = index > 0 ? index - 1 : index;
                else index = item - MENU_SCALE_50;
                settings.scale = settings_scale_at(index);
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
                notice = capture_copy_screen(view) ? "screen copied" : "could not copy the screen";
                notice_left = NOTICE_SECONDS;
                break;
            case MENU_SAVE_SCREENSHOT: {
                static char screenshot_notice[1200];
                char path[1100];
                if (capture_save_screenshot(view, path, sizeof path)) snprintf(screenshot_notice, sizeof screenshot_notice, "saved %s", file_leaf_name(path));
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
                snapshot_store_backup_machine(&snapshots, machine, state);
                notice = machine_load(machine, state, NULL) ? "state loaded" : "no saved state";
                set_serial(machine, (serial_mode_t)settings.serial, settings.serial_device, &serial_plug_at);
                notice_left = NOTICE_SECONDS;
                break;
            case MENU_SHOW_STATE: host_reveal_file(state); break;
            case MENU_SAVE_SNAPSHOT: {
                static const SDL_DialogFileFilter filters[] = { { "Snapshot", "state" } };
                static char default_snapshot[1200];
                snapshot_store_default_name(&snapshots, default_snapshot, sizeof default_snapshot);
                SDL_ShowSaveFileDialog(picks_done, (void *)(intptr_t)PICK_SAVE_SNAPSHOT, window, filters, 1, default_snapshot);
                break;
            }
            case MENU_LOAD_SNAPSHOT: {
                static const SDL_DialogFileFilter filters[] = { { "Snapshot", "state;bin" } };
                static char folder[1100];
                snapshot_store_folder(&snapshots, folder, sizeof folder);
                SDL_ShowOpenFileDialog(picks_done, (void *)(intptr_t)PICK_LOAD_SNAPSHOT, window, filters, 1, folder, false);
                break;
            }
            case MENU_SHOW_DEBUG_OUTPUT: {
                char path[1100];
                debug_log_path(path, sizeof path);
                debug_log_flush();
                host_open_path(path);
                break;
            }
            case MENU_QUIT:
                running = false;
                break;
            case MENU_GDB_SERVER:
                if (debugger) {
                    gdb_destroy(debugger);
                    debugger = NULL;
                    settings.gdb_server = 0;
                    notice = "GDB server stopped";
                } else {
                    debugger = start_network_gdb(machine, settings.gdb_port, gdb_notice, sizeof gdb_notice);
                    settings.gdb_server = debugger != NULL;
                    notice = gdb_notice;
                    debugmgr_wanted = debugger != NULL;
                }
                settings_save(&settings);
                notice_left = NOTICE_SECONDS * 3;
                break;
#ifdef __ANDROID__
            case MENU_IMPORT: {
                static char import_notice[160];
                int cards, imported = library_import_files(&cards);
                library_find_roms(&roms);
                snprintf(import_notice, sizeof import_notice, "imported %d ROMs and %d cards", imported, cards);
                notice = import_notice;
                notice_left = NOTICE_SECONDS * 2;
                break;
            }
            case MENU_FULL_BRIGHTNESS:
                settings.full_brightness = !settings.full_brightness;
                settings_save(&settings);
                break;
            case MENU_FETCH_DOCUMENTS:
            case MENU_SHARED_FOLDER: {
                if (!android_all_files_access()) {
                    android_request_all_files_access();
                    notice = "allow All files access for SH3Emu, then try again";
                    notice_left = NOTICE_SECONDS * 3;
                    break;
                }
                bool shared = item == MENU_SHARED_FOLDER;
                picked_t *folder = calloc(1, sizeof *folder);
                const char *start = shared && settings.shared_folder[0] ? settings.shared_folder : "/storage/emulated/0/Documents";
                if (folder && android_choose_folder(shared ? "Folder to share with My Documents" : "Folder to copy My Documents into", start, folder->paths[0], sizeof folder->paths[0])) {
                    folder->kind = shared ? PICK_SHARED : PICK_FETCH;
                    folder->count = 1;
                    free(picked);
                    picked = folder;
                } else {
                    free(folder);
                }
                events_seen = true;
                break;
            }
#else
            case MENU_FETCH_DOCUMENTS:
                SDL_ShowOpenFolderDialog(picks_done, (void *)(intptr_t)PICK_FETCH, window, NULL, false);
                break;
            case MENU_SHARED_FOLDER:
                SDL_ShowOpenFolderDialog(picks_done, (void *)(intptr_t)PICK_SHARED, window, settings.shared_folder[0] ? settings.shared_folder : NULL, false);
                break;
#endif
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
                SDL_ShowOpenFileDialog(picks_done, (void *)(intptr_t)PICK_CARD, window, filters, 2, NULL, false);
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
            case MENU_SERIAL_PTY:
            case MENU_SERIAL_TCP: {
                serial_mode_t mode = item == MENU_SERIAL_NETWORK ? SERIAL_NETWORK : item == MENU_SERIAL_PTY ? SERIAL_PTY : item == MENU_SERIAL_TCP ? SERIAL_TCP : SERIAL_OFF;
                notice = serial_choice(machine, &settings, mode, NULL, &serial_plug_at);
                notice_left = NOTICE_SECONDS * 3;
                break;
            }
            case MENU_EJECT_CARD:
                machine_eject_card(machine);
                notice = "card ejected";
                notice_left = NOTICE_SECONDS;
                break;
            case MENU_MOUNT_DICTIONARY: {
                static const SDL_DialogFileFilter filters[] = { { "Dictionary images", "bin;rom" }, { "All files", "*" } };
                SDL_ShowOpenFileDialog(picks_done, (void *)(intptr_t)PICK_DICTIONARY, window, filters, 2, settings.dictionary[0] ? settings.dictionary : NULL, false);
                break;
            }
            case MENU_UNMOUNT_DICTIONARY:
                machine_unmount_dictionary(machine);
                settings.dictionary[0] = 0;
                settings_save(&settings);
                notice = "dictionary unmounted";
                notice_left = NOTICE_SECONDS;
                break;
            case MENU_SEND_FILES:
                SDL_ShowOpenFileDialog(picks_done, (void *)(intptr_t)PICK_SEND, window, NULL, 0, NULL, true);
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
            case MENU_NETWORK_RAPI: {
                static char rapi_notice[160];
                settings.network_rapi = !settings.network_rapi;
                settings_save(&settings);
                serial.options.rapi_port = settings.network_rapi ? (int)settings.rapi_port : 0;
                char address[64];
                host_local_address(address, sizeof address);
                if (settings.network_rapi) snprintf(rapi_notice, sizeof rapi_notice, "RAPI at %s:%u", address, settings.rapi_port);
                else snprintf(rapi_notice, sizeof rapi_notice, "RAPI over the network off");
                if (serial.mode == SERIAL_NETWORK) {
                    serial_link_close(&serial);
                    set_serial(machine, SERIAL_NETWORK, NULL, &serial_plug_at);
                }
                notice = rapi_notice;
                notice_left = NOTICE_SECONDS * 3;
                break;
            }
            case MENU_STOP_SHARING:
                snprintf(shared_notice, sizeof shared_notice, "stopped sharing %s", file_leaf_name(settings.shared_folder));
                settings.shared_folder[0] = 0;
                settings_save(&settings);
                notice = shared_notice;
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
                    power_release_at = backlight_release_at = serial_unplug_at = 0;
                    const char *dictionary_notice = mount_dictionary(machine, &settings);
                    if (dictionary_notice) switch_notice = dictionary_notice;
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
            } else if (picked->kind == PICK_DICTIONARY) {
                if (machine_mount_dictionary(machine, picked->paths[0])) {
                    snprintf(settings.dictionary, sizeof settings.dictionary, "%s", picked->paths[0]);
                    settings_save(&settings);
                    notice = "dictionary mounted; restart the dictionary app to use it";
                } else {
                    notice = "could not open the dictionary image (up to 8 MB)";
                }
                notice_left = NOTICE_SECONDS * 2;
            } else if (picked->kind == PICK_SAVE_SNAPSHOT) {
                static char snapshot_notice[1200];
                char path[1100];
                snprintf(path, sizeof path, "%s%s", picked->paths[0], file_has_extension(picked->paths[0], ".state") ? "" : ".state");
                bool saved = machine_save(machine, path, (int64_t)time(NULL));
#ifdef __ANDROID__
                if (saved && picked->export_uri[0]) {
                    saved = android_export(path, picked->export_uri);
                    remove(path);
                }
#endif
                snprintf(snapshot_notice, sizeof snapshot_notice, saved ? "saved snapshot %s" : "could not save %s", file_leaf_name(path));
                notice = snapshot_notice;
                notice_left = NOTICE_SECONDS * 2;
            } else if (picked->kind == PICK_SEND) {
                const char *files[PICK_MAX + 1];
                for (int i = 0; i < picked->count; i++) files[i] = picked->paths[i];
                files[picked->count] = NULL;
                desktop_send(desktop, files);
            } else if (picked->kind == PICK_FETCH) {
                desktop_fetch(desktop, picked->paths[0]);
            } else if (picked->kind == PICK_SHARED) {
                snprintf(settings.shared_folder, sizeof settings.shared_folder, "%s", picked->paths[0]);
                settings_save(&settings);
                snprintf(shared_notice, sizeof shared_notice, "sharing %s with \\My Documents", file_leaf_name(settings.shared_folder));
                notice = shared_notice;
                notice_left = NOTICE_SECONDS * 2;
                if (serial.gateway && net_gateway_online(serial.gateway)) desktop_sync(desktop, settings.shared_folder);
            } else if (picked->kind == PICK_LOAD_SNAPSHOT) {
                static char snapshot_notice[1200];
                if (machine_state_matches(machine, picked->paths[0])) snapshot_store_backup_machine(&snapshots, machine, state);
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
        bool device_online = serial.gateway && net_gateway_online(serial.gateway);
        if (serial.gateway && net_gateway_take_desktop_connected(serial.gateway)) {
            if (settings.shared_folder[0]) desktop_sync(desktop, settings.shared_folder);
            debugmgr_wanted = debugger != NULL;
        }
        if (!device_online) debugmgr_wanted = false;
        if (debugmgr_wanted && !desktop_busy(desktop)) {
            debugmgr_wanted = false;
            if (debugger && !machine_agent_running(machine)) desktop_install_debugmgr(desktop);
        }
        if (desktop_take_reconnect(desktop) && serial.mode == SERIAL_NETWORK) serial_unplug_at = machine_cycles(machine) + SPEED_SETTLE_SECONDS * (uint64_t)MACHINE_CLOCK_HZ;
        if (serial_unplug_at && machine_cycles(machine) >= serial_unplug_at) {
            serial_unplug_at = 0;
            if (serial.mode == SERIAL_NETWORK) {
                serial_link_close(&serial);
                set_serial(machine, SERIAL_NETWORK, NULL, &serial_plug_at);
            }
        }
        if (desktop_take_status(desktop, desktop_notice, sizeof desktop_notice)) {
            notice = desktop_notice;
            notice_left = NOTICE_SECONDS * 2;
        }
        bool desktop_free = device_online && !desktop_busy(desktop);
        menu_ensure();
        menu_set_enabled(MENU_SEND_FILES, desktop_free);
        menu_set_enabled(MENU_FETCH_DOCUMENTS, desktop_free);
        menu_set_enabled(MENU_SYNC_NOW, desktop_free && settings.shared_folder[0]);
        menu_set_enabled(MENU_STOP_SHARING, settings.shared_folder[0] != 0);
        menu_set_enabled(MENU_SET_PROXY, desktop_free);
        static const uint32_t LINK_SPEEDS[] = { 19200, 38400, 57600, 115200 };
        bool speed_settable = desktop_free && machine_rom_system(machine) == MACHINE_BOARD_HP;
        uint32_t link_baud = device_online ? machine_serial_baud(machine) : 0;
        for (int baud_item = MENU_BAUD_19200; baud_item <= MENU_BAUD_115200; baud_item++) {
            uint32_t speed = LINK_SPEEDS[baud_item - MENU_BAUD_19200];
            menu_set_enabled(baud_item, speed_settable);
            menu_set_checked(baud_item, link_baud && link_baud * 20 > speed * 19 && link_baud * 20 < speed * 21);
        }
        menu_set_checked(MENU_NETWORK_RAPI, settings.network_rapi != 0);
        menu_set_enabled(MENU_EJECT_CARD, machine_card_inserted(machine));
        menu_set_enabled(MENU_MOUNT_DICTIONARY, machine_has_dictionary_slot(machine));
        menu_set_enabled(MENU_UNMOUNT_DICTIONARY, machine_dictionary_mounted(machine));
        menu_set_checked(MENU_BACKLIGHT, machine_backlight(machine));
        menu_set_enabled(MENU_SERIAL_NETWORK, net_gateway_available());
        menu_set_checked(MENU_SERIAL_OFF, serial.mode == SERIAL_OFF);
        menu_set_checked(MENU_SERIAL_NETWORK, serial.mode == SERIAL_NETWORK);
        menu_set_checked(MENU_SERIAL_PTY, serial.mode == SERIAL_PTY);
        menu_set_checked(MENU_SERIAL_TCP, serial.mode == SERIAL_TCP);
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
        if (backlight_release_at && machine_cycles(machine) >= backlight_release_at) {
            backlight_release_at = 0;
            machine_backlight_button(machine, false);
        }
        if (power_release_at && machine_cycles(machine) >= power_release_at) {
            power_release_at = 0;
            machine_power_button(machine, false);
        }
        if (serial.mode == SERIAL_TCP && serial_link_attached(&serial) != machine_serial_connected(machine)) {
            bool attached = serial_link_attached(&serial);
            machine_serial_connect(machine, attached);
            if (attached != serial_tcp_attached) {
                notice = attached ? "TCP client connected" : "TCP client disconnected";
                notice_left = NOTICE_SECONDS;
            }
            serial_tcp_attached = attached;
        }
        if (serial_plug_at && machine_cycles(machine) >= serial_plug_at) {
            serial_plug_at = 0;
            machine_serial_connect(machine, true);
        }
        host_reap_children();
        menu_set_checked(MENU_PAUSE, paused);
        menu_set_checked(MENU_GDB_SERVER, debugger != NULL);
        menu_set_checked(MENU_FULL_BRIGHTNESS, settings.full_brightness != 0);
        menu_set_checked(MENU_SOUND, sound);
        for (int i = 0; i < PROFILES_MAX; i++) {
            int machine_item = MENU_MACHINE_FIRST + i;
            menu_set_hidden(machine_item, i >= profiles.count);
            if (i >= profiles.count) continue;
            menu_set_title(machine_item, profiles.entries[i].name);
            menu_set_checked(machine_item, i == current_index);
        }
        menu_set_enabled(MENU_NEW_MACHINE, profiles.count < PROFILES_MAX);
        for (int scale_item = MENU_SCALE_50; scale_item <= MENU_SCALE_200; scale_item++) menu_set_checked(scale_item, settings.scale == settings_scale_at(scale_item - MENU_SCALE_50));
        menu_set_enabled(MENU_ZOOM_IN, settings.scale < settings_scale_at(SETTINGS_SCALE_COUNT - 1));
        menu_set_enabled(MENU_ZOOM_OUT, settings.scale > settings_scale_at(0));
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
#ifdef __ANDROID__
        if (android_toast(notice ? notice : paused ? "Paused" : NULL)) events_seen = true;
#endif
        since_autosave += elapsed;
        if (!paused) since_backup += elapsed;
        if (since_backup >= BACKUP_SECONDS) {
            since_backup = 0;
            snapshot_store_backup_machine(&snapshots, machine, state);
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
        lcd_set_backlight_colour(machine_backlight_colour(machine));
        lcd_set_backlight(machine_backlight(machine));
#ifdef __ANDROID__
        android_update(settings.full_brightness && machine_backlight(machine) && machine_lcd_enabled(machine), !machine_suspended(machine));
#endif
        uint32_t rate;
        for (size_t count; (count = machine_audio(machine, samples, AUDIO_CHUNK, &rate)) > 0;) {
            if (!audio || !sound) continue;
            if ((int)rate != audio_spec.freq) {
                audio_spec.freq = (int)rate;
                SDL_SetAudioStreamFormat(audio, &audio_spec, NULL);
            }
            SDL_PutAudioStreamData(audio, samples, (int)(count * sizeof samples[0]));
        }
        uint32_t palette[LCD_PALETTE_MAX];
        lcd_set_palette(palette, machine_screen_palette(machine, palette));
        machine_screen(machine, lcd_framebuffer);
        bool lcd_on = machine_lcd_enabled(machine);
        SDL_UnlockMutex(runner.lock);
        int inset_left, inset_top, inset_right, inset_bottom;
        menu_insets(&inset_left, &inset_top, &inset_right, &inset_bottom);
        view_set_insets(view, inset_left, inset_top, inset_right, inset_bottom);
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
    desktop_destroy(desktop);
    serial_link_close(&serial);
    agent = NULL;
    SDL_DestroyMutex(runner.lock);
    free(picked);
    machine_save(machine, state, (int64_t)time(NULL));
    if (app_log_verbose()) machine_dump_state(machine);
    view_destroy(view);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    machine_destroy(machine);
    return 0;
}
