#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "machine.h"
#include "netgw.h"

static void log_stderr(const char *message) { fputs(message, stderr); }

static volatile sig_atomic_t stop_requested = 0;

static void request_stop(int signal_number) {
    (void)signal_number;
    stop_requested = 1;
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

#define SCANCODE_SHIFT 0x51

typedef struct { char plain, shifted; uint8_t scancode; } key_char_t;

static const key_char_t key_chars[] = {
    { 'a', 'A', 0x14 }, { 'b', 'B', 0x2B }, { 'c', 'C', 0x2A }, { 'd', 'D', 0x2C }, { 'e', 'E', 0x28 },
    { 'f', 'F', 0x34 }, { 'g', 'G', 0x38 }, { 'h', 'H', 0x40 }, { 'i', 'I', 0x45 }, { 'j', 'J', 0x3C },
    { 'k', 'K', 0x44 }, { 'l', 'L', 0x36 }, { 'm', 'M', 0x3B }, { 'n', 'N', 0x33 }, { 'o', 'O', 0x3E },
    { 'p', 'P', 0x4D }, { 'q', 'Q', 0x26 }, { 'r', 'R', 0x30 }, { 's', 'S', 0x24 }, { 't', 'T', 0x2D },
    { 'u', 'U', 0x3D }, { 'v', 'V', 0x23 }, { 'w', 'W', 0x18 }, { 'x', 'X', 0x22 }, { 'y', 'Y', 0x35 },
    { 'z', 'Z', 0x12 },
    { '0', ')', 0x47 }, { '1', '!', 0x13 }, { '2', '@', 0x16 }, { '3', '#', 0x15 }, { '4', '$', 0x25 },
    { '5', '%', 0x17 }, { '6', '^', 0x27 }, { '7', '&', 0x2F }, { '8', '*', 0x37 }, { '9', '(', 0x3F },
    { ' ', 0, 0x21 }, { ';', ':', 0x4C }, { '=', '+', 0x4F }, { ',', '<', 0x43 }, { '-', '_', 0x4E },
    { '.', '>', 0x3A }, { '/', '?', 0x42 }, { '`', '~', 0x31 }, { '[', '{', 0x48 }, { '\\', '|', 0x50 },
    { ']', '}', 0x46 }, { '\'', '"', 0x2E }, { '\n', 0, 0x4B },
};

static void type_text(machine_t *machine, const char *text) {
    for (const char *c = text; *c; c++) {
        char ch = *c;
        if (ch == '\\' && c[1] == 'n') { ch = '\n'; c++; }
        for (size_t i = 0; i < sizeof key_chars / sizeof key_chars[0]; i++) {
            bool shifted = key_chars[i].shifted && key_chars[i].shifted == ch;
            if (key_chars[i].plain != ch && !shifted) continue;
            if (shifted) machine_key(machine, SCANCODE_SHIFT, false);
            machine_key(machine, key_chars[i].scancode, false);
            machine_run(machine, MACHINE_CLOCK_HZ / 50);
            machine_key(machine, key_chars[i].scancode, true);
            if (shifted) machine_key(machine, SCANCODE_SHIFT, true);
            machine_run(machine, MACHINE_CLOCK_HZ / 50);
            break;
        }
    }
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

static void write_pgm(const char *path, const uint8_t *levels) {
    FILE *file = fopen(path, "wb");
    if (!file) return;
    fprintf(file, "P5\n%d %d\n255\n", MACHINE_SCREEN_WIDTH, MACHINE_SCREEN_HEIGHT);
    for (int i = 0; i < MACHINE_SCREEN_WIDTH * MACHINE_SCREEN_HEIGHT; i++) fputc(255 - levels[i] * 17, file);
    fclose(file);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: headless ROM [--seconds=N] [--pgm=FILE] [--trace-pc] [--key=SECONDS:SCANCODE]... [--tap=SECONDS:X:Y[:HOLD]]... [--power=SECONDS]... [--backlight=SECONDS]... [--load=STATE] [--save=STATE] [--wav=FILE] [--memory=4|8|16|20|32] [--speed=N] [--card=IMAGE] [--serial=SECONDS] [--net=SECONDS] [--user-agent=TEXT] [--rapi=SOCKET] [--realtime[=N]] [--watch-pc=VA]... [--type=SECONDS:TEXT]... [--serial-send=SECONDS:TEXT]...\n");
        return 2;
    }
    double seconds = 5;
    const char *pgm = NULL, *load = NULL, *save = NULL, *wav = NULL, *card = NULL;
    bool trace_pc = false;
    double key_times[32];
    unsigned key_codes[32];
    int key_count = 0;
    double tap_times[32];
    int tap_x[32], tap_y[32];
    double tap_hold[32];
    int tap_count = 0;
    double power_times[8];
    int power_count = 0;
    double backlight_times[8];
    int backlight_count = 0;
    double serial_at = -1, net_at = -1, realtime = 0;
    uint32_t watches[MACHINE_WATCH_MAX];
    int watch_count = 0;
    netgw_t *gateway = NULL;
    netgw_options_t net_options = { NETGW_DEFAULT_USER_AGENT, NULL };
    double send_times[8];
    const char *send_text[8];
    int send_count = 0;
    double type_times[16];
    const char *type_strings[16];
    int type_count = 0;
    for (int i = 2; i < argc; i++) {
        if (!strncmp(argv[i], "--seconds=", 10)) seconds = atof(argv[i] + 10);
        else if (!strncmp(argv[i], "--pgm=", 6)) pgm = argv[i] + 6;
        else if (!strcmp(argv[i], "--trace-pc")) trace_pc = true;
        else if (!strncmp(argv[i], "--serial=", 9)) serial_at = atof(argv[i] + 9);
        else if (!strncmp(argv[i], "--net=", 6)) net_at = atof(argv[i] + 6);
        else if (!strncmp(argv[i], "--user-agent=", 13)) net_options.user_agent = argv[i] + 13;
        else if (!strncmp(argv[i], "--rapi=", 7)) net_options.rapi_socket = argv[i] + 7;
        else if (!strcmp(argv[i], "--realtime")) realtime = 1;
        else if (!strncmp(argv[i], "--watch-pc=", 11) && watch_count < MACHINE_WATCH_MAX) watches[watch_count++] = (uint32_t)strtoul(argv[i] + 11, NULL, 0);
        else if (!strncmp(argv[i], "--realtime=", 11)) realtime = atof(argv[i] + 11);
        else if (!strncmp(argv[i], "--type=", 7) && type_count < 16) {
            char *colon = strchr(argv[i] + 7, ':');
            if (colon) { type_times[type_count] = atof(argv[i] + 7); type_strings[type_count++] = colon + 1; }
        }
        else if (!strncmp(argv[i], "--serial-send=", 14) && send_count < 8) {
            char *colon = strchr(argv[i] + 14, ':');
            if (colon) { send_times[send_count] = atof(argv[i] + 14); send_text[send_count++] = colon + 1; }
        }
        else if (!strncmp(argv[i], "--backlight=", 12) && backlight_count < 8) backlight_times[backlight_count++] = atof(argv[i] + 12);
        else if (!strncmp(argv[i], "--power=", 8) && power_count < 8) power_times[power_count++] = atof(argv[i] + 8);
        else if (!strncmp(argv[i], "--wav=", 6)) wav = argv[i] + 6;
        else if (!strncmp(argv[i], "--card=", 7)) card = argv[i] + 7;
        else if (!strncmp(argv[i], "--load=", 7)) load = argv[i] + 7;
        else if (!strncmp(argv[i], "--save=", 7)) save = argv[i] + 7;
        else if (!strncmp(argv[i], "--tap=", 6) && tap_count < 32) {
            tap_hold[tap_count] = 0.5;
            if (sscanf(argv[i] + 6, "%lf:%d:%d:%lf", &tap_times[tap_count], &tap_x[tap_count], &tap_y[tap_count], &tap_hold[tap_count]) >= 3) tap_count++;
        }
        else if (!strncmp(argv[i], "--key=", 6) && key_count < 32) {
            if (sscanf(argv[i] + 6, "%lf:%x", &key_times[key_count], &key_codes[key_count]) == 2) key_count++;
        }
    }
    size_t rom_size;
    uint8_t *rom = read_file(argv[1], &rom_size);
    if (!rom) { fprintf(stderr, "cannot read %s\n", argv[1]); return 1; }
    char error[256];
    machine_t *machine = machine_create(rom, rom_size, error, sizeof error);
    if (!machine) { fprintf(stderr, "%s\n", error); return 1; }
    machine_set_log(machine, log_stderr);
    for (int i = 2; i < argc; i++) {
        if (!strncmp(argv[i], "--memory=", 9)) machine_set_memory(machine, (uint32_t)atoi(argv[i] + 9));
        else if (!strncmp(argv[i], "--speed=", 8)) machine_set_speed(machine, (uint32_t)atoi(argv[i] + 8));
    }
    if (load && !machine_load(machine, load, NULL)) { fprintf(stderr, "cannot load state %s\n", load); return 1; }
    if (load) machine_serial_connect(machine, false);
    for (int w = 0; w < watch_count; w++) machine_watch_pc(machine, watches[w]);
    if (card && !machine_insert_card(machine, card)) { fprintf(stderr, "cannot open card image %s\n", card); return 1; }

    FILE *wav_file = wav ? fopen(wav, "wb") : NULL;
    uint32_t wav_rate = 0;
    size_t wav_samples = 0;
    static int16_t audio[65536];
    if (wav_file) fseek(wav_file, 44, SEEK_SET);

    signal(SIGTERM, request_stop);
    signal(SIGINT, request_stop);
    double wall_start = wall_seconds();
    uint64_t cycles_start = machine_cycles(machine);
    uint64_t total = (uint64_t)(seconds * MACHINE_CLOCK_HZ);
    uint64_t slice = MACHINE_CLOCK_HZ / 10;
    for (uint64_t done = 0; done < total && !machine_halted(machine) && !stop_requested; done += slice) {
        for (int k = 0; k < key_count; k++) {
            uint64_t at = (uint64_t)(key_times[k] * MACHINE_CLOCK_HZ);
            if (at >= done && at < done + slice) {
                machine_key(machine, (uint8_t)key_codes[k], false);
                machine_run(machine, MACHINE_CLOCK_HZ / 20);
                machine_key(machine, (uint8_t)key_codes[k], true);
            }
        }
        for (int t = 0; t < tap_count; t++) {
            uint64_t at = (uint64_t)(tap_times[t] * MACHINE_CLOCK_HZ);
            if (at >= done && at < done + slice) {
                machine_touch(machine, true, tap_x[t], tap_y[t]);
                machine_run(machine, (uint64_t)(tap_hold[t] * MACHINE_CLOCK_HZ));
                machine_touch(machine, false, tap_x[t], tap_y[t]);
            }
        }
        if (net_at >= 0 && !gateway && (uint64_t)(net_at * MACHINE_CLOCK_HZ) < done + slice) {
            gateway = netgw_create(log_stderr, &net_options);
            machine_serial_connect(machine, true);
        }
        if (serial_at >= 0 && (uint64_t)(serial_at * MACHINE_CLOCK_HZ) >= done && (uint64_t)(serial_at * MACHINE_CLOCK_HZ) < done + slice) machine_serial_connect(machine, true);
        for (int k = 0; k < send_count; k++) {
            uint64_t at = (uint64_t)(send_times[k] * MACHINE_CLOCK_HZ);
            if (at >= done && at < done + slice) {
                char text[512];
                size_t length = 0;
                for (const char *c = send_text[k]; *c && length < sizeof text - 1; c++) {
                    if (c[0] == '\\' && c[1] == 'r') { text[length++] = '\r'; c++; }
                    else if (c[0] == '\\' && c[1] == 'n') { text[length++] = '\n'; c++; }
                    else text[length++] = *c;
                }
                machine_serial_send(machine, (const uint8_t *)text, length);
            }
        }
        for (int k = 0; k < type_count; k++) {
            uint64_t at = (uint64_t)(type_times[k] * MACHINE_CLOCK_HZ);
            if (at >= done && at < done + slice) type_text(machine, type_strings[k]);
        }
        for (int b = 0; b < backlight_count; b++) {
            uint64_t at = (uint64_t)(backlight_times[b] * MACHINE_CLOCK_HZ);
            if (at >= done && at < done + slice) {
                machine_backlight_button(machine, true);
                machine_run(machine, MACHINE_CLOCK_HZ / 10);
                machine_backlight_button(machine, false);
            }
        }
        for (int b = 0; b < power_count; b++) {
            uint64_t at = (uint64_t)(power_times[b] * MACHINE_CLOCK_HZ);
            if (at >= done && at < done + slice) {
                machine_power_button(machine, true);
                machine_run(machine, MACHINE_CLOCK_HZ / 5);
                machine_power_button(machine, false);
            }
        }
        if (gateway) {
            uint64_t step = MACHINE_CLOCK_HZ / 100;
            for (uint64_t ran = 0; ran < slice; ran += step) {
                machine_run(machine, step);
                pace(machine, realtime, wall_start, cycles_start);
                uint8_t buffer[4096];
                size_t count;
                while ((count = machine_serial_take(machine, buffer, sizeof buffer)) > 0) netgw_from_guest(gateway, buffer, count);
                netgw_poll(gateway, machine_cycles(machine) / (MACHINE_CLOCK_HZ / 1000));
                while ((count = netgw_to_guest(gateway, buffer, sizeof buffer)) > 0) machine_serial_send(machine, buffer, count);
            }
        } else {
            machine_run(machine, slice);
            pace(machine, realtime, wall_start, cycles_start);
        }
        {
            uint8_t tx[4096];
            size_t count = machine_serial_take(machine, tx, sizeof tx);
            if (count) {
                fprintf(stderr, "SERIAL TX %zu bytes at %u baud t=%.1f:", count, machine_serial_baud(machine), (double)machine_cycles(machine) / MACHINE_CLOCK_HZ);
                for (size_t i = 0; i < count && i < 64; i++) fprintf(stderr, " %02X", tx[i]);
                fprintf(stderr, "\n");
            }
        }
        if (wav_file) {
            uint32_t rate;
            size_t count = machine_audio(machine, audio, sizeof audio / sizeof audio[0], &rate);
            if (count && !wav_rate) wav_rate = rate;
            fwrite(audio, sizeof audio[0], count, wav_file);
            wav_samples += count;
        }
        if (trace_pc) fprintf(stderr, "t=%.1fs pc=%08X lcd=%d backlight=%d\n", (double)machine_cycles(machine) / MACHINE_CLOCK_HZ, machine_pc(machine), machine_lcd_enabled(machine), machine_backlight(machine));
    }
    machine_dump_state(machine);
    if (wav_file) {
        uint32_t rate = wav_rate ? wav_rate : 22050, data = (uint32_t)(wav_samples * 2), riff = data + 36, fmt = 16, byte_rate = rate * 2;
        uint16_t pcm = 1, channels = 1, align = 2, bits = 16;
        fseek(wav_file, 0, SEEK_SET);
        fwrite("RIFF", 1, 4, wav_file); fwrite(&riff, 4, 1, wav_file); fwrite("WAVEfmt ", 1, 8, wav_file);
        fwrite(&fmt, 4, 1, wav_file); fwrite(&pcm, 2, 1, wav_file); fwrite(&channels, 2, 1, wav_file);
        fwrite(&rate, 4, 1, wav_file); fwrite(&byte_rate, 4, 1, wav_file); fwrite(&align, 2, 1, wav_file);
        fwrite(&bits, 2, 1, wav_file); fwrite("data", 1, 4, wav_file); fwrite(&data, 4, 1, wav_file);
        fclose(wav_file);
        fprintf(stderr, "wav: %zu samples at %u Hz\n", wav_samples, rate);
    }
    if (save && !machine_save(machine, save, 0)) { fprintf(stderr, "cannot save state %s\n", save); return 1; }
    if (pgm) {
        static uint8_t levels[MACHINE_SCREEN_WIDTH * MACHINE_SCREEN_HEIGHT];
        machine_screen(machine, levels);
        write_pgm(pgm, levels);
    }
    netgw_destroy(gateway);
    machine_destroy(machine);
    free(rom);
    return 0;
}
