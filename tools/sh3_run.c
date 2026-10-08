#include "core/sh3.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define RAM_SIZE      (64u << 20)
#define STACK_TOP     0x03F00000u
#define LINUX_TRAPA   0x13
#define SYSCALL_EXIT  1
#define SYSCALL_WRITE 4

typedef struct {
    sh3_cpu_t cpu;
    uint8_t  *ram;
    bool exited;
    int exit_code;
    bool trace;
    bool stop_on_exception;
    bool faulted;
    uint32_t fault_code;
    uint32_t fault_pc;
} runner_t;

static bool bus_read(void *context, uint32_t pa, int size, uint32_t *value) {
    runner_t *runner = context;
    if (runner->trace) fprintf(stderr, "read of unmapped %08x (%d)\n", pa, size);
    *value = 0;
    return false;
}

static bool bus_write(void *context, uint32_t pa, int size, uint32_t value) {
    runner_t *runner = context;
    if (runner->trace) fprintf(stderr, "write of unmapped %08x (%d) = %08x\n", pa, size, value);
    return false;
}

static uint8_t *bus_fetch_page(void *context, uint32_t pa) {
    (void)context;
    (void)pa;
    return NULL;
}

static bool on_trapa(void *context, uint32_t number) {
    runner_t *runner = context;
    sh3_cpu_t *cpu = &runner->cpu;
    if (number != LINUX_TRAPA) return false;
    switch (cpu->r[3]) {
    case SYSCALL_EXIT:
        runner->exited = true;
        runner->exit_code = (int)cpu->r[4];
        cpu->yield = true;
        return true;
    case SYSCALL_WRITE: {
        uint32_t address = cpu->r[5], length = cpu->r[6];
        if (address >= RAM_SIZE || length > RAM_SIZE - address) { cpu->r[0] = (uint32_t)-14; return true; }
        fflush(stdout);
        ssize_t written = write((int)cpu->r[4], runner->ram + address, length);
        cpu->r[0] = (uint32_t)written;
        return true;
    }
    default:
        fprintf(stderr, "unsupported syscall %u\n", cpu->r[3]);
        cpu->r[0] = (uint32_t)-38;
        return true;
    }
}

static uint32_t read_le32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint16_t read_le16(const uint8_t *p) {
    return (uint16_t)(p[0] | p[1] << 8);
}

static bool load_elf(runner_t *runner, const uint8_t *file, size_t size, uint32_t *entry) {
    if (size < 52 || memcmp(file, "\177ELF", 4) || file[4] != 1 || file[5] != 1) return false;
    *entry = read_le32(file + 24);
    uint32_t program_headers = read_le32(file + 28);
    uint16_t header_size = read_le16(file + 42), count = read_le16(file + 44);
    for (uint16_t i = 0; i < count; i++) {
        size_t offset = program_headers + (size_t)i * header_size;
        if (offset + 32 > size) return false;
        const uint8_t *header = file + offset;
        if (read_le32(header) != 1) continue;
        uint32_t file_offset = read_le32(header + 4), address = read_le32(header + 8) & 0x1FFFFFFFu;
        uint32_t file_size = read_le32(header + 16), memory_size = read_le32(header + 20);
        if (file_offset + (size_t)file_size > size || file_size > memory_size) return false;
        if (address >= RAM_SIZE || memory_size > RAM_SIZE - address) continue;
        memcpy(runner->ram + address, file + file_offset, file_size);
        memset(runner->ram + address + file_size, 0, memory_size - file_size);
    }
    return true;
}

static bool trace_before(void *context, uint32_t pc) {
    runner_t *runner = context;
    sh3_cpu_t *cpu = &runner->cpu;
    uint32_t pa;
    uint16_t op = 0;
    if (sh3_translate(cpu, pc, false, &pa) && pa < RAM_SIZE) op = (uint16_t)(runner->ram[pa] | runner->ram[pa + 1] << 8);
    fprintf(stderr, "%08x %04x sr=%08x r0=%08x r1=%08x r2=%08x r3=%08x\n", pc, op, cpu->sr, cpu->r[0], cpu->r[1], cpu->r[2], cpu->r[3]);
    return false;
}

static void on_exception(void *context, uint32_t code, uint32_t pc, bool user) {
    runner_t *runner = context;
    (void)user;
    if (runner->faulted) return;
    runner->faulted = true;
    runner->fault_code = code;
    runner->fault_pc = pc;
    runner->cpu.yield = true;
}

static void usage(void) {
    fprintf(stderr, "usage: sh3-run [--privileged] [--trace] [--stop-on-exception] [--cycles=N] PROGRAM.elf\n");
}

static runner_t runner;

int main(int argc, char **argv) {
    const char *path = NULL;
    bool privileged = false;
    uint64_t cycles = 100000000;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--privileged")) privileged = true;
        else if (!strcmp(argv[i], "--trace")) runner.trace = true;
        else if (!strcmp(argv[i], "--stop-on-exception")) runner.stop_on_exception = true;
        else if (!strncmp(argv[i], "--cycles=", 9)) cycles = strtoull(argv[i] + 9, NULL, 0);
        else if (argv[i][0] == '-') { usage(); return 2; }
        else path = argv[i];
    }
    if (!path) { usage(); return 2; }
    FILE *file = fopen(path, "rb");
    if (!file) { perror(path); return 2; }
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    uint8_t *data = malloc((size_t)size);
    if (!data || fread(data, 1, (size_t)size, file) != (size_t)size) { fclose(file); return 2; }
    fclose(file);

    runner.ram = calloc(1, RAM_SIZE);
    runner.cpu.bus = (sh3_bus_t){ &runner, bus_read, bus_write, bus_fetch_page, runner.ram, 0, RAM_SIZE };
    runner.cpu.on_trapa = on_trapa;
    static sh3_debug_t debug;
    debug = (sh3_debug_t){ .context = &runner, .before = trace_before, .every = true };
    if (runner.stop_on_exception) {
        debug.exception = on_exception;
        debug.every = runner.trace;
    }
    if (runner.trace || runner.stop_on_exception) runner.cpu.debug = &debug;
    sh3_reset(&runner.cpu);
    uint32_t entry;
    if (!load_elf(&runner, data, (size_t)size, &entry)) { fprintf(stderr, "%s: not an SH ELF\n", path); return 2; }
    sh3_set_sr(&runner.cpu, privileged ? SH3_SR_MD : 0);
    runner.cpu.pc = entry;
    runner.cpu.r[15] = STACK_TOP;
    while (!runner.exited && !runner.faulted && runner.cpu.cycles < cycles) sh3_run(&runner.cpu, cycles);
    if (runner.faulted) {
        fprintf(stderr, "exception %03x at pc %08x\n", runner.fault_code, runner.fault_pc);
        return 4;
    }
    if (!runner.exited) {
        fprintf(stderr, "did not exit: pc %08x sr %08x expevt %03x\n", runner.cpu.pc, runner.cpu.sr, runner.cpu.expevt);
        return 3;
    }
    return runner.exit_code;
}
