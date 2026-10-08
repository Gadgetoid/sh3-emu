#include "core/accel.h"

#include <string.h>

#include "core/lz.h"
#include "core/lzw.h"

#define CE1_DECODE_VA 0x8001F53Cu
#define CE1_ENCODE_VA 0x8001F7DCu
#define CE2_DECODE_VA 0x80027540u
#define CE2_ENCODE_VA 0x800273A4u
#define STACK_ARGUMENT 16u
#define FILL_VA       0x8000C39Cu
#define CE1_STRCMP_VA 0x800221ECu
#define CE2_STRCMP_VA 0x8002A14Cu
#define CE1_PURGE_VA  0x8000E5BCu
#define CE2_PURGE_VA  0x800106A0u
#define CE1_WIDEN_VA  0x8001CE58u
#define CE1_RANGE_VA  0x01FECC6Cu
#define CE2_RANGE_VA  0x01FDE218u
#define CE1_WCSLEN_VA 0x01FF31BCu
#define STRING_MAX    0x10000u
#define RANGE_STEPS   32
#define PURGE_END     0xF0002000u
#define FILL_MAX      0x4000000u
#define CE2_DATA_MAX  0x4000u
#define CODEC_MAX     0x2000u
#define PAGE          0x400u
#define ACCEL_CODE_MAX 24

static const uint32_t CE1_DECODE_CODE[] = {
    0x2F862FE6u, 0x2FA62F96u, 0x2FC62FB6u, 0x4F222FD6u, 0x1F591F48u, 0x1F7B6B63u,
    0x7E206EF3u, 0xD0507FDCu, 0xE600DC4Cu, 0xE70055E1u, 0x64E2400Bu, 0x89062BB8u,
    0x64B3D04Bu, 0xE60051E3u, 0x400BE701u, 0xD0466512u, 0x400BD449u, 0x6AB30009u,
    0x611251E3u, 0x21181F15u, 0x62E2D147u, 0x21220829u, 0x52E1D144u, 0x28882122u,
};

static const uint32_t CE1_ENCODE_CODE[] = {
    0x2F862FE6u, 0x2FA62F96u, 0x2FC62FB6u, 0x4F222FD6u, 0x1F591F48u, 0x1F7B1F6Au,
    0x6EF37FF4u, 0xE0017FF0u, 0x59EB80E1u, 0xC5184E1Eu, 0x6B0DE600u, 0x65B3D05Au,
    0x400BE700u, 0x51ED6493u, 0x89072118u, 0xE600D056u, 0x611151EEu, 0xE70154EDu,
    0x651D400Bu, 0xD454D051u, 0x0009400Bu, 0x52EDD154u, 0x2122D84Du, 0x611151EEu,
};

static const uint32_t CE2_DECODE_CODE[] = {
    0x2F862FE6u, 0x2FA62F96u, 0x2FC62FB6u, 0x4F222FD6u, 0x1F591F48u, 0x6A731F6Au,
    0xE0345BFCu, 0x699D09FDu, 0x7E206EF3u, 0xD1357FE0u, 0xE1011F16u, 0x89043910u,
    0x3910E102u, 0xA0478901u, 0x915E0009u, 0x890121B8u, 0x0009A042u, 0xE10352E1u,
    0x89013212u, 0x0009A03Cu, 0x400BD02Cu, 0x680364E2u, 0x481DE1F6u, 0x7802E103u,
};

static const uint32_t CE2_ENCODE_CODE[] = {
    0x2F962F86u, 0x2FB62FA6u, 0x2FD62FC6u, 0x1F474F22u, 0x6B636853u, 0xE02C6C73u,
    0x699D09FDu, 0xE1017FCCu, 0x1A18DA4Fu, 0x3816D14Cu, 0xA01C8B01u, 0x2BB80009u,
    0xE1038912u, 0x89093C12u, 0x64A3E050u, 0xD04A06FEu, 0x6783E5FEu, 0x1F94400Bu,
    0x0009A07Bu, 0x6583D045u, 0x64B3400Bu, 0x0009A001u, 0xE101DC40u, 0x89043910u,
};

static const uint32_t FILL_CODE[] = {
    0x68532F86u, 0x89052668u, 0x74046143u, 0x76FC2182u, 0x0009AFF8u, 0x68F6000Bu,
};

static const uint32_t STRCMP_CODE[] = {
    0x205B6043u, 0x8F38C803u, 0x6246E100u, 0x221C6356u, 0x33208916u, 0x62468B14u,
    0x221C6356u, 0x33208910u, 0x62468B0Eu, 0x221C6356u, 0x3320890Au, 0x62468B08u,
};

static const uint32_t PURGE_CODE[] = {
    0xD10AD009u, 0x0009E240u, 0xE0000023u, 0x11042102u, 0x110C1108u, 0x21027140u,
};

static const uint32_t WIDEN_CODE[] = {
    0x68632F86u, 0xE601A001u, 0x36837601u, 0x6150890Au, 0x89072118u, 0x61106153u,
    0x611C7501u, 0x74026243u, 0x2211AFF2u,
};

static const uint32_t RANGE_CE1_CODE[] = {
    0x2F962F86u, 0x2FB62FA6u, 0x2FD62FC6u, 0x68434F22u, 0x69636B53u, 0x2BB87FF0u,
    0x7BFF892Eu, 0xEA006CB3u, 0x89293AC7u, 0x64A3D01Au, 0x400BE502u, 0x6D0334CCu,
};

static const uint32_t RANGE_CE2_CODE[] = {
    0x2F962F86u, 0x69636843u, 0x892B2558u, 0x675375FFu, 0x3477E400u, 0x66438926u,
    0x4621367Cu, 0x0617E103u,
};

static const uint32_t WCSLEN_CODE[] = {
    0x61536543u, 0x21186111u, 0x75028D02u, 0x0009AFF9u, 0x45213548u, 0x000B75FFu,
};

static void codec_return(sh3_cpu_t *cpu, uint32_t value) {
    cpu->r[0] = value;
    cpu->pc = cpu->pr;
}

static bool code_matches(accel_rom_fn rom, void *context, uint32_t va, const uint32_t *code, size_t words) {
    const uint8_t *bytes = rom(context, va & 0x1FFFFFFFu, (uint32_t)(words * 4));
    return bytes && memcmp(bytes, code, words * 4) == 0;
}

static bool ce1_decode(sh3_cpu_t *cpu, const accel_memory_t *memory);
static bool ce1_encode(sh3_cpu_t *cpu, const accel_memory_t *memory);
static bool ce2_decode(sh3_cpu_t *cpu, const accel_memory_t *memory);
static bool ce2_encode(sh3_cpu_t *cpu, const accel_memory_t *memory);
static bool fill32(sh3_cpu_t *cpu, const accel_memory_t *memory);
static bool string_compare(sh3_cpu_t *cpu, const accel_memory_t *memory);
static bool cache_purge(sh3_cpu_t *cpu, const accel_memory_t *memory);
static bool widen(sh3_cpu_t *cpu, const accel_memory_t *memory);
static bool range_lookup(sh3_cpu_t *cpu, const accel_memory_t *memory);
static bool wide_length(sh3_cpu_t *cpu, const accel_memory_t *memory);

#define WORDS(code) (code), (uint32_t)(sizeof(code) / sizeof((code)[0]))

static void add_rom_hook(accel_hooks_t *hooks, accel_rom_fn rom, void *context, uint32_t va, accel_kind_t kind, const uint32_t *code, uint32_t words,
                         accel_handler_fn run) {
    if (hooks->count < ACCEL_HOOKS_MAX && code_matches(rom, context, va, code, words)) hooks->hooks[hooks->count++] = (accel_hook_t){ va, kind, code, words, run, ACCEL_MATCHED };
}

static void add_module_hook(accel_hooks_t *hooks, uint32_t va, accel_kind_t kind, const uint32_t *code, uint32_t words, accel_handler_fn run) {
    if (hooks->count < ACCEL_HOOKS_MAX) hooks->hooks[hooks->count++] = (accel_hook_t){ va, kind, code, words, run, ACCEL_UNCHECKED };
}

bool accel_find(accel_rom_fn rom, void *context, accel_hooks_t *hooks) {
    *hooks = (accel_hooks_t){ 0 };
    if (code_matches(rom, context, CE1_DECODE_VA, CE1_DECODE_CODE, sizeof CE1_DECODE_CODE / 4) &&
        code_matches(rom, context, CE1_ENCODE_VA, CE1_ENCODE_CODE, sizeof CE1_ENCODE_CODE / 4)) {
        hooks->system = 1;
        add_rom_hook(hooks, rom, context, CE1_DECODE_VA, ACCEL_DECODE, WORDS(CE1_DECODE_CODE), ce1_decode);
        add_rom_hook(hooks, rom, context, CE1_ENCODE_VA, ACCEL_ENCODE, WORDS(CE1_ENCODE_CODE), ce1_encode);
        add_rom_hook(hooks, rom, context, CE1_STRCMP_VA, ACCEL_STRCMP, WORDS(STRCMP_CODE), string_compare);
        add_rom_hook(hooks, rom, context, CE1_PURGE_VA, ACCEL_PURGE, WORDS(PURGE_CODE), cache_purge);
        add_rom_hook(hooks, rom, context, CE1_WIDEN_VA, ACCEL_WIDEN, WORDS(WIDEN_CODE), widen);
        add_module_hook(hooks, CE1_RANGE_VA, ACCEL_RANGE, WORDS(RANGE_CE1_CODE), range_lookup);
        add_module_hook(hooks, CE1_WCSLEN_VA, ACCEL_WCSLEN, WORDS(WCSLEN_CODE), wide_length);
        return true;
    }
    if (code_matches(rom, context, CE2_DECODE_VA, CE2_DECODE_CODE, sizeof CE2_DECODE_CODE / 4) &&
        code_matches(rom, context, CE2_ENCODE_VA, CE2_ENCODE_CODE, sizeof CE2_ENCODE_CODE / 4)) {
        hooks->system = 2;
        add_rom_hook(hooks, rom, context, CE2_DECODE_VA, ACCEL_DECODE, WORDS(CE2_DECODE_CODE), ce2_decode);
        add_rom_hook(hooks, rom, context, CE2_ENCODE_VA, ACCEL_ENCODE, WORDS(CE2_ENCODE_CODE), ce2_encode);
        add_rom_hook(hooks, rom, context, FILL_VA, ACCEL_FILL, WORDS(FILL_CODE), fill32);
        add_rom_hook(hooks, rom, context, CE2_STRCMP_VA, ACCEL_STRCMP, WORDS(STRCMP_CODE), string_compare);
        add_rom_hook(hooks, rom, context, CE2_PURGE_VA, ACCEL_PURGE, WORDS(PURGE_CODE), cache_purge);
        add_module_hook(hooks, CE2_RANGE_VA, ACCEL_RANGE, WORDS(RANGE_CE2_CODE), range_lookup);
        return true;
    }
    return false;
}

static bool guest_read(const accel_memory_t *memory, uint32_t va, uint8_t *data, uint32_t length) {
    while (length) {
        uint32_t chunk = PAGE - (va & (PAGE - 1));
        if (chunk > length) chunk = length;
        const uint8_t *host = memory->map(memory->context, va, false);
        if (!host) return false;
        memcpy(data, host, chunk);
        va += chunk;
        data += chunk;
        length -= chunk;
    }
    return true;
}

static bool guest_writable(const accel_memory_t *memory, uint32_t va, uint32_t length) {
    for (uint32_t at = va & ~(PAGE - 1); at < va + length; at += PAGE) {
        if (!memory->map(memory->context, at < va ? va : at, true)) return false;
    }
    return true;
}

static void guest_write(const accel_memory_t *memory, uint32_t va, const uint8_t *data, uint32_t length) {
    while (length) {
        uint32_t chunk = PAGE - (va & (PAGE - 1));
        if (chunk > length) chunk = length;
        memcpy(memory->map(memory->context, va, true), data, chunk);
        va += chunk;
        data += chunk;
        length -= chunk;
    }
}

static bool guest_word(const accel_memory_t *memory, uint32_t va, uint32_t *value) {
    uint8_t bytes[4];
    if (!guest_read(memory, va, bytes, 4)) return false;
    *value = (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 | (uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
    return true;
}

static bool guest_writable_strided(const accel_memory_t *memory, uint32_t va, uint32_t count, uint32_t stride) {
    if (!count) return true;
    uint32_t last = va + (count - 1) * stride;
    if (stride == 1) return guest_writable(memory, va, count);
    for (uint32_t i = 0; i < count; i++) {
        uint32_t at = va + i * stride;
        if ((i == 0 || (at & ~(PAGE - 1)) != ((at - stride) & ~(PAGE - 1))) && !memory->map(memory->context, at, true)) return false;
    }
    return last >= va;
}

static bool ce1_decode(sh3_cpu_t *cpu, const accel_memory_t *memory) {
    static uint8_t input[CODEC_MAX], output[CODEC_MAX];
    static uint16_t runs[CODEC_MAX];
    uint32_t source = cpu->r[4], length = cpu->r[5], destination = cpu->r[6], capacity_va = cpu->r[7];
    uint32_t skip, stride, capacity;
    if (!guest_word(memory, cpu->r[15] + STACK_ARGUMENT, &skip) || !guest_word(memory, cpu->r[15] + STACK_ARGUMENT + 4, &stride)) return false;
    if (!guest_word(memory, capacity_va, &capacity)) return false;
    if (!destination || !stride || stride > CODEC_MAX || !length || length > CODEC_MAX || !capacity || capacity > CODEC_MAX) return false;
    if (!guest_read(memory, source, input, length)) return false;
    size_t run_count = 0;
    size_t produced = lzw_decode_runs(input, length, output, sizeof output, runs, &run_count, CODEC_MAX);
    if (!produced) return false;
    uint32_t remaining = capacity, skipping = skip, written = 0;
    for (size_t i = 0; i < run_count; i++) {
        if (runs[i] > remaining) return false;
        uint32_t skipped = skipping < runs[i] ? skipping : runs[i];
        skipping -= skipped;
        remaining -= runs[i] - skipped;
        written += runs[i] - skipped;
    }
    if (written > CODEC_MAX || (uint64_t)written * stride > CODEC_MAX * 2u) return false;
    if (!guest_writable_strided(memory, destination, written, stride) || !guest_writable(memory, capacity_va, 4)) return false;
    const uint8_t *from = output + (produced - written);
    if (stride == 1) {
        guest_write(memory, destination, from, written);
    } else {
        for (uint32_t i = 0; i < written; i++) guest_write(memory, destination + i * stride, from + i, 1);
    }
    uint8_t count[4] = { (uint8_t)written, (uint8_t)(written >> 8), (uint8_t)(written >> 16), (uint8_t)(written >> 24) };
    guest_write(memory, capacity_va, count, 4);
    codec_return(cpu, 0);
    return true;
}

static bool ce1_encode(sh3_cpu_t *cpu, const accel_memory_t *memory) {
    static uint8_t input[CODEC_MAX], output[CODEC_MAX];
    uint32_t source = cpu->r[4], length = cpu->r[5] & 0xFFFF, destination = cpu->r[6], capacity_va = cpu->r[7];
    uint32_t stride_word;
    uint8_t capacity_bytes[2];
    if (!guest_word(memory, cpu->r[15] + STACK_ARGUMENT, &stride_word) || !guest_read(memory, capacity_va, capacity_bytes, 2)) return false;
    uint32_t stride = stride_word & 0xFFFF, capacity = (uint32_t)capacity_bytes[0] | (uint32_t)capacity_bytes[1] << 8;
    if (!stride || !length || !capacity) return false;
    uint32_t span = (length - 1) * stride + 1;
    if (span > CODEC_MAX || !guest_read(memory, source, input, span)) return false;
    bool all_zero = true;
    for (uint32_t i = 0; i < length && all_zero; i++) all_zero = input[i * stride] == 0;
    if (all_zero) return false;
    size_t produced = lzw_encode(input, length, stride, output, capacity, true);
    if (!produced || produced >= capacity) return false;
    if ((destination && !guest_writable(memory, destination, (uint32_t)produced)) || !guest_writable(memory, capacity_va, 2)) return false;
    if (destination) guest_write(memory, destination, output, (uint32_t)produced);
    uint8_t written[2] = { (uint8_t)produced, (uint8_t)(produced >> 8) };
    guest_write(memory, capacity_va, written, 2);
    codec_return(cpu, 0);
    return true;
}

static uint32_t read24(const uint8_t *bytes) {
    return (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 | (uint32_t)bytes[2] << 16;
}

static bool ce2_decode(sh3_cpu_t *cpu, const accel_memory_t *memory) {
    static uint8_t data[CE2_DATA_MAX], output[CODEC_MAX], table[3 * (LZ_WINDOW_BLOCKS + 2)];
    uint32_t source = cpu->r[4], length = cpu->r[5], destination = cpu->r[6], capacity = cpu->r[7];
    uint32_t skip, stride;
    uint8_t header[3];
    if (!guest_word(memory, cpu->r[15] + STACK_ARGUMENT, &skip) || !guest_word(memory, cpu->r[15] + STACK_ARGUMENT + 4, &stride)) return false;
    if (stride != 1 || !destination || !capacity || capacity > CODEC_MAX || length < 3) return false;
    if (!guest_read(memory, source, header, 3)) return false;
    uint32_t size = read24(header), blocks = size / 1024 + 1, header_length = 3 * (blocks + 1);
    if (header_length > length || skip >= size || skip % 1024) return false;
    uint32_t count = size - skip < capacity ? size - skip : capacity;
    uint32_t first = skip / 1024, last = (skip + count - 1) / 1024, span = last - first + 1;
    if (span > LZ_WINDOW_BLOCKS) return false;
    if (!guest_read(memory, source + 3 * first, table, 3 * (span + 1))) return false;
    uint32_t starts[LZ_WINDOW_BLOCKS + 1];
    for (uint32_t i = 0; i <= span; i++) starts[i] = first + i ? read24(table + 3 * i) : header_length;
    if (starts[span] < starts[0] || starts[span] > length || starts[span] - starts[0] > sizeof data) return false;
    if (!guest_read(memory, source + starts[0], data, starts[span] - starts[0])) return false;
    if (lz_decode_window(data, starts[0], starts[span] - starts[0], starts, first, span, skip, output, count) != (long)count) return false;
    if (!guest_writable(memory, destination, count)) return false;
    guest_write(memory, destination, output, count);
    codec_return(cpu, count);
    return true;
}

static bool ce2_encode(sh3_cpu_t *cpu, const accel_memory_t *memory) {
    static uint8_t input[CODEC_MAX], output[CODEC_MAX * 2];
    uint32_t source = cpu->r[4], length = cpu->r[5], destination = cpu->r[6], capacity = cpu->r[7];
    uint32_t stride;
    if (!guest_word(memory, cpu->r[15] + STACK_ARGUMENT, &stride)) return false;
    if (stride != 1 || !destination || !length || length > CODEC_MAX) return false;
    if (!guest_read(memory, source, input, length)) return false;
    bool all_zero;
    size_t produced = lz_encode(input, length, output, sizeof output, &all_zero);
    if (all_zero || !produced || produced > capacity) return false;
    if (!guest_writable(memory, destination, (uint32_t)produced)) return false;
    guest_write(memory, destination, output, (uint32_t)produced);
    codec_return(cpu, (uint32_t)produced);
    return true;
}

static bool fill32(sh3_cpu_t *cpu, const accel_memory_t *memory) {
    uint32_t destination = cpu->r[4], value = cpu->r[5], length = cpu->r[6];
    if ((destination & 3) || (length & 3) || length > FILL_MAX) return false;
    if (!guest_writable(memory, destination, length)) return false;
    uint8_t word[4] = { (uint8_t)value, (uint8_t)(value >> 8), (uint8_t)(value >> 16), (uint8_t)(value >> 24) };
    for (uint32_t at = destination; at < destination + length;) {
        uint32_t chunk = PAGE - (at & (PAGE - 1));
        if (chunk > destination + length - at) chunk = destination + length - at;
        uint8_t *host = memory->map(memory->context, at, true);
        for (uint32_t i = 0; i < chunk; i += 4) memcpy(host + i, word, 4);
        at += chunk;
    }
    if (length) cpu->r[1] = destination + length - 4;
    cpu->r[4] = destination + length;
    cpu->r[6] = 0;
    cpu->pc = cpu->pr;
    return true;
}

static bool guest_byte(const accel_memory_t *memory, uint32_t va, uint8_t *value) {
    return guest_read(memory, va, value, 1);
}

static bool string_compare(sh3_cpu_t *cpu, const accel_memory_t *memory) {
    uint32_t left = cpu->r[4], right = cpu->r[5];
    uint8_t a = 0, b = 0;
    uint32_t i = 0;
    for (; i < STRING_MAX; i++) {
        if (!guest_byte(memory, left + i, &a) || !guest_byte(memory, right + i, &b)) return false;
        if (!a || a != b) break;
    }
    if (i == STRING_MAX) return false;
    cpu->r[1] = 0xFF;
    cpu->r[2] = (uint32_t)a - b;
    cpu->r[3] = b;
    cpu->r[4] = left + i + 1;
    cpu->r[5] = right + i + 1;
    codec_return(cpu, (uint32_t)a - b);
    return true;
}

static bool cache_purge(sh3_cpu_t *cpu, const accel_memory_t *memory) {
    (void)memory;
    cpu->r[1] = PURGE_END;
    cpu->r[2] = 0;
    cpu->sr |= SH3_SR_T;
    codec_return(cpu, 0);
    return true;
}

static bool widen(sh3_cpu_t *cpu, const accel_memory_t *memory) {
    static uint8_t text[STRING_MAX];
    uint32_t destination = cpu->r[4], source = cpu->r[5];
    int32_t limit = (int32_t)cpu->r[6];
    uint32_t count = 0;
    while ((int32_t)(count + 1) < limit) {
        if (count == STRING_MAX || !guest_byte(memory, source + count, &text[count])) return false;
        if (!text[count]) break;
        count++;
    }
    if (!guest_writable(memory, destination, (count + 1) * 2)) return false;
    for (uint32_t i = 0; i <= count; i++) {
        uint8_t wide[2] = { i < count ? text[i] : 0, 0 };
        guest_write(memory, destination + i * 2, wide, 2);
    }
    cpu->r[1] = 0;
    if (count) cpu->r[2] = destination + (count - 1) * 2;
    cpu->r[4] = destination + count * 2;
    cpu->r[5] = source + count;
    cpu->r[6] = count + 1;
    cpu->pc = cpu->pr;
    return true;
}

static bool range_lookup(sh3_cpu_t *cpu, const accel_memory_t *memory) {
    uint32_t table = cpu->r[4];
    int32_t character = (int32_t)cpu->r[6], low = 0, high = (int32_t)cpu->r[5] - 1;
    uint32_t result = cpu->r[6];
    if (!cpu->r[5]) high = -1;
    for (int steps = 0; low <= high; steps++) {
        if (steps == RANGE_STEPS) return false;
        int32_t middle = (low + high) / 2;
        uint8_t entry[6];
        if (!guest_read(memory, table + (uint32_t)middle * 6, entry, sizeof entry)) return false;
        int32_t first = entry[0] | entry[1] << 8, last = entry[2] | entry[3] << 8;
        if (character < first) {
            high = middle - 1;
        } else if (character > last) {
            low = middle + 1;
        } else {
            result = (uint16_t)((entry[4] | entry[5] << 8) + (uint32_t)character);
            break;
        }
    }
    codec_return(cpu, result);
    return true;
}

static bool wide_length(sh3_cpu_t *cpu, const accel_memory_t *memory) {
    uint32_t start = cpu->r[4], count = 0;
    for (uint8_t wide[2];; count++) {
        if (count == STRING_MAX || !guest_read(memory, start + count * 2, wide, 2)) return false;
        if (!wide[0] && !wide[1]) break;
    }
    cpu->r[1] = 0;
    cpu->r[5] = count;
    codec_return(cpu, count);
    return true;
}

bool accel_hooked(const accel_hooks_t *hooks, uint32_t pc) {
    for (int i = 0; i < hooks->count; i++) {
        if (hooks->hooks[i].va == pc) return true;
    }
    return false;
}

static bool code_in_memory(const accel_memory_t *memory, const accel_hook_t *hook, bool *readable) {
    uint8_t bytes[ACCEL_CODE_MAX * 4];
    *readable = hook->words <= ACCEL_CODE_MAX && guest_read(memory, hook->va, bytes, hook->words * 4);
    return *readable && memcmp(bytes, hook->code, hook->words * 4) == 0;
}

bool accel_call(accel_hooks_t *hooks, sh3_cpu_t *cpu, const accel_memory_t *memory, uint32_t pc) {
    for (int i = 0; i < hooks->count; i++) {
        accel_hook_t *hook = &hooks->hooks[i];
        if (hook->va != pc) continue;
        if (hook->state == ACCEL_UNCHECKED) {
            bool readable;
            bool matches = code_in_memory(memory, hook, &readable);
            if (!readable) return false;
            hook->state = matches ? ACCEL_MATCHED : ACCEL_MISMATCHED;
        }
        return hook->state == ACCEL_MATCHED && hook->run(cpu, memory);
    }
    return false;
}
