#include "core/optimiser.h"
#include "core/sh3.h"
#include "util/file.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RAM_SIZE      (64u << 20)
#define FLASH_SIZE    (16u << 20)
#define AREA_MASK     0x1FFFFFFFu
#define MODULE_WINDOW 0x20000u
#define RETURN_VA     0x00004000u
#define STACK_TOP     0x03F00000u
#define DATA_VA       0x03000000u
#define DATA_SIZE     0x10000u
#define SOURCE_VA     (DATA_VA + 0x100u)
#define OTHER_VA      (DATA_VA + 0x2000u)
#define TABLE_VA      (DATA_VA + 0x4000u)
#define TARGET_VA     (DATA_VA + 0x8000u)
#define MODULE_VA     (DATA_VA + 0xC000u)
#define EXPORTS_RVA   0xE000u
#define FUNCTIONS_RVA 0xD000u
#define ORDINALS_RVA  0xD400u
#define NAME_LIST_RVA 0xD800u
#define NAMES_RVA     0x1000u
#define NAME_SLOT     16u
#define RUN_CYCLES    5000000u
#define TRIALS        4000
#define CODE_WORDS    24

static uint8_t *ram;
static const uint8_t *rom;
static size_t rom_size;

static bool bus_read(void *context, uint32_t pa, int size, uint32_t *value) {
    (void)context;
    (void)pa;
    (void)size;
    *value = 0;
    return false;
}

static bool bus_write(void *context, uint32_t pa, int size, uint32_t value) {
    (void)context;
    (void)pa;
    (void)size;
    (void)value;
    return false;
}

static uint8_t *bus_fetch_page(void *context, uint32_t pa) {
    (void)context;
    (void)pa;
    return NULL;
}

static uint8_t *memory_map(void *context, uint32_t va, bool write) {
    (void)context;
    (void)write;
    uint32_t pa = va & AREA_MASK;
    return pa < RAM_SIZE ? ram + pa : NULL;
}

static const uint8_t *rom_at(void *context, uint32_t pa, uint32_t length) {
    (void)context;
    return pa < rom_size && length <= rom_size - pa ? rom + pa : NULL;
}

typedef enum { KIND_OTHER, KIND_FILL, KIND_STRCMP, KIND_PURGE, KIND_WIDEN, KIND_RANGE, KIND_WCSLEN, KIND_EXPORT } kind_t;

static kind_t kind_of(native_fn run) {
    if (run == native_fill32) return KIND_FILL;
    if (run == native_strcmp) return KIND_STRCMP;
    if (run == native_return_zero) return KIND_PURGE;
    if (run == native_widen) return KIND_WIDEN;
    if (run == native_range_lookup) return KIND_RANGE;
    if (run == native_wcslen) return KIND_WCSLEN;
    if (run == native_export_lookup) return KIND_EXPORT;
    return KIND_OTHER;
}

static bool load_module_code(const optimiser_hook_t *hook) {
    uint8_t code[CODE_WORDS * 4];
    for (uint32_t i = 0; i < hook->words; i++) {
        for (int b = 0; b < 4; b++) code[i * 4 + (uint32_t)b] = (uint8_t)(hook->code[i] >> (8 * b));
    }
    size_t length = hook->words * 4;
    for (size_t pa = 0; pa + length <= rom_size; pa += 2) {
        if (memcmp(rom + pa, code, length)) continue;
        size_t from = pa > MODULE_WINDOW ? pa - MODULE_WINDOW : 0, to = pa + MODULE_WINDOW < rom_size ? pa + MODULE_WINDOW : rom_size;
        memcpy(ram + (hook->va - (uint32_t)(pa - from)), rom + from, to - from);
        return true;
    }
    return false;
}

static sh3_cpu_t guest, native;

static bool returned(void *context, uint32_t pc) {
    (void)context;
    (void)pc;
    guest.yield = true;
    return true;
}

static void start(sh3_cpu_t *cpu, uint32_t va) {
    memset(cpu, 0, sizeof *cpu);
    cpu->bus = (sh3_bus_t){ NULL, bus_read, bus_write, bus_fetch_page, ram, 0, RAM_SIZE };
    cpu->speed = 1;
    cpu->on_watch = returned;
    sh3_reset(cpu);
    cpu->watch[0] = RETURN_VA;
    cpu->watch_count = 1;
    cpu->pc = va;
    cpu->pr = RETURN_VA;
    cpu->r[15] = STACK_TOP;
}

static uint32_t random_word(void) {
    return (uint32_t)rand() << 16 ^ (uint32_t)rand();
}

static void put16(uint32_t va, uint32_t value) {
    ram[va] = (uint8_t)value;
    ram[va + 1] = (uint8_t)(value >> 8);
}

static void put32(uint32_t va, uint32_t value) {
    put16(va, value);
    put16(va + 2, value >> 16);
}

static void random_string(uint32_t va, int length, int alphabet) {
    for (int i = 0; i < length; i++) ram[va + (uint32_t)i] = (uint8_t)(1 + rand() % alphabet);
    ram[va + (uint32_t)length] = 0;
}

static void prepare(kind_t kind, sh3_cpu_t *cpu, int trial) {
    memset(ram + DATA_VA, 0xA5, DATA_SIZE);
    switch (kind) {
    case KIND_STRCMP: {
        int length = rand() % 40, alphabet = 1 + rand() % 3 * 100;
        uint32_t left = SOURCE_VA + (uint32_t)(trial % 4), right = OTHER_VA + (uint32_t)(rand() % 4);
        random_string(left, length, alphabet);
        memcpy(ram + right, ram + left, (size_t)length + 1);
        if (rand() % 3) random_string(right + (uint32_t)(length ? rand() % (length + 1) : 0), rand() % 10, alphabet);
        cpu->r[4] = left;
        cpu->r[5] = right;
        break;
    }
    case KIND_WCSLEN: {
        int length = rand() % 60;
        for (int i = 0; i < length; i++) put16(SOURCE_VA + (uint32_t)i * 2, 1 + random_word() % 0xFFFF);
        put16(SOURCE_VA + (uint32_t)length * 2, 0);
        cpu->r[4] = SOURCE_VA;
        break;
    }
    case KIND_WIDEN:
        random_string(SOURCE_VA + (uint32_t)(trial % 3), rand() % 50, 255);
        cpu->r[4] = TARGET_VA;
        cpu->r[5] = SOURCE_VA + (uint32_t)(trial % 3);
        cpu->r[6] = (uint32_t)(rand() % 64 - 4);
        break;
    case KIND_RANGE: {
        int count = rand() % 40;
        uint32_t next = random_word() % 200;
        for (int i = 0; i < count; i++) {
            uint32_t first = next + random_word() % 50, last = first + random_word() % 30;
            put16(TABLE_VA + (uint32_t)i * 6, first);
            put16(TABLE_VA + (uint32_t)i * 6 + 2, last);
            put16(TABLE_VA + (uint32_t)i * 6 + 4, random_word());
            next = last + 1;
        }
        cpu->r[4] = TABLE_VA;
        cpu->r[5] = (uint32_t)count;
        cpu->r[6] = trial % 7 ? random_word() % (next + 20) : random_word();
        break;
    }
    case KIND_EXPORT: {
        int count = rand() % 60, functions = count + rand() % 4 - 2, chosen = rand() % (count + 2);
        put32(MODULE_VA + 80, DATA_VA);
        put32(MODULE_VA + 124, trial % 17 ? EXPORTS_RVA : 0);
        put32(MODULE_VA + 128, 40);
        put32(DATA_VA + EXPORTS_RVA + 20, (uint32_t)(functions < 0 ? 0 : functions));
        put32(DATA_VA + EXPORTS_RVA + 24, (uint32_t)count);
        put32(DATA_VA + EXPORTS_RVA + 28, FUNCTIONS_RVA);
        put32(DATA_VA + EXPORTS_RVA + 32, NAME_LIST_RVA);
        put32(DATA_VA + EXPORTS_RVA + 36, ORDINALS_RVA);
        for (int i = 0; i < count; i++) {
            uint32_t name = NAMES_RVA + (uint32_t)i * NAME_SLOT;
            random_string(DATA_VA + name, 1 + rand() % (NAME_SLOT - 2), 3);
            put32(DATA_VA + NAME_LIST_RVA + (uint32_t)i * 4, name);
            put16(DATA_VA + ORDINALS_RVA + (uint32_t)i * 2, (uint32_t)(rand() % 64));
        }
        for (int i = 0; i < 64; i++) put32(DATA_VA + FUNCTIONS_RVA + (uint32_t)i * 4, 0x100u + random_word() % 0xE00u);
        if (chosen < count) memcpy(ram + SOURCE_VA, ram + DATA_VA + NAMES_RVA + (uint32_t)chosen * NAME_SLOT, NAME_SLOT);
        else random_string(SOURCE_VA, 1 + rand() % 12, 3);
        cpu->r[4] = MODULE_VA;
        cpu->r[5] = SOURCE_VA;
        break;
    }
    case KIND_FILL:
        cpu->r[4] = TARGET_VA;
        cpu->r[5] = random_word();
        cpu->r[6] = (uint32_t)(rand() % 1024) * 4;
        break;
    default:
        break;
    }
}

static const char *kind_name(kind_t kind) {
    static const char *names[] = { "other", "fill", "strcmp", "purge", "widen", "range", "wcslen", "export" };
    return names[kind];
}

static bool same_result(const sh3_cpu_t *guest, const sh3_cpu_t *native, const uint8_t *guest_data) {
    if (guest->pc != RETURN_VA || native->pc != RETURN_VA || guest->r[0] != native->r[0]) return false;
    if ((guest->sr ^ native->sr) & ~(SH3_SR_T | SH3_SR_Q | SH3_SR_M)) return false;
    for (int i = 8; i < 16; i++) {
        if (guest->r[i] != native->r[i]) return false;
    }
    return memcmp(guest_data, ram + DATA_VA, DATA_SIZE) == 0;
}

static int check_hook(optimiser_t *optimiser, int index) {
    optimiser_hook_t *hook = &optimiser->hooks[index];
    kind_t kind = kind_of(hook->run);
    if (kind == KIND_OTHER) return 0;
    static uint8_t guest_data[DATA_SIZE], data_before[DATA_SIZE];
    int failures = 0, declined = 0;
    for (int trial = 0; trial < TRIALS; trial++) {
        srand((unsigned)(trial * 7919 + index));
        start(&guest, hook->va);
        prepare(kind, &guest, trial);
        native = guest;
        memcpy(data_before, ram + DATA_VA, DATA_SIZE);
        sh3_run(&guest, RUN_CYCLES);
        memcpy(guest_data, ram + DATA_VA, DATA_SIZE);
        memcpy(ram + DATA_VA, data_before, DATA_SIZE);
        if (!optimiser_call(optimiser, &native, hook->va)) {
            declined++;
            continue;
        }
        if (native.pc != RETURN_VA) {
            sh3_cpu_t *saved = malloc(sizeof guest);
            memcpy(saved, &guest, sizeof guest);
            guest = native;
            sh3_run(&guest, RUN_CYCLES);
            native = guest;
            memcpy(&guest, saved, sizeof guest);
            free(saved);
        }
        if (!same_result(&guest, &native, guest_data) && failures++ < 3) {
            printf("  %s trial %d: guest r0 %08X sr %08X, native r0 %08X sr %08X\n", kind_name(kind), trial, guest.r[0], guest.sr, native.r[0], native.sr);
        }
    }
    printf("%-8s %08X: %d trials, %d declined, %d differ\n", kind_name(kind), hook->va, TRIALS, declined, failures);
    return failures || declined == TRIALS;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: native-check ROM\n");
        return 2;
    }
    uint8_t *image = file_read(argv[1], &rom_size);
    if (!image) {
        fprintf(stderr, "native-check: cannot read %s\n", argv[1]);
        return 1;
    }
    rom = image;
    ram = calloc(1, RAM_SIZE);
    memcpy(ram, rom, rom_size < FLASH_SIZE ? rom_size : FLASH_SIZE);
    put16(RETURN_VA, 0xAFFE);
    put16(RETURN_VA + 2, 0x0009);
    optimiser_t optimiser;
    optimiser_init(&optimiser, rom_at, NULL, (native_memory_t){ NULL, memory_map }, RUN_CYCLES);
    if (!optimiser.hook_count) {
        fprintf(stderr, "native-check: no optimisation profile matches %s\n", argv[1]);
        return 1;
    }
    printf("%s\n", optimiser.profile);
    for (int i = 0; i < optimiser.hook_count; i++) {
        optimiser_hook_t *hook = &optimiser.hooks[i];
        if (hook->state == OPTIMISER_UNCHECKED && !load_module_code(hook)) printf("%s %08X: code not found in the ROM\n", kind_name(kind_of(hook->run)), hook->va);
    }
    int failed = 0;
    for (int i = 0; i < optimiser.hook_count; i++) failed |= check_hook(&optimiser, i);
    free(ram);
    free(image);
    return failed;
}
