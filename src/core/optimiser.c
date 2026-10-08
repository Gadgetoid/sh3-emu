#include "core/optimiser.h"

#include <string.h>

#define AREA_MASK         0x1FFFFFFFu
#define STACK_ARGUMENTS   16u
#define CODE_WORDS_MAX    24
#define POLL_READS        16
#define TICK_COUNTER_PA   0xFFFFFE98u
#define CASIO_LINK_STATUS 0x10000122u

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

static const uint32_t EXPORT_CE2_CODE[] = {
    0x2F962F86u, 0x2FB62FA6u, 0x2FD62FC6u, 0x68434F22u, 0x7FEC1F58u, 0x048EE07Cu,
    0x8B012448u, 0xE000A033u, 0x0D8EE050u, 0x3D4C018Eu, 0xEA0052D8u, 0x1F24321Cu,
    0x5CD7018Eu, 0x3B1C5BD9u, 0x3C1CA001u, 0x59D67A01u,
};

typedef struct {
    uint32_t va;
    const uint32_t *code;
    uint32_t words;
    native_fn run;
    bool in_rom;
    uint32_t next;
} hook_spec_t;

typedef struct {
    const char *name;
    const hook_spec_t *hooks;
    int hook_count;
    const uint32_t *polls;
    int poll_count;
} profile_t;

#define CODE(code) (code), (uint32_t)(sizeof(code) / sizeof((code)[0]))
#define COUNT(table) (int)(sizeof(table) / sizeof((table)[0]))

static const hook_spec_t CASIO_HOOKS[] = {
    { 0x8001F53Cu, CODE(CE1_DECODE_CODE), native_ce1_decode, true, 0 },
    { 0x8001F7DCu, CODE(CE1_ENCODE_CODE), native_ce1_encode, true, 0 },
    { 0x800221ECu, CODE(STRCMP_CODE), native_strcmp, true, 0 },
    { 0x8000E5BCu, CODE(PURGE_CODE), native_return_zero, true, 0 },
    { 0x8001CE58u, CODE(WIDEN_CODE), native_widen, true, 0 },
    { 0x01FECC6Cu, CODE(RANGE_CE1_CODE), native_range_lookup, false, 0 },
    { 0x01FF31BCu, CODE(WCSLEN_CODE), native_wcslen, false, 0 },
};

static const uint32_t CASIO_POLLS[] = { TICK_COUNTER_PA, CASIO_LINK_STATUS };

static const hook_spec_t HP_HOOKS[] = {
    { 0x80027540u, CODE(CE2_DECODE_CODE), native_ce2_decode, true, 0 },
    { 0x800273A4u, CODE(CE2_ENCODE_CODE), native_ce2_encode, true, 0 },
    { 0x8000C39Cu, CODE(FILL_CODE), native_fill32, true, 0 },
    { 0x8002A14Cu, CODE(STRCMP_CODE), native_strcmp, true, 0 },
    { 0x800106A0u, CODE(PURGE_CODE), native_return_zero, true, 0 },
    { 0x01FDE218u, CODE(RANGE_CE2_CODE), native_range_lookup, false, 0 },
    { 0x800145A4u, CODE(EXPORT_CE2_CODE), native_export_lookup, true, 0x8001447Cu },
};

static const uint32_t HP_POLLS[] = { TICK_COUNTER_PA };

static const profile_t PROFILES[] = {
    { "Casio A-51, CE 1.01", CASIO_HOOKS, COUNT(CASIO_HOOKS), CASIO_POLLS, COUNT(CASIO_POLLS) },
    { "HP 320LX, CE 2.0", HP_HOOKS, COUNT(HP_HOOKS), HP_POLLS, COUNT(HP_POLLS) },
};

static bool in_rom(optimiser_rom_fn rom, void *context, const hook_spec_t *spec) {
    const uint8_t *bytes = rom(context, spec->va & AREA_MASK, spec->words * 4);
    return bytes && memcmp(bytes, spec->code, spec->words * 4) == 0;
}

void optimiser_init(optimiser_t *optimiser, optimiser_rom_fn rom, void *rom_context, native_memory_t memory, uint64_t poll_window) {
    *optimiser = (optimiser_t){ .memory = memory, .poll_window = poll_window };
    for (int p = 0; p < COUNT(PROFILES); p++) {
        const profile_t *profile = &PROFILES[p];
        if (!in_rom(rom, rom_context, &profile->hooks[0])) continue;
        optimiser->profile = profile->name;
        for (int i = 0; i < profile->hook_count && optimiser->hook_count < OPTIMISER_HOOKS_MAX; i++) {
            const hook_spec_t *spec = &profile->hooks[i];
            if (spec->in_rom && !in_rom(rom, rom_context, spec)) continue;
            optimiser->hooks[optimiser->hook_count++] = (optimiser_hook_t){ spec->va, spec->code, spec->words, spec->run, spec->next, spec->in_rom ? OPTIMISER_MATCHED : OPTIMISER_UNCHECKED };
        }
        for (int i = 0; i < profile->poll_count && optimiser->poll_count < OPTIMISER_POLLS_MAX; i++) optimiser->polls[optimiser->poll_count++] = (optimiser_poll_t){ profile->polls[i], 0, 0 };
        return;
    }
}

bool optimiser_hooked(const optimiser_t *optimiser, uint32_t pc) {
    for (int i = 0; i < optimiser->hook_count; i++) {
        if (optimiser->hooks[i].va == pc) return true;
    }
    return false;
}

static bool code_in_memory(const optimiser_t *optimiser, const optimiser_hook_t *hook, bool *readable) {
    uint8_t bytes[CODE_WORDS_MAX * 4];
    *readable = hook->words <= CODE_WORDS_MAX && native_read(&optimiser->memory, hook->va, bytes, hook->words * 4);
    return *readable && memcmp(bytes, hook->code, hook->words * 4) == 0;
}

static bool sh3_arguments(const optimiser_t *optimiser, const sh3_cpu_t *cpu, uint32_t *arguments) {
    for (int i = 0; i < 4; i++) arguments[i] = cpu->r[4 + i];
    uint8_t stack[(NATIVE_ARGUMENTS - 4) * 4];
    if (!native_read(&optimiser->memory, cpu->r[15] + STACK_ARGUMENTS, stack, sizeof stack)) return false;
    for (int i = 4; i < NATIVE_ARGUMENTS; i++) {
        const uint8_t *word = stack + (i - 4) * 4;
        arguments[i] = (uint32_t)word[0] | (uint32_t)word[1] << 8 | (uint32_t)word[2] << 16 | (uint32_t)word[3] << 24;
    }
    return true;
}

bool optimiser_call(optimiser_t *optimiser, sh3_cpu_t *cpu, uint32_t pc) {
    for (int i = 0; i < optimiser->hook_count; i++) {
        optimiser_hook_t *hook = &optimiser->hooks[i];
        if (hook->va != pc) continue;
        if (hook->state == OPTIMISER_UNCHECKED) {
            bool readable;
            bool matches = code_in_memory(optimiser, hook, &readable);
            if (!readable) return false;
            hook->state = matches ? OPTIMISER_MATCHED : OPTIMISER_MISMATCHED;
        }
        uint32_t arguments[NATIVE_ARGUMENTS];
        native_result_t result = { 0, false };
        if (hook->state != OPTIMISER_MATCHED || !sh3_arguments(optimiser, cpu, arguments) || !hook->run(&optimiser->memory, arguments, &result)) return false;
        if (result.call_next && !hook->next) return false;
        if (result.call_next) {
            cpu->r[4] = arguments[0];
            cpu->r[5] = result.value;
            cpu->pc = hook->next;
        } else {
            cpu->r[0] = result.value;
            cpu->pc = cpu->pr;
        }
        return true;
    }
    return false;
}

bool optimiser_polled(optimiser_t *optimiser, uint32_t pa, uint64_t cycles) {
    for (int i = 0; i < optimiser->poll_count; i++) {
        optimiser_poll_t *poll = &optimiser->polls[i];
        if (poll->pa != pa) continue;
        uint64_t window = cycles / optimiser->poll_window;
        if (window != poll->window) {
            poll->window = window;
            poll->reads = 0;
            return false;
        }
        if (++poll->reads < POLL_READS) return false;
        poll->reads = 0;
        return true;
    }
    return false;
}
