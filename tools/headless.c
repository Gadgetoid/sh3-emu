#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "core/agent.h"
#include "core/gdb.h"
#include "core/key_text.h"
#include "core/lcd.h"
#include "core/machine.h"
#include "net/net_link.h"
#include "util/file.h"
#include "util/options.h"
#include "util/png.h"

static void log_stderr(const char *message) { fputs(message, stderr); }

static volatile sig_atomic_t stop_requested = 0;

static void request_stop(int signal_number) {
    (void)signal_number;
    stop_requested = 1;
}

static gdb_t *debugger;
static agent_t *agent;
static net_link_t network;

#define AGENT_POLL_CYCLES (MACHINE_CLOCK_HZ / 100)

static void run_cycles(machine_t *machine, uint64_t cycles) {
    if (!debugger) machine_run(machine, cycles);
    else if (!gdb_run(debugger, cycles)) stop_requested = 1;
}

static void advance(machine_t *machine, uint64_t cycles) {
    if (!agent && !network.gateway) {
        run_cycles(machine, cycles);
        return;
    }
    while (cycles && !stop_requested) {
        uint64_t step = cycles < AGENT_POLL_CYCLES ? cycles : AGENT_POLL_CYCLES;
        run_cycles(machine, step);
        if (agent) agent_poll(agent, machine_mailbox(machine));
        net_link_pump(&network, machine);
        cycles -= step;
    }
}

static double wall_seconds(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)now.tv_sec + (double)now.tv_nsec / 1e9;
}

static void pace(machine_t *machine, double realtime, double wall_start, uint64_t cycles_start) {
    if (realtime <= 0) return;
    double due = wall_start + (double)(machine_cycles(machine) - cycles_start) / MACHINE_CLOCK_HZ / realtime;
    double ahead = due - wall_seconds();
    if (ahead <= 0) return;
    struct timespec wait = { (time_t)ahead, (long)((ahead - (double)(time_t)ahead) * 1e9) };
    nanosleep(&wait, NULL);
}

static void type_text(machine_t *machine, const char *text) {
    for (const char *c = text; *c; c++) {
        char ch = *c;
        if (ch == '\\' && c[1] == 'n') { ch = '\n'; c++; }
        uint8_t scancode;
        bool shifted;
        if (!key_text_find(machine_key_layout(machine), ch, &scancode, &shifted)) continue;
        if (shifted) machine_key(machine, KEY_TEXT_SHIFT, false);
        machine_key(machine, scancode, false);
        advance(machine, MACHINE_CLOCK_HZ / 50);
        machine_key(machine, scancode, true);
        if (shifted) machine_key(machine, KEY_TEXT_SHIFT, true);
        advance(machine, MACHINE_CLOCK_HZ / 50);
    }
}

static bool write_panel_png(const char *path, machine_t *machine, int cell, int backlight) {
    screen_size_t size = machine_screen_size(machine);
    lcd_set_size(size.width, size.height);
    lcd_compose_setup(cell);
    lcd_set_power(machine_lcd_enabled(machine));
    lcd_set_backlight(backlight < 0 ? machine_backlight(machine) : backlight != 0);
    uint32_t palette[LCD_PALETTE_MAX];
    lcd_set_palette(palette, machine_screen_palette(machine, palette));
    machine_screen(machine, lcd_framebuffer);
    lcd_compose(10.0f);
    uint8_t *png;
    size_t length;
    if (!png_encode(lcd_compose_pixels(), lcd_compose_width(), lcd_compose_height(), &png, &length)) return false;
    FILE *file = fopen(path, "wb");
    bool written = file && fwrite(png, 1, length, file) == length;
    if (file) fclose(file);
    free(png);
    return written;
}

static void write_pgm(const char *path, const uint8_t *levels, screen_size_t size) {
    FILE *file = fopen(path, "wb");
    if (!file) return;
    fprintf(file, "P5\n%d %d\n255\n", size.width, size.height);
    for (int i = 0; i < size.width * size.height; i++) fputc(255 - levels[i] * 17, file);
    fclose(file);
}

typedef struct {
    const char *rom_path;
    double   seconds;
    const char *png, *pgm, *load, *save, *card, *folder, *agent_socket, *gdb_process;
    int      png_cell, png_backlight, gdb_port;
    bool     trace_pc, host_time, trace_exceptions, debug_output, seconds_given;
    double   key_times[32];
    uint8_t  key_codes[32][4];
    int      key_lengths[32];
    int      key_count;
    double   tap_times[32];
    int      tap_x[32], tap_y[32];
    double   tap_hold[32];
    int      tap_count;
    double   type_times[16];
    const char *type_strings[16];
    int      type_count;
    double   power_times[8];
    int      power_count;
    double   soft_reset_at, realtime, net_at;
    bool     net;
    uint32_t watches[MACHINE_WATCH_MAX];
    int      watch_count;
    uint32_t memory, speed;
} run_t;

enum {
    OPT_HEADING_RUN, OPT_SECONDS, OPT_LOAD, OPT_SAVE, OPT_CARD, OPT_FOLDER, OPT_NET, OPT_MEMORY, OPT_SPEED, OPT_REALTIME, OPT_HOST_TIME,
    OPT_HEADING_INPUT, OPT_TAP, OPT_KEY, OPT_TYPE, OPT_POWER, OPT_SOFT_RESET,
    OPT_HEADING_OUTPUT, OPT_PGM, OPT_PNG, OPT_PNG_CELL, OPT_PNG_BACKLIGHT, OPT_TRACE_PC, OPT_WATCH_PC, OPT_DEBUG_OUTPUT, OPT_TRACE_EXCEPTIONS,
    OPT_HEADING_DEBUG, OPT_AGENT, OPT_GDB, OPT_GDB_PROCESS,
};

static const option_t OPTIONS[] = {
    [OPT_HEADING_RUN] = { NULL, NULL, "Running", 0 },
    [OPT_SECONDS] = { "seconds", "N", "emulated seconds to run (default 5)", 0 },
    [OPT_LOAD] = { "load", "STATE", "start from a saved state", 0 },
    [OPT_SAVE] = { "save", "STATE", "save the machine at the end (and on SIGTERM)", 0 },
    [OPT_CARD] = { "card", "IMAGE", "insert a CompactFlash card backed by a raw disk image, after --load", 0 },
    [OPT_FOLDER] = { "folder", "DIR", "serve DIR to CE's parallel-port file system (PPFS), for programs that aren't in ROM", 0 },
    [OPT_NET] = { "net", "[SECONDS]", "plug COM1 into the PPP network (default at 0 s, or 2 s after --load); CE dials it at boot", 0 },
    [OPT_MEMORY] = { "memory", "MB", "RAM for a cold boot: 16, 32 or 64", 0 },
    [OPT_SPEED] = { "speed", "N", "CPU speed multiple: 1, 2, 4 or 8", 0 },
    [OPT_REALTIME] = { "realtime", "[N]", "pace emulated time at N times real time (default 1), for agent clients", 0 },
    [OPT_HOST_TIME] = { "host-time", NULL, "set the clock from this computer at a cold boot", 0 },
    [OPT_HEADING_INPUT] = { NULL, NULL, "Input, at emulated times in seconds", 0 },
    [OPT_TAP] = { "tap", "SECONDS:X:Y[:HOLD]", "hold the pen at a screen position, for 0.5 s by default (0.08 for double taps)", 32 },
    [OPT_KEY] = { "key", "SECONDS:SCANCODE[+SCANCODE]", "press PS/2 set 2 scancodes (hex) for 50 ms, several joined by + as a chord; 80 and up are E0-prefixed", 32 },
    [OPT_TYPE] = { "type", "SECONDS:TEXT", "type text, with \\n for Enter", 16 },
    [OPT_POWER] = { "power", "SECONDS", "press the power button for 200 ms", 8 },
    [OPT_SOFT_RESET] = { "soft-reset", "SECONDS", "restart CE, keeping RAM", 0 },
    [OPT_HEADING_OUTPUT] = { NULL, NULL, "Output", 0 },
    [OPT_PGM] = { "pgm", "FILE", "save the raw greyscale screen at the end", 0 },
    [OPT_PNG] = { "png", "FILE", "save the screen through the simulated LCD at the end", 0 },
    [OPT_PNG_CELL] = { "png-cell", "N", "device pixels per LCD pixel for --png (default 4)", 0 },
    [OPT_PNG_BACKLIGHT] = { "png-backlight", "on|off", "draw --png lit or unlit", 0 },
    [OPT_TRACE_PC] = { "trace-pc", NULL, "print the program counter every 0.1 emulated seconds", 0 },
    [OPT_WATCH_PC] = { "watch-pc", "VA", "log registers each time the CPU reaches an address", MACHINE_WATCH_MAX },
    [OPT_DEBUG_OUTPUT] = { "debug-output", NULL, "print CE's debug serial port output to stderr", 0 },
    [OPT_TRACE_EXCEPTIONS] = { "trace-exceptions", NULL, "log CPU exceptions other than TLB misses and system calls", 0 },
    [OPT_HEADING_DEBUG] = { NULL, NULL, "Debugging", 0 },
    [OPT_AGENT] = { "agent", "SOCKET", "pass messages between a guest agent's trapa #0xCE mailbox and one client on this Unix socket", 0 },
    [OPT_GDB] = { "gdb", "PORT", "wait for GDB on 127.0.0.1:PORT before running; runs until GDB detaches or kills, unless --seconds is given", 0 },
    [OPT_GDB_PROCESS] = { "gdb-process", "NAME", "debug one process, e.g. maths.exe: breakpoints below 0x02000000 only stop there, and GDB stops when it starts", 0 },
};

static bool print_debug_output;

static void print_debug_line(void *context, const char *line) {
    (void)context;
    if (print_debug_output) fprintf(stderr, "debug: %s\n", line);
    if (debugger) gdb_debug_line(debugger, line);
}

static bool parse_option(void *context, int option, const char *value, char *error, size_t error_size) {
    run_t *run = context;
    long integer;
    const char *rest;
    switch (option) {
    case OPT_SECONDS:
        run->seconds_given = true;
        return option_number(value, &run->seconds) && run->seconds > 0;
    case OPT_LOAD: run->load = value; return true;
    case OPT_SAVE: run->save = value; return true;
    case OPT_CARD: run->card = value; return true;
    case OPT_FOLDER: run->folder = value; return true;
    case OPT_NET:
        run->net = true;
        run->net_at = -1;
        return !value || (option_number(value, &run->net_at) && run->net_at >= 0);
    case OPT_MEMORY:
        if (!option_integer(value, 10, &integer) || (integer != 16 && integer != 32 && integer != 64)) return false;
        run->memory = (uint32_t)integer;
        return true;
    case OPT_SPEED:
        if (!option_integer(value, 10, &integer) || (integer != 1 && integer != 2 && integer != 4 && integer != 8)) return false;
        run->speed = (uint32_t)integer;
        return true;
    case OPT_REALTIME:
        run->realtime = 1;
        return !value || (option_number(value, &run->realtime) && run->realtime > 0);
    case OPT_HOST_TIME: run->host_time = true; return true;
    case OPT_TAP: {
        int n = run->tap_count;
        if (!option_timed(value, &run->tap_times[n], &rest)) return false;
        run->tap_hold[n] = 0.5;
        int used = -1;
        if (sscanf(rest, "%d:%d%n", &run->tap_x[n], &run->tap_y[n], &used) != 2 || used < 0) return false;
        if (rest[used] == ':') {
            int hold_used = -1;
            if (sscanf(rest + used + 1, "%lf%n", &run->tap_hold[n], &hold_used) != 1 || hold_used <= 0 || run->tap_hold[n] <= 0) return false;
            used += 1 + hold_used;
        }
        if (rest[used]) return false;
        if (run->tap_x[n] < 0 || run->tap_x[n] >= SCREEN_MAX_WIDTH || run->tap_y[n] < 0 || run->tap_y[n] >= SCREEN_MAX_HEIGHT) {
            snprintf(error, error_size, "--tap position %d,%d is off the screen", run->tap_x[n], run->tap_y[n]);
            return false;
        }
        run->tap_count++;
        return true;
    }
    case OPT_KEY: {
        int n = run->key_count;
        if (!option_timed(value, &run->key_times[n], &rest)) return false;
        char codes[32];
        snprintf(codes, sizeof codes, "%s", rest);
        run->key_lengths[n] = 0;
        for (char *code = strtok(codes, "+"); code; code = strtok(NULL, "+")) {
            if (run->key_lengths[n] == 4 || !option_integer(code, 16, &integer) || integer < 0 || integer > 0xFF) return false;
            run->key_codes[n][run->key_lengths[n]++] = (uint8_t)integer;
        }
        if (!run->key_lengths[n]) return false;
        run->key_count++;
        return true;
    }
    case OPT_TYPE:
        if (!option_timed(value, &run->type_times[run->type_count], &rest)) return false;
        run->type_strings[run->type_count++] = rest;
        return true;
    case OPT_POWER: {
        double number;
        if (!option_number(value, &number) || number < 0) return false;
        run->power_times[run->power_count++] = number;
        return true;
    }
    case OPT_SOFT_RESET: return option_number(value, &run->soft_reset_at) && run->soft_reset_at >= 0;
    case OPT_PGM: run->pgm = value; return true;
    case OPT_PNG: run->png = value; return true;
    case OPT_PNG_CELL:
        if (!option_integer(value, 10, &integer) || integer < 2 || integer > 16) return false;
        run->png_cell = (int)integer;
        return true;
    case OPT_PNG_BACKLIGHT:
        if (strcmp(value, "on") && strcmp(value, "off")) return false;
        run->png_backlight = !strcmp(value, "on");
        return true;
    case OPT_TRACE_PC: run->trace_pc = true; return true;
    case OPT_WATCH_PC:
        if (!option_integer(value, 0, &integer) || integer < 0) return false;
        run->watches[run->watch_count++] = (uint32_t)integer;
        return true;
    case OPT_DEBUG_OUTPUT: run->debug_output = true; return true;
    case OPT_TRACE_EXCEPTIONS: run->trace_exceptions = true; return true;
    case OPT_AGENT: run->agent_socket = value; return true;
    case OPT_GDB:
        if (!option_integer(value, 10, &integer) || integer < 1 || integer > 65535) return false;
        run->gdb_port = (int)integer;
        return true;
    case OPT_GDB_PROCESS: run->gdb_process = value; return true;
    }
    return false;
}

static const option_spec_t SPEC = {
    "headless", "ROM [OPTIONS]",
    "Runs the Odo SH3 board without a window, for tests and scripts. ROM is a Platform Builder nk.bin (B000FF) RAM image, or a raw ROM image such as the Casio Cassiopeia A-51's.",
    OPTIONS, (int)(sizeof OPTIONS / sizeof OPTIONS[0]),
    "Events at or after --seconds don't happen, and are reported. Options taking a value also accept it as the next argument.",
};

static bool due(double at, uint64_t done, uint64_t slice) {
    if (at < 0) return false;
    uint64_t cycle = (uint64_t)(at * MACHINE_CLOCK_HZ);
    return cycle >= done && cycle < done + slice;
}

int main(int argc, char **argv) {
    static run_t run;
    run = (run_t){ .seconds = 5, .png_cell = 4, .png_backlight = -1, .soft_reset_at = -1 };
    const char *positional[1];
    int positional_count;
    options_result_t parsed = options_parse(&SPEC, argc, argv, parse_option, &run, positional, 1, &positional_count);
    if (parsed == OPTIONS_EXIT) return 0;
    if (parsed == OPTIONS_ERROR) return 2;
    if (!positional_count) {
        fprintf(stderr, "headless: no ROM given (see --help)\n");
        return 2;
    }
    run.rom_path = positional[0];
    struct stat folder_info;
    if (run.folder && (stat(run.folder, &folder_info) || !S_ISDIR(folder_info.st_mode))) {
        fprintf(stderr, "cannot open folder %s\n", run.folder);
        return 1;
    }
    if (run.gdb_port && !run.seconds_given) run.seconds = 1e7;
    double latest = run.soft_reset_at;
    for (int k = 0; k < run.key_count; k++) if (run.key_times[k] > latest) latest = run.key_times[k];
    for (int t = 0; t < run.tap_count; t++) if (run.tap_times[t] > latest) latest = run.tap_times[t];
    for (int k = 0; k < run.type_count; k++) if (run.type_times[k] > latest) latest = run.type_times[k];
    for (int b = 0; b < run.power_count; b++) if (run.power_times[b] > latest) latest = run.power_times[b];
    if (latest >= run.seconds) fprintf(stderr, "headless: an event at %.2f s is at or after --seconds=%.2f and won't happen\n", latest, run.seconds);
    if (run.agent_socket && !(agent = agent_create(run.agent_socket, log_stderr))) {
        fprintf(stderr, "cannot listen on agent socket %s\n", run.agent_socket);
        return 1;
    }
    size_t rom_size;
    uint8_t *rom = file_read(run.rom_path, &rom_size);
    if (!rom) { fprintf(stderr, "cannot read %s\n", run.rom_path); return 1; }
    char error[256];
    machine_t *machine = machine_create(rom, rom_size, error, sizeof error);
    if (!machine) { fprintf(stderr, "%s\n", error); return 1; }
    machine_set_log(machine, log_stderr);
    if (run.memory) machine_set_memory(machine, run.memory);
    if (run.speed) machine_set_speed(machine, run.speed);
    machine_set_host_clock(machine, run.host_time);
    print_debug_output = run.debug_output;
    if (run.debug_output || run.gdb_port) machine_set_debug_output(machine, print_debug_line, NULL);
    if (run.trace_exceptions) machine_trace_exceptions(machine, true);
    if (run.load && !machine_load(machine, run.load, NULL)) { fprintf(stderr, "cannot load state %s\n", run.load); return 1; }
    if (run.folder && !machine_set_host_folder(machine, run.folder)) { fprintf(stderr, "cannot open folder %s\n", run.folder); return 1; }
    if (run.card && !machine_insert_card(machine, run.card)) { fprintf(stderr, "cannot open card image %s\n", run.card); return 1; }
    for (int w = 0; w < run.watch_count; w++) machine_watch_pc(machine, run.watches[w]);
    if (run.net) {
        if (!net_gateway_available()) { fprintf(stderr, "headless: --net needs a build with libslirp\n"); return 2; }
        network.gateway = net_gateway_create(log_stderr);
        if (!network.gateway) { fprintf(stderr, "headless: cannot start the network\n"); return 1; }
        if (run.net_at < 0) run.net_at = run.load ? 2 : 0;
        machine_serial_connect(machine, false);
    }
    if (run.gdb_process && !run.gdb_port) {
        fprintf(stderr, "headless: --gdb-process needs --gdb\n");
        return 2;
    }
    if (run.gdb_port) {
        debugger = gdb_create(machine, run.gdb_port, log_stderr);
        if (!debugger) { fprintf(stderr, "cannot listen for GDB on port %d\n", run.gdb_port); return 1; }
        if (run.gdb_process && !gdb_set_process(debugger, run.gdb_process)) fprintf(stderr, "gdb: waiting for %s to start\n", run.gdb_process);
        fprintf(stderr, "gdb: waiting for a connection: target remote :%d\n", run.gdb_port);
        gdb_wait_for_client(debugger);
    }
    screen_size_t screen = machine_screen_size(machine);
    for (int t = 0; t < run.tap_count; t++) {
        if (run.tap_x[t] >= screen.width || run.tap_y[t] >= screen.height) {
            fprintf(stderr, "--tap position %d,%d is off the %ux%u screen\n", run.tap_x[t], run.tap_y[t], screen.width, screen.height);
            return 2;
        }
    }

    signal(SIGTERM, request_stop);
    signal(SIGINT, request_stop);
    double wall_start = wall_seconds();
    uint64_t cycles_start = machine_cycles(machine);
    uint64_t total = (uint64_t)(run.seconds * MACHINE_CLOCK_HZ);
    uint64_t slice = MACHINE_CLOCK_HZ / 10;
    for (uint64_t done = 0; done < total && !stop_requested; done += slice) {
        for (int k = 0; k < run.key_count; k++) {
            if (!due(run.key_times[k], done, slice)) continue;
            for (int c = 0; c < run.key_lengths[k]; c++) {
                machine_key(machine, run.key_codes[k][c], false);
                advance(machine, MACHINE_CLOCK_HZ / 50);
            }
            advance(machine, MACHINE_CLOCK_HZ / 20);
            for (int c = run.key_lengths[k] - 1; c >= 0; c--) {
                machine_key(machine, run.key_codes[k][c], true);
                advance(machine, MACHINE_CLOCK_HZ / 50);
            }
        }
        for (int t = 0; t < run.tap_count; t++) {
            if (!due(run.tap_times[t], done, slice)) continue;
            machine_touch(machine, true, run.tap_x[t], run.tap_y[t]);
            advance(machine, (uint64_t)(run.tap_hold[t] * MACHINE_CLOCK_HZ));
            machine_touch(machine, false, run.tap_x[t], run.tap_y[t]);
        }
        for (int k = 0; k < run.type_count; k++)
            if (due(run.type_times[k], done, slice)) type_text(machine, run.type_strings[k]);
        for (int b = 0; b < run.power_count; b++) {
            if (!due(run.power_times[b], done, slice)) continue;
            machine_power_button(machine, true);
            advance(machine, MACHINE_CLOCK_HZ / 5);
            machine_power_button(machine, false);
        }
        if (due(run.soft_reset_at, done, slice)) machine_soft_reset(machine);
        if (run.net && due(run.net_at, done, slice)) machine_serial_connect(machine, true);
        advance(machine, slice);
        pace(machine, run.realtime, wall_start, cycles_start);
        if (run.trace_pc) fprintf(stderr, "t=%.1fs pc=%08X lcd=%d\n", (double)machine_cycles(machine) / MACHINE_CLOCK_HZ, machine_pc(machine), machine_lcd_enabled(machine));
    }
    machine_dump_state(machine);
    if (run.save && !machine_save(machine, run.save, 0)) { fprintf(stderr, "cannot save state %s\n", run.save); return 1; }
    if (run.pgm) {
        static uint8_t levels[SCREEN_MAX_WIDTH * SCREEN_MAX_HEIGHT];
        machine_screen(machine, levels);
        write_pgm(run.pgm, levels, machine_screen_size(machine));
    }
    if (run.png && !write_panel_png(run.png, machine, run.png_cell, run.png_backlight)) { fprintf(stderr, "cannot write %s\n", run.png); return 1; }
    gdb_destroy(debugger);
    agent_destroy(agent);
    net_gateway_destroy(network.gateway);
    machine_destroy(machine);
    free(rom);
    return 0;
}
