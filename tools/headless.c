#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "machine.h"

static void log_stderr(const char *message) { fputs(message, stderr); }

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
    for (int i = 0; i < MACHINE_SCREEN_WIDTH * MACHINE_SCREEN_HEIGHT; i++) fputc(255 - levels[i] * 85, file);
    fclose(file);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: headless ROM [--seconds=N] [--pgm=FILE] [--trace-pc] [--key=SECONDS:SCANCODE]... [--tap=SECONDS:X:Y[:HOLD]]... [--power=SECONDS]... [--load=STATE] [--save=STATE] [--wav=FILE] [--card=IMAGE]\n");
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
    for (int i = 2; i < argc; i++) {
        if (!strncmp(argv[i], "--seconds=", 10)) seconds = atof(argv[i] + 10);
        else if (!strncmp(argv[i], "--pgm=", 6)) pgm = argv[i] + 6;
        else if (!strcmp(argv[i], "--trace-pc")) trace_pc = true;
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
    if (load && !machine_load(machine, load, NULL)) { fprintf(stderr, "cannot load state %s\n", load); return 1; }
    if (card && !machine_insert_card(machine, card)) { fprintf(stderr, "cannot open card image %s\n", card); return 1; }

    FILE *wav_file = wav ? fopen(wav, "wb") : NULL;
    uint32_t wav_rate = 0;
    size_t wav_samples = 0;
    static int16_t audio[65536];
    if (wav_file) fseek(wav_file, 44, SEEK_SET);

    uint64_t total = (uint64_t)(seconds * MACHINE_CLOCK_HZ);
    uint64_t slice = MACHINE_CLOCK_HZ / 10;
    for (uint64_t done = 0; done < total && !machine_halted(machine); done += slice) {
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
        for (int b = 0; b < power_count; b++) {
            uint64_t at = (uint64_t)(power_times[b] * MACHINE_CLOCK_HZ);
            if (at >= done && at < done + slice) {
                machine_power_button(machine, true);
                machine_run(machine, MACHINE_CLOCK_HZ / 5);
                machine_power_button(machine, false);
            }
        }
        machine_run(machine, slice);
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
    machine_destroy(machine);
    free(rom);
    return 0;
}
