#include "core/gdb.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

#include "core/ce.h"

#define PACKET_MAX         0x4000
#define BREAKPOINT_MAX     64
#define WATCHPOINT_MAX     16
#define FAULT_HISTORY      8
#define RUN_QUANTUM        (MACHINE_CLOCK_HZ / 100)
#define LIBRARY_CHECK_QUANTA 10
#define REGISTER_COUNT     72
#define REGISTER_SR        32
#define REGISTER_LO        33
#define REGISTER_HI        34
#define REGISTER_BADVADDR  35
#define REGISTER_CAUSE     36
#define REGISTER_PC        37
#define KSEG0              0x80000000u
#define ANY_PROCESS        (-1)

#define SIGNAL_INT  2
#define SIGNAL_ILL  4
#define SIGNAL_TRAP 5
#define SIGNAL_FPE  8
#define SIGNAL_BUS  10
#define SIGNAL_SEGV 11

static const char TARGET_XML[] =
    "<?xml version=\"1.0\"?>\n"
    "<!DOCTYPE target SYSTEM \"gdb-target.dtd\">\n"
    "<target version=\"1.0\">\n"
    "<architecture>mips</architecture>\n"
    "<feature name=\"org.gnu.gdb.mips.cpu\">\n"
    "<reg name=\"r0\" bitsize=\"32\" regnum=\"0\"/><reg name=\"r1\" bitsize=\"32\"/><reg name=\"r2\" bitsize=\"32\"/>"
    "<reg name=\"r3\" bitsize=\"32\"/><reg name=\"r4\" bitsize=\"32\"/><reg name=\"r5\" bitsize=\"32\"/><reg name=\"r6\" bitsize=\"32\"/>"
    "<reg name=\"r7\" bitsize=\"32\"/><reg name=\"r8\" bitsize=\"32\"/><reg name=\"r9\" bitsize=\"32\"/><reg name=\"r10\" bitsize=\"32\"/>"
    "<reg name=\"r11\" bitsize=\"32\"/><reg name=\"r12\" bitsize=\"32\"/><reg name=\"r13\" bitsize=\"32\"/><reg name=\"r14\" bitsize=\"32\"/>"
    "<reg name=\"r15\" bitsize=\"32\"/><reg name=\"r16\" bitsize=\"32\"/><reg name=\"r17\" bitsize=\"32\"/><reg name=\"r18\" bitsize=\"32\"/>"
    "<reg name=\"r19\" bitsize=\"32\"/><reg name=\"r20\" bitsize=\"32\"/><reg name=\"r21\" bitsize=\"32\"/><reg name=\"r22\" bitsize=\"32\"/>"
    "<reg name=\"r23\" bitsize=\"32\"/><reg name=\"r24\" bitsize=\"32\"/><reg name=\"r25\" bitsize=\"32\"/><reg name=\"r26\" bitsize=\"32\"/>"
    "<reg name=\"r27\" bitsize=\"32\"/><reg name=\"r28\" bitsize=\"32\"/><reg name=\"r29\" bitsize=\"32\"/><reg name=\"r30\" bitsize=\"32\"/>"
    "<reg name=\"r31\" bitsize=\"32\"/>\n"
    "<reg name=\"lo\" bitsize=\"32\" regnum=\"33\"/><reg name=\"hi\" bitsize=\"32\" regnum=\"34\"/>"
    "<reg name=\"pc\" bitsize=\"32\" regnum=\"37\"/>\n"
    "</feature>\n"
    "<feature name=\"org.gnu.gdb.mips.cp0\">\n"
    "<reg name=\"status\" bitsize=\"32\" regnum=\"32\"/><reg name=\"badvaddr\" bitsize=\"32\" regnum=\"35\"/>"
    "<reg name=\"cause\" bitsize=\"32\" regnum=\"36\"/>\n"
    "</feature>\n"
    "<feature name=\"org.gnu.gdb.mips.fpu\">\n"
    "<reg name=\"f0\" bitsize=\"32\" type=\"ieee_single\" regnum=\"38\"/><reg name=\"f1\" bitsize=\"32\" type=\"ieee_single\"/>"
    "<reg name=\"f2\" bitsize=\"32\" type=\"ieee_single\"/><reg name=\"f3\" bitsize=\"32\" type=\"ieee_single\"/>"
    "<reg name=\"f4\" bitsize=\"32\" type=\"ieee_single\"/><reg name=\"f5\" bitsize=\"32\" type=\"ieee_single\"/>"
    "<reg name=\"f6\" bitsize=\"32\" type=\"ieee_single\"/><reg name=\"f7\" bitsize=\"32\" type=\"ieee_single\"/>"
    "<reg name=\"f8\" bitsize=\"32\" type=\"ieee_single\"/><reg name=\"f9\" bitsize=\"32\" type=\"ieee_single\"/>"
    "<reg name=\"f10\" bitsize=\"32\" type=\"ieee_single\"/><reg name=\"f11\" bitsize=\"32\" type=\"ieee_single\"/>"
    "<reg name=\"f12\" bitsize=\"32\" type=\"ieee_single\"/><reg name=\"f13\" bitsize=\"32\" type=\"ieee_single\"/>"
    "<reg name=\"f14\" bitsize=\"32\" type=\"ieee_single\"/><reg name=\"f15\" bitsize=\"32\" type=\"ieee_single\"/>"
    "<reg name=\"f16\" bitsize=\"32\" type=\"ieee_single\"/><reg name=\"f17\" bitsize=\"32\" type=\"ieee_single\"/>"
    "<reg name=\"f18\" bitsize=\"32\" type=\"ieee_single\"/><reg name=\"f19\" bitsize=\"32\" type=\"ieee_single\"/>"
    "<reg name=\"f20\" bitsize=\"32\" type=\"ieee_single\"/><reg name=\"f21\" bitsize=\"32\" type=\"ieee_single\"/>"
    "<reg name=\"f22\" bitsize=\"32\" type=\"ieee_single\"/><reg name=\"f23\" bitsize=\"32\" type=\"ieee_single\"/>"
    "<reg name=\"f24\" bitsize=\"32\" type=\"ieee_single\"/><reg name=\"f25\" bitsize=\"32\" type=\"ieee_single\"/>"
    "<reg name=\"f26\" bitsize=\"32\" type=\"ieee_single\"/><reg name=\"f27\" bitsize=\"32\" type=\"ieee_single\"/>"
    "<reg name=\"f28\" bitsize=\"32\" type=\"ieee_single\"/><reg name=\"f29\" bitsize=\"32\" type=\"ieee_single\"/>"
    "<reg name=\"f30\" bitsize=\"32\" type=\"ieee_single\"/><reg name=\"f31\" bitsize=\"32\" type=\"ieee_single\"/>\n"
    "<reg name=\"fcsr\" bitsize=\"32\" group=\"float\" regnum=\"70\"/><reg name=\"fir\" bitsize=\"32\" group=\"float\" regnum=\"71\"/>\n"
    "</feature>\n"
    "</target>\n";

typedef enum { STOP_NONE, STOP_SIGNAL, STOP_WATCH, STOP_LIBRARY } stop_kind_t;

typedef struct {
    uint32_t gpr[32];
    uint32_t lo, hi, status, badvaddr, cause, pc;
    uint32_t code;
    int      process;
} fault_t;

typedef struct {
    uint32_t address;
    uint32_t length;
    int      type;
} watchpoint_t;

struct gdb {
    machine_t   *machine;
    ce_t         ce;
    gdb_log_fn   log;
    mips_debug_t debug;
    int          listener;
    int          client;
    bool         no_ack;
    bool         halted;
    bool         killed;
    bool         catch_faults;
    bool         forward_output;
    bool         elf_libraries;
    uint32_t     library_hash;
    int          library_check;
    uint32_t     module_list_pa;
    bool         module_list_known;
    bool         module_added;
    uint32_t     new_module;
    uint32_t     new_module_entry_pa;
    bool         entry_written;
    uint32_t     entry_breaks[BREAKPOINT_MAX];
    int          entry_break_count;

    uint8_t  input[PACKET_MAX * 2];
    size_t   input_length;

    uint32_t     breakpoints[BREAKPOINT_MAX];
    int          breakpoint_count;
    watchpoint_t watchpoints[WATCHPOINT_MAX];
    int          watchpoint_count;

    char process_name[CE_NAME_MAX];
    int  process;
    bool waiting_for_process;
    int  last_user_process;

    bool     stepping;
    bool     step_executed;
    uint32_t step_pc;
    int      step_process;
    bool     step_user;

    bool     resume_skip;
    bool     watch_skip;
    uint32_t resume_pc;
    int      resume_process;

    stop_kind_t stop_kind;
    int         stop_signal;
    uint32_t    stop_address;
    int         stop_watch_type;

    fault_t faults[FAULT_HISTORY];
    int     fault_next;
    bool    post_mortem;
    fault_t post_mortem_fault;
};

static void logf_gdb(gdb_t *gdb, const char *format, ...) {
    if (!gdb->log) return;
    char message[512];
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(message, sizeof message, format, arguments);
    va_end(arguments);
    gdb->log(message);
}

static int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static const char HEX[] = "0123456789abcdef";

static void hex_bytes(char *out, const uint8_t *data, size_t length) {
    for (size_t i = 0; i < length; i++) {
        out[i * 2] = HEX[data[i] >> 4];
        out[i * 2 + 1] = HEX[data[i] & 15];
    }
    out[length * 2] = 0;
}

static size_t unhex_bytes(uint8_t *out, const char *text, size_t max) {
    size_t length = 0;
    while (length < max && hex_value(text[0]) >= 0 && hex_value(text[1]) >= 0) {
        out[length++] = (uint8_t)(hex_value(text[0]) << 4 | hex_value(text[1]));
        text += 2;
    }
    return length;
}

static uint32_t parse_hex(const char **text) {
    uint32_t value = 0;
    while (hex_value(**text) >= 0) {
        value = value << 4 | (uint32_t)hex_value(**text);
        (*text)++;
    }
    return value;
}

static void hex_word(char *out, uint32_t value) {
    uint8_t bytes[4] = { (uint8_t)value, (uint8_t)(value >> 8), (uint8_t)(value >> 16), (uint8_t)(value >> 24) };
    hex_bytes(out, bytes, 4);
}

static uint32_t unhex_word(const char *text) {
    uint8_t bytes[4] = { 0 };
    unhex_bytes(bytes, text, 4);
    return (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 | (uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
}

static void close_client(gdb_t *gdb) {
    if (gdb->client >= 0) close(gdb->client);
    gdb->client = -1;
    gdb->input_length = 0;
}

static bool send_all(gdb_t *gdb, const char *data, size_t length) {
    while (length) {
        ssize_t sent = send(gdb->client, data, length, 0);
        if (sent < 0 && errno == EINTR) continue;
        if (sent <= 0) {
            close_client(gdb);
            return false;
        }
        data += sent;
        length -= (size_t)sent;
    }
    return true;
}

static bool send_packet(gdb_t *gdb, const char *payload) {
    if (gdb->client < 0) return false;
    static char frame[PACKET_MAX * 2 + 8];
    size_t length = 0;
    uint8_t checksum = 0;
    frame[length++] = '$';
    for (const char *c = payload; *c && length < sizeof frame - 4; c++) {
        frame[length++] = *c;
        checksum += (uint8_t)*c;
    }
    frame[length++] = '#';
    frame[length++] = HEX[checksum >> 4];
    frame[length++] = HEX[checksum & 15];
    return send_all(gdb, frame, length);
}

static void send_console(gdb_t *gdb, const char *text) {
    char payload[PACKET_MAX];
    size_t length = strlen(text);
    if (length > (sizeof payload - 2) / 2) length = (sizeof payload - 2) / 2;
    payload[0] = 'O';
    hex_bytes(payload + 1, (const uint8_t *)text, length);
    send_packet(gdb, payload);
}

static int fault_signal(uint32_t code) {
    switch (code) {
    case MIPS_EXC_MOD:
    case MIPS_EXC_TLBL:
    case MIPS_EXC_TLBS:
    case MIPS_EXC_ADEL:
    case MIPS_EXC_ADES: return SIGNAL_SEGV;
    case MIPS_EXC_IBE:
    case MIPS_EXC_DBE: return SIGNAL_BUS;
    case MIPS_EXC_RI:
    case MIPS_EXC_CPU: return SIGNAL_ILL;
    case MIPS_EXC_OV: return SIGNAL_FPE;
    default: return SIGNAL_TRAP;
    }
}

static int current_process(gdb_t *gdb) {
    return ce_current_process(&gdb->ce);
}

static int memory_process(gdb_t *gdb) {
    if (gdb->post_mortem) return gdb->post_mortem_fault.process;
    return gdb->process >= 0 ? gdb->process : CE_CURRENT;
}

static bool split_address(uint32_t va, int process, int *owner, uint32_t *offset) {
    if (va >= KSEG0) return false;
    if (va < MIPS_SLOT_SIZE) {
        *owner = process;
        *offset = va;
    } else {
        *owner = (int)(va / MIPS_SLOT_SIZE) - 1;
        *offset = va & (MIPS_SLOT_SIZE - 1);
    }
    return true;
}

static bool address_matches(gdb_t *gdb, uint32_t target, uint32_t va, int process) {
    int target_owner, owner;
    uint32_t target_offset, offset;
    if (!split_address(va, process, &owner, &offset) || !split_address(target, ANY_PROCESS, &target_owner, &target_offset)) return target == va;
    if (offset != target_offset) return false;
    if (target_owner != ANY_PROCESS) return target_owner == owner;
    return gdb->process < 0 || owner == gdb->process;
}

static uint32_t current_library_hash(gdb_t *gdb);

static void update_debug(gdb_t *gdb) {
    memset(gdb->debug.filter, 0, sizeof gdb->debug.filter);
    for (int i = 0; i < gdb->breakpoint_count; i++) mips_debug_filter_add(&gdb->debug, gdb->breakpoints[i]);
    for (int i = 0; i < gdb->entry_break_count; i++) mips_debug_filter_add(&gdb->debug, gdb->entry_breaks[i]);
    gdb->debug.every = gdb->stepping || gdb->resume_skip || gdb->waiting_for_process || gdb->module_added || gdb->entry_written;
    gdb->debug.data = gdb->watchpoint_count > 0 || (gdb->client >= 0 && gdb->module_list_known);
}

static void request_stop(gdb_t *gdb, stop_kind_t kind, int signal) {
    gdb->stop_kind = kind;
    gdb->stop_signal = signal;
    gdb->debug.stop = true;
}

static bool on_before(void *context, uint32_t pc) {
    gdb_t *gdb = context;
    mips_cpu_t *cpu = machine_cpu(gdb->machine);
    int process = current_process(gdb);
    bool user = mips_user_mode(cpu);
    if (gdb->resume_skip && pc == gdb->resume_pc && process == gdb->resume_process) {
        gdb->resume_skip = false;
        update_debug(gdb);
        if (!gdb->stepping) return false;
    }
    if (gdb->module_added) {
        gdb->module_added = false;
        if (ce_first_module(&gdb->ce, &gdb->new_module)) gdb->new_module_entry_pa = ce_module_entry_field(&gdb->ce, gdb->new_module);
        else gdb->new_module = 0;
        update_debug(gdb);
    }
    if (gdb->entry_written) {
        gdb->entry_written = false;
        uint32_t entry;
        if (gdb->new_module && ce_module_entry(&gdb->ce, gdb->new_module, &entry)) {
            if (gdb->entry_break_count == BREAKPOINT_MAX) memmove(gdb->entry_breaks, gdb->entry_breaks + 1, sizeof gdb->entry_breaks - sizeof gdb->entry_breaks[0]);
            else gdb->entry_break_count++;
            gdb->entry_breaks[gdb->entry_break_count - 1] = entry;
            gdb->new_module = 0;
    gdb->entry_written = false;
        }
        update_debug(gdb);
    }
    for (int i = 0; i < gdb->entry_break_count; i++) {
        if (!address_matches(gdb, gdb->entry_breaks[i], pc, process)) continue;
        gdb->entry_breaks[i] = gdb->entry_breaks[--gdb->entry_break_count];
        update_debug(gdb);
        if (current_library_hash(gdb) == gdb->library_hash) break;
        gdb->stop_kind = STOP_LIBRARY;
        return true;
    }
    if (gdb->waiting_for_process && user && process != gdb->last_user_process) {
        gdb->last_user_process = process;
        char name[CE_NAME_MAX];
        if (ce_process_name(&gdb->ce, process, name, sizeof name) && !strcasecmp(name, gdb->process_name)) {
            gdb->process = process;
            gdb->waiting_for_process = false;
            update_debug(gdb);
            gdb->stop_kind = STOP_SIGNAL;
            gdb->stop_signal = SIGNAL_TRAP;
            logf_gdb(gdb, "gdb: %s started as process %d\n", name, process);
            return true;
        }
    }
    if (gdb->stepping) {
        if (!gdb->step_executed) {
            if (pc == gdb->step_pc && process == gdb->step_process) gdb->step_executed = true;
            return false;
        }
        if (user == gdb->step_user && (!user || process == gdb->step_process)) {
            gdb->stop_kind = STOP_SIGNAL;
            gdb->stop_signal = SIGNAL_TRAP;
            return true;
        }
    }
    for (int i = 0; i < gdb->breakpoint_count; i++) {
        if (!address_matches(gdb, gdb->breakpoints[i], pc, process)) continue;
        gdb->stop_kind = STOP_SIGNAL;
        gdb->stop_signal = SIGNAL_TRAP;
        return true;
    }
    return false;
}

static bool on_access(void *context, uint32_t va, int size, bool write) {
    gdb_t *gdb = context;
    uint32_t pa;
    if (write && gdb->module_list_known && (va & 0xFFFu) == (gdb->module_list_pa & 0xFFFu) &&
        ce_translate(&gdb->ce, va, CE_CURRENT, false, &pa) && pa == gdb->module_list_pa) {
        gdb->module_added = true;
        gdb->debug.every = true;
    }
    if (write && gdb->new_module && va >= KSEG0 && va < 0xC0000000u && (va & 0x1FFFFFFFu) == gdb->new_module_entry_pa) {
        gdb->entry_written = true;
        gdb->debug.every = true;
    }
    if (!gdb->watchpoint_count) return false;
    int process = current_process(gdb);
    if (gdb->watch_skip) {
        if (gdb->debug.pc == gdb->resume_pc && process == gdb->resume_process) {
            gdb->watch_skip = false;
            return false;
        }
    }
    for (int i = 0; i < gdb->watchpoint_count; i++) {
        const watchpoint_t *watch = &gdb->watchpoints[i];
        if (watch->type == 2 && !write) continue;
        if (watch->type == 3 && write) continue;
        for (int byte = 0; byte < size; byte++) {
            uint32_t address = va + (uint32_t)byte;
            bool hit = false;
            for (uint32_t offset = 0; offset < watch->length && !hit; offset++) hit = address_matches(gdb, watch->address + offset, address, process);
            if (!hit) continue;
            gdb->stop_address = watch->address;
            gdb->stop_watch_type = watch->type;
            gdb->stop_kind = STOP_WATCH;
            gdb->stop_signal = SIGNAL_TRAP;
            return true;
        }
    }
    return false;
}

static void on_exception(void *context, uint32_t code, uint32_t pc, bool user) {
    gdb_t *gdb = context;
    if (!user) return;
    mips_cpu_t *cpu = machine_cpu(gdb->machine);
    fault_t *fault = &gdb->faults[gdb->fault_next];
    gdb->fault_next = (gdb->fault_next + 1) % FAULT_HISTORY;
    memcpy(fault->gpr, cpu->gpr, sizeof fault->gpr);
    fault->lo = cpu->lo;
    fault->hi = cpu->hi;
    fault->status = cpu->cp0[CP0_STATUS];
    fault->badvaddr = cpu->cp0[CP0_BADVADDR];
    fault->cause = (cpu->cp0[CP0_CAUSE] & ~0x7Cu) | code << 2;
    fault->pc = pc;
    fault->code = code;
    fault->process = current_process(gdb);
}

void gdb_debug_line(gdb_t *gdb, const char *line) {
    if (gdb->client < 0) return;
    if (gdb->forward_output && !gdb->halted) {
        char text[600];
        snprintf(text, sizeof text, "%s\n", line);
        send_console(gdb, text);
    }
    unsigned code;
    if (!gdb->catch_faults || sscanf(line, "Exception %u", &code) != 1) return;
    for (int back = 1; back <= FAULT_HISTORY; back++) {
        const fault_t *fault = &gdb->faults[(gdb->fault_next + FAULT_HISTORY - back) % FAULT_HISTORY];
        if (fault->code != code || (!fault->pc && !fault->badvaddr)) continue;
        if (gdb->process >= 0 && fault->process != gdb->process) return;
        gdb->post_mortem_fault = *fault;
        gdb->post_mortem = true;
        gdb->faults[(gdb->fault_next + FAULT_HISTORY - back) % FAULT_HISTORY] = (fault_t){ 0 };
        request_stop(gdb, STOP_SIGNAL, fault_signal(code));
        return;
    }
}

static uint32_t read_register(gdb_t *gdb, int number) {
    mips_cpu_t *cpu = machine_cpu(gdb->machine);
    if (gdb->post_mortem) {
        const fault_t *fault = &gdb->post_mortem_fault;
        if (number < 32) return fault->gpr[number];
        switch (number) {
        case REGISTER_SR: return fault->status;
        case REGISTER_LO: return fault->lo;
        case REGISTER_HI: return fault->hi;
        case REGISTER_BADVADDR: return fault->badvaddr;
        case REGISTER_CAUSE: return fault->cause;
        case REGISTER_PC: return fault->pc;
        default: return 0;
        }
    }
    if (number < 32) return cpu->gpr[number];
    switch (number) {
    case REGISTER_SR: return cpu->cp0[CP0_STATUS];
    case REGISTER_LO: return cpu->lo;
    case REGISTER_HI: return cpu->hi;
    case REGISTER_BADVADDR: return cpu->cp0[CP0_BADVADDR];
    case REGISTER_CAUSE: return cpu->cp0[CP0_CAUSE];
    case REGISTER_PC: return cpu->pc;
    default: return 0;
    }
}

static void write_register(gdb_t *gdb, int number, uint32_t value) {
    mips_cpu_t *cpu = machine_cpu(gdb->machine);
    if (gdb->post_mortem) return;
    if (number > 0 && number < 32) cpu->gpr[number] = value;
    else if (number == REGISTER_LO) cpu->lo = value;
    else if (number == REGISTER_HI) cpu->hi = value;
    else if (number == REGISTER_PC && value != cpu->pc) {
        cpu->pc = value;
        cpu->next_pc = value + 4;
        cpu->next_in_delay_slot = false;
        mips_flush_translations(cpu);
    }
}

static void monitor_reply(gdb_t *gdb, const char *format, ...) {
    char text[1024];
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(text, sizeof text, format, arguments);
    va_end(arguments);
    send_console(gdb, text);
}

static int library_process(gdb_t *gdb) {
    return gdb->process;
}

static void library_file_name(gdb_t *gdb, const char *name, char *file, size_t size) {
    snprintf(file, size, "%s", name);
    char *extension = strrchr(file, '.');
    if (gdb->elf_libraries && extension && !strcasecmp(extension, ".dll")) snprintf(extension, size - (size_t)(extension - file), ".elf");
}

static size_t library_list(gdb_t *gdb, char *xml, size_t size) {
    static ce_module_t modules[CE_MODULE_MAX];
    int count = ce_modules(&gdb->ce, modules, CE_MODULE_MAX), process = library_process(gdb);
    size_t length = (size_t)snprintf(xml, size, "<library-list>\n");
    for (int i = 0; i < count && length < size; i++) {
        if (process >= 0 && process < CE_PROCESS_MAX && !(modules[i].in_use >> process & 1)) continue;
        char file[CE_NAME_MAX + 8];
        library_file_name(gdb, modules[i].name, file, sizeof file);
        length += (size_t)snprintf(xml + length, size - length, "<library name=\"%s\"><segment address=\"0x%08x\"/></library>\n", file,
                                   modules[i].base);
    }
    if (length < size) length += (size_t)snprintf(xml + length, size - length, "</library-list>\n");
    return length < size ? length : size - 1;
}

static uint32_t current_library_hash(gdb_t *gdb) {
    static char xml[CE_MODULE_MAX * 128 + 64];
    size_t length = library_list(gdb, xml, sizeof xml);
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < length; i++) hash = (hash ^ (uint8_t)xml[i]) * 16777619u;
    return hash;
}

static void send_stop_reply(gdb_t *gdb) {
    char reply[96];
    int signal = gdb->stop_signal ? gdb->stop_signal : SIGNAL_TRAP;
    if (gdb->stop_kind == STOP_LIBRARY) {
        gdb->library_hash = current_library_hash(gdb);
        snprintf(reply, sizeof reply, "T%02xlibrary:;", SIGNAL_TRAP);
    } else if (gdb->stop_kind == STOP_WATCH) {
        const char *kind = gdb->stop_watch_type == 3 ? "rwatch" : gdb->stop_watch_type == 4 ? "awatch" : "watch";
        snprintf(reply, sizeof reply, "T%02x%s:%08x;", signal, kind, gdb->stop_address);
    } else {
        snprintf(reply, sizeof reply, "T%02x", signal);
    }
    send_packet(gdb, reply);
}

static void monitor_modules(gdb_t *gdb) {
    static ce_module_t modules[CE_MODULE_MAX];
    int count = ce_modules(&gdb->ce, modules, CE_MODULE_MAX), process = library_process(gdb);
    if (!count) {
        monitor_reply(gdb, "CE's module list isn't set up yet\n");
        return;
    }
    for (int i = 0; i < count; i++) {
        bool used = process >= 0 && process < CE_PROCESS_MAX && (modules[i].in_use >> process & 1);
        char file[CE_NAME_MAX + 8];
        library_file_name(gdb, modules[i].name, file, sizeof file);
        monitor_reply(gdb, "%c %08x  %-16s add-symbol-file %s -o 0x%x\n", used ? '*' : ' ', modules[i].base, modules[i].name, file,
                      modules[i].base - 0x10000u);
    }
}

static void halt(gdb_t *gdb) {
    gdb->halted = true;
    gdb->stepping = false;
    gdb->resume_skip = false;
    update_debug(gdb);
    if (gdb->post_mortem) {
        char name[CE_NAME_MAX] = "";
        ce_process_name(&gdb->ce, gdb->post_mortem_fault.process, name, sizeof name);
        char text[200];
        snprintf(text, sizeof text, "velo: exception %u in %s at %08x, badvaddr %08x\n", gdb->post_mortem_fault.code, name[0] ? name : "a process",
                 gdb->post_mortem_fault.pc, gdb->post_mortem_fault.badvaddr);
        send_console(gdb, text);
    }
    send_stop_reply(gdb);
}

static void resume(gdb_t *gdb, bool step) {
    mips_cpu_t *cpu = machine_cpu(gdb->machine);
    gdb->watch_skip = gdb->stop_kind == STOP_WATCH;
    gdb->halted = false;
    gdb->post_mortem = false;
    gdb->stop_kind = STOP_NONE;
    gdb->stop_signal = 0;
    gdb->debug.stop = false;
    gdb->resume_pc = cpu->pc;
    gdb->resume_process = current_process(gdb);
    gdb->resume_skip = false;
    for (int i = 0; i < gdb->breakpoint_count && !gdb->resume_skip; i++)
        gdb->resume_skip = address_matches(gdb, gdb->breakpoints[i], cpu->pc, gdb->resume_process);
    gdb->stepping = step;
    if (step) {
        gdb->step_executed = false;
        gdb->step_pc = cpu->pc;
        gdb->step_process = gdb->resume_process;
        gdb->step_user = mips_user_mode(cpu);
    }
    update_debug(gdb);
}

static bool add_breakpoint(gdb_t *gdb, uint32_t address) {
    for (int i = 0; i < gdb->breakpoint_count; i++) {
        if (gdb->breakpoints[i] == address) return true;
    }
    if (gdb->breakpoint_count == BREAKPOINT_MAX) return false;
    gdb->breakpoints[gdb->breakpoint_count++] = address;
    return true;
}

static void remove_breakpoint(gdb_t *gdb, uint32_t address) {
    for (int i = 0; i < gdb->breakpoint_count; i++) {
        if (gdb->breakpoints[i] != address) continue;
        gdb->breakpoints[i] = gdb->breakpoints[--gdb->breakpoint_count];
        return;
    }
}

static bool add_watchpoint(gdb_t *gdb, uint32_t address, uint32_t length, int type) {
    if (gdb->watchpoint_count == WATCHPOINT_MAX || !length) return false;
    gdb->watchpoints[gdb->watchpoint_count++] = (watchpoint_t){ address, length, type };
    return true;
}

static void remove_watchpoint(gdb_t *gdb, uint32_t address, uint32_t length, int type) {
    for (int i = 0; i < gdb->watchpoint_count; i++) {
        const watchpoint_t *watch = &gdb->watchpoints[i];
        if (watch->address != address || watch->length != length || watch->type != type) continue;
        gdb->watchpoints[i] = gdb->watchpoints[--gdb->watchpoint_count];
        return;
    }
}

static void handle_breakpoint_packet(gdb_t *gdb, const char *packet) {
    bool insert = packet[0] == 'Z';
    const char *cursor = packet + 1;
    int type = (int)parse_hex(&cursor);
    if (*cursor++ != ',') { send_packet(gdb, "E01"); return; }
    uint32_t address = parse_hex(&cursor);
    if (*cursor++ != ',') { send_packet(gdb, "E01"); return; }
    uint32_t kind = parse_hex(&cursor);
    bool ok = true;
    if (type == 0 || type == 1) {
        if (insert) ok = add_breakpoint(gdb, address);
        else remove_breakpoint(gdb, address);
    } else if (type >= 2 && type <= 4) {
        if (insert) ok = add_watchpoint(gdb, address, kind, type);
        else remove_watchpoint(gdb, address, kind, type);
    } else {
        send_packet(gdb, "");
        return;
    }
    update_debug(gdb);
    send_packet(gdb, ok ? "OK" : "E0e");
}

static void handle_read_memory(gdb_t *gdb, const char *packet) {
    const char *cursor = packet + 1;
    uint32_t address = parse_hex(&cursor);
    if (*cursor++ != ',') { send_packet(gdb, "E01"); return; }
    uint32_t length = parse_hex(&cursor);
    if (length > PACKET_MAX / 2 - 8) length = PACKET_MAX / 2 - 8;
    static uint8_t data[PACKET_MAX];
    uint32_t done = 0;
    while (done < length && ce_read(&gdb->ce, address + done, memory_process(gdb), data + done, 1)) done++;
    if (!done && length) { send_packet(gdb, "E14"); return; }
    static char reply[PACKET_MAX + 1];
    hex_bytes(reply, data, done);
    send_packet(gdb, reply);
}

static void handle_write_memory(gdb_t *gdb, const char *packet) {
    const char *cursor = packet + 1;
    uint32_t address = parse_hex(&cursor);
    if (*cursor++ != ',') { send_packet(gdb, "E01"); return; }
    uint32_t length = parse_hex(&cursor);
    if (*cursor++ != ':') { send_packet(gdb, "E01"); return; }
    static uint8_t data[PACKET_MAX];
    if (length > sizeof data || unhex_bytes(data, cursor, length) != length) { send_packet(gdb, "E01"); return; }
    send_packet(gdb, ce_write(&gdb->ce, address, memory_process(gdb), data, length) ? "OK" : "E14");
}

static void handle_read_registers(gdb_t *gdb) {
    char reply[REGISTER_COUNT * 8 + 1];
    for (int i = 0; i < REGISTER_COUNT; i++) hex_word(reply + i * 8, read_register(gdb, i));
    send_packet(gdb, reply);
}

static void handle_write_registers(gdb_t *gdb, const char *packet) {
    const char *values = packet + 1;
    for (int i = 0; i < REGISTER_COUNT && strlen(values) >= 8; i++, values += 8) write_register(gdb, i, unhex_word(values));
    send_packet(gdb, "OK");
}

static void handle_xfer(gdb_t *gdb, const char *packet, const char *object, const char *document, size_t document_length) {
    char prefix[64];
    snprintf(prefix, sizeof prefix, "qXfer:%s:read:", object);
    const char *cursor = packet + strlen(prefix);
    while (*cursor && *cursor != ':') cursor++;
    if (*cursor++ != ':') { send_packet(gdb, "E01"); return; }
    uint32_t offset = parse_hex(&cursor);
    if (*cursor++ != ',') { send_packet(gdb, "E01"); return; }
    uint32_t length = parse_hex(&cursor);
    if (length > PACKET_MAX / 2) length = PACKET_MAX / 2;
    static char reply[PACKET_MAX + 2];
    if (offset >= document_length) {
        send_packet(gdb, "l");
        return;
    }
    size_t available = document_length - offset, take = available < length ? available : length, used = 0;
    reply[used++] = take == available ? 'l' : 'm';
    for (size_t i = 0; i < take && used < sizeof reply - 3; i++) {
        char c = document[offset + i];
        if (c == '#' || c == '$' || c == '}' || c == '*') {
            reply[used++] = '}';
            reply[used++] = (char)(c ^ 0x20);
        } else {
            reply[used++] = c;
        }
    }
    reply[used] = 0;
    send_packet(gdb, reply);
}


static void monitor_processes(gdb_t *gdb) {
    if (!ce_ready(&gdb->ce)) {
        monitor_reply(gdb, "CE's process table isn't set up yet\n");
        return;
    }
    int current = current_process(gdb);
    for (int process = 0; process < CE_PROCESS_MAX; process++) {
        char name[CE_NAME_MAX];
        if (!ce_process_name(&gdb->ce, process, name, sizeof name)) continue;
        monitor_reply(gdb, "%c%c %2d  %08x  %s\n", process == current ? '*' : ' ', process == gdb->process ? '>' : ' ', process,
                      (uint32_t)(process + 1) * MIPS_SLOT_SIZE, name);
    }
}

static void handle_monitor(gdb_t *gdb, const char *packet) {
    char command[256];
    size_t length = unhex_bytes((uint8_t *)command, packet + strlen("qRcmd,"), sizeof command - 1);
    command[length] = 0;
    char *argument = strchr(command, ' ');
    if (argument) {
        *argument++ = 0;
        while (*argument == ' ') argument++;
    }
    if (!strcmp(command, "processes") || !strcmp(command, "ps")) {
        monitor_processes(gdb);
    } else if (!strcmp(command, "process")) {
        if (!argument || !*argument) {
            if (gdb->process >= 0) monitor_reply(gdb, "debugging %s (process %d)\n", gdb->process_name, gdb->process);
            else if (gdb->waiting_for_process) monitor_reply(gdb, "waiting for %s to start\n", gdb->process_name);
            else monitor_reply(gdb, "breakpoints below 0x02000000 match any process\n");
        } else if (!strcmp(argument, "any")) {
            gdb_set_process(gdb, NULL);
            monitor_reply(gdb, "breakpoints below 0x02000000 now match any process\n");
        } else if (gdb_set_process(gdb, argument)) {
            monitor_reply(gdb, "debugging %s (process %d)\n", gdb->process_name, gdb->process);
        } else {
            monitor_reply(gdb, "%s isn't running; stopping when it starts\n", gdb->process_name);
        }
    } else if (!strcmp(command, "modules")) {
        monitor_modules(gdb);
    } else if (!strcmp(command, "libraries")) {
        if (argument && !strcmp(argument, "dll")) gdb->elf_libraries = false;
        else if (argument && !strcmp(argument, "elf")) gdb->elf_libraries = true;
        monitor_reply(gdb, "DLLs are reported to GDB as %s files\n", gdb->elf_libraries ? ".elf" : ".dll");
    } else if (!strcmp(command, "catch")) {
        if (argument && !strcmp(argument, "off")) gdb->catch_faults = false;
        else if (argument && !strcmp(argument, "on")) gdb->catch_faults = true;
        monitor_reply(gdb, "stopping on crashes: %s\n", gdb->catch_faults ? "on" : "off");
    } else if (!strcmp(command, "output")) {
        if (argument && !strcmp(argument, "off")) gdb->forward_output = false;
        else if (argument && !strcmp(argument, "on")) gdb->forward_output = true;
        monitor_reply(gdb, "CE debug output: %s\n", gdb->forward_output ? "on" : "off");
    } else {
        monitor_reply(gdb, "velo-emu monitor commands:\n"
                           "  processes          list CE's processes (* current, > debugged)\n"
                           "  process [NAME|any] debug one process; stops when it starts if it isn't running\n"
                           "  modules            list loaded modules (* used by the debugged process)\n"
                           "  libraries elf|dll  name DLLs to GDB by their .elf (default) or .dll file\n"
                           "  catch on|off       stop when CE reports a crash (default on)\n"
                           "  output on|off      show CE's debug output while running (default on)\n");
    }
    send_packet(gdb, "OK");
}

static void handle_vcont(gdb_t *gdb, const char *packet) {
    if (!strcmp(packet, "vCont?")) {
        send_packet(gdb, "vCont;c;C;s;S");
        return;
    }
    const char *action = packet + strlen("vCont;");
    resume(gdb, action[0] == 's' || action[0] == 'S');
}

static void handle_packet(gdb_t *gdb, const char *packet) {
    switch (packet[0]) {
    case '?':
        send_stop_reply(gdb);
        return;
    case 'g':
        handle_read_registers(gdb);
        return;
    case 'G':
        handle_write_registers(gdb, packet);
        return;
    case 'p': {
        const char *cursor = packet + 1;
        int number = (int)parse_hex(&cursor);
        char reply[9];
        hex_word(reply, read_register(gdb, number));
        send_packet(gdb, number < REGISTER_COUNT ? reply : "E01");
        return;
    }
    case 'P': {
        const char *cursor = packet + 1;
        int number = (int)parse_hex(&cursor);
        if (*cursor++ != '=') { send_packet(gdb, "E01"); return; }
        write_register(gdb, number, unhex_word(cursor));
        send_packet(gdb, "OK");
        return;
    }
    case 'm':
        handle_read_memory(gdb, packet);
        return;
    case 'M':
        handle_write_memory(gdb, packet);
        return;
    case 'c':
    case 'C':
        resume(gdb, false);
        return;
    case 's':
    case 'S':
        resume(gdb, true);
        return;
    case 'Z':
    case 'z':
        handle_breakpoint_packet(gdb, packet);
        return;
    case 'H':
        send_packet(gdb, "OK");
        return;
    case 'T':
        send_packet(gdb, "OK");
        return;
    case 'k':
        gdb->killed = true;
        close_client(gdb);
        return;
    case 'D':
        send_packet(gdb, "OK");
        close_client(gdb);
        return;
    case 'v':
        if (!strncmp(packet, "vCont", 5)) handle_vcont(gdb, packet);
        else send_packet(gdb, "");
        return;
    case 'q':
        if (!strncmp(packet, "qSupported", 10)) send_packet(gdb, "PacketSize=4000;qXfer:features:read+;qXfer:libraries:read+;QStartNoAckMode+;vContSupported+");
        else if (!strncmp(packet, "qXfer:features:read:target.xml:", 31)) handle_xfer(gdb, packet, "features", TARGET_XML, sizeof TARGET_XML - 1);
        else if (!strncmp(packet, "qXfer:libraries:read::", 22)) {
            static char xml[CE_MODULE_MAX * 128 + 64];
            size_t length = library_list(gdb, xml, sizeof xml);
            gdb->library_hash = current_library_hash(gdb);
            handle_xfer(gdb, packet, "libraries", xml, length);
        }
        else if (!strcmp(packet, "qAttached")) send_packet(gdb, "1");
        else if (!strncmp(packet, "qRcmd,", 6)) handle_monitor(gdb, packet);
        else if (!strcmp(packet, "qSymbol::")) send_packet(gdb, "OK");
        else send_packet(gdb, "");
        return;
    case 'Q':
        if (!strcmp(packet, "QStartNoAckMode")) {
            send_packet(gdb, "OK");
            gdb->no_ack = true;
        } else {
            send_packet(gdb, "");
        }
        return;
    default:
        send_packet(gdb, "");
    }
}

static bool receive(gdb_t *gdb, bool wait) {
    if (gdb->client < 0) return false;
    struct pollfd poll_fd = { .fd = gdb->client, .events = POLLIN };
    int ready = poll(&poll_fd, 1, wait ? -1 : 0);
    if (ready <= 0) return ready == 0 || errno == EINTR;
    if (gdb->input_length == sizeof gdb->input) gdb->input_length = 0;
    ssize_t received = recv(gdb->client, gdb->input + gdb->input_length, sizeof gdb->input - gdb->input_length, 0);
    if (received <= 0) {
        logf_gdb(gdb, "gdb: client disconnected\n");
        close_client(gdb);
        return false;
    }
    gdb->input_length += (size_t)received;
    return true;
}

static void consume(gdb_t *gdb, size_t count) {
    memmove(gdb->input, gdb->input + count, gdb->input_length - count);
    gdb->input_length -= count;
}

static bool next_packet(gdb_t *gdb, char *packet, bool *interrupt) {
    *interrupt = false;
    while (gdb->input_length) {
        uint8_t first = gdb->input[0];
        if (first == 0x03) {
            consume(gdb, 1);
            *interrupt = true;
            return false;
        }
        if (first != '$') {
            consume(gdb, 1);
            continue;
        }
        uint8_t *end = memchr(gdb->input, '#', gdb->input_length);
        if (!end || (size_t)(end - gdb->input) + 3 > gdb->input_length) return false;
        size_t length = (size_t)(end - gdb->input) - 1;
        if (length >= PACKET_MAX) length = PACKET_MAX - 1;
        memcpy(packet, gdb->input + 1, length);
        packet[length] = 0;
        consume(gdb, (size_t)(end - gdb->input) + 3);
        if (!gdb->no_ack) send_all(gdb, "+", 1);
        return true;
    }
    return false;
}

static void detach_debugger(gdb_t *gdb) {
    gdb->breakpoint_count = 0;
    gdb->entry_break_count = 0;
    gdb->module_added = false;
    gdb->new_module = 0;
    gdb->entry_written = false;
    gdb->watchpoint_count = 0;
    gdb->halted = false;
    gdb->stepping = false;
    gdb->resume_skip = false;
    gdb->post_mortem = false;
    gdb->debug.stop = false;
    update_debug(gdb);
}

static bool accept_client(gdb_t *gdb, bool wait) {
    struct pollfd poll_fd = { .fd = gdb->listener, .events = POLLIN };
    if (poll(&poll_fd, 1, wait ? -1 : 0) <= 0) return false;
    int client = accept(gdb->listener, NULL, NULL);
    if (client < 0) return false;
    int on = 1;
    setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &on, sizeof on);
    gdb->client = client;
    gdb->no_ack = false;
    gdb->library_hash = 0;
    gdb->killed = false;
    gdb->input_length = 0;
    gdb->stop_kind = STOP_SIGNAL;
    gdb->stop_signal = SIGNAL_TRAP;
    gdb->halted = true;
    gdb->module_list_known = ce_module_list_address(&gdb->ce, &gdb->module_list_pa);
    update_debug(gdb);
    logf_gdb(gdb, "gdb: client connected\n");
    return true;
}

gdb_t *gdb_create(machine_t *machine, int port, gdb_log_fn log) {
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) return NULL;
    int on = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
    struct sockaddr_in address = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port), .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    if (bind(listener, (struct sockaddr *)&address, sizeof address) < 0 || listen(listener, 1) < 0) {
        close(listener);
        return NULL;
    }
    gdb_t *gdb = calloc(1, sizeof *gdb);
    gdb->machine = machine;
    gdb->log = log;
    gdb->listener = listener;
    gdb->client = -1;
    gdb->process = -1;
    gdb->last_user_process = -1;
    gdb->catch_faults = true;
    gdb->forward_output = true;
    gdb->elf_libraries = true;
    ce_init(&gdb->ce, machine);
    gdb->debug.context = gdb;
    gdb->debug.before = on_before;
    gdb->debug.access = on_access;
    gdb->debug.exception = on_exception;
    machine_cpu(machine)->debug = &gdb->debug;
    logf_gdb(gdb, "gdb: listening on 127.0.0.1:%d\n", port);
    return gdb;
}

void gdb_destroy(gdb_t *gdb) {
    if (!gdb) return;
    if (gdb->client >= 0 && !gdb->killed) send_packet(gdb, "W00");
    machine_cpu(gdb->machine)->debug = NULL;
    close_client(gdb);
    close(gdb->listener);
    free(gdb);
}

bool gdb_wait_for_client(gdb_t *gdb) {
    return accept_client(gdb, true);
}

bool gdb_connected(const gdb_t *gdb) {
    return gdb->client >= 0;
}

bool gdb_set_process(gdb_t *gdb, const char *name) {
    gdb->process = -1;
    gdb->waiting_for_process = false;
    gdb->process_name[0] = 0;
    if (name && *name) {
        snprintf(gdb->process_name, sizeof gdb->process_name, "%s", name);
        gdb->process = ce_find_process(&gdb->ce, name);
        gdb->waiting_for_process = gdb->process < 0;
        gdb->last_user_process = current_process(gdb);
    }
    update_debug(gdb);
    return gdb->process >= 0;
}

void gdb_service(gdb_t *gdb) {
    if (gdb->client < 0) {
        detach_debugger(gdb);
        if (!accept_client(gdb, false)) return;
    }
    static char packet[PACKET_MAX];
    receive(gdb, false);
    while (gdb->client >= 0) {
        bool interrupt;
        bool got = next_packet(gdb, packet, &interrupt);
        if (interrupt) {
            if (!gdb->halted) {
                gdb->stop_kind = STOP_SIGNAL;
                gdb->stop_signal = SIGNAL_INT;
                halt(gdb);
            }
            continue;
        }
        if (!got) break;
        handle_packet(gdb, packet);
    }
    if (gdb->client < 0) detach_debugger(gdb);
}

bool gdb_halted(const gdb_t *gdb) {
    return gdb->halted && gdb->client >= 0;
}

static void check_libraries(gdb_t *gdb) {
    if (++gdb->library_check < LIBRARY_CHECK_QUANTA) return;
    gdb->library_check = 0;
    if (!gdb->module_list_known && (gdb->module_list_known = ce_module_list_address(&gdb->ce, &gdb->module_list_pa))) update_debug(gdb);
    uint32_t hash = current_library_hash(gdb);
    if (hash == gdb->library_hash) return;
    gdb->library_hash = hash;
    gdb->halted = true;
    gdb->stop_kind = STOP_SIGNAL;
    gdb->stop_signal = SIGNAL_TRAP;
    update_debug(gdb);
    send_packet(gdb, "T05library:;");
}

void gdb_after_run(gdb_t *gdb) {
    if (gdb->debug.stop) {
        gdb->debug.stop = false;
        if (gdb->client >= 0) halt(gdb);
        return;
    }
    if (gdb->client >= 0 && !gdb->halted) check_libraries(gdb);
}

void gdb_set_machine(gdb_t *gdb, machine_t *machine) {
    if (gdb->machine && gdb->machine != machine) machine_cpu(gdb->machine)->debug = NULL;
    gdb->machine = machine;
    ce_init(&gdb->ce, machine);
    gdb->debug.stop = false;
    gdb->post_mortem = false;
    memset(gdb->faults, 0, sizeof gdb->faults);
    if (gdb->process_name[0]) {
        char name[CE_NAME_MAX];
        snprintf(name, sizeof name, "%s", gdb->process_name);
        gdb_set_process(gdb, name);
    }
    machine_cpu(machine)->debug = &gdb->debug;
    if (gdb_halted(gdb)) {
        gdb->stop_kind = STOP_SIGNAL;
        gdb->stop_signal = SIGNAL_TRAP;
        send_console(gdb, "velo: switched machine\n");
    }
}

bool gdb_run(gdb_t *gdb, uint64_t cycles) {
    uint64_t target = machine_cycles(gdb->machine) + cycles;
    while (machine_cycles(gdb->machine) < target && !gdb->killed) {
        gdb_service(gdb);
        if (gdb->killed) break;
        if (gdb_halted(gdb)) {
            receive(gdb, true);
            continue;
        }
        uint64_t remaining = target - machine_cycles(gdb->machine);
        machine_run(gdb->machine, remaining < RUN_QUANTUM ? remaining : RUN_QUANTUM);
        gdb_after_run(gdb);
    }
    return !gdb->killed;
}
