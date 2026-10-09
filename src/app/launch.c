#include "app/launch.h"

#include <stdio.h>
#include <string.h>

enum {
    LAUNCH_HEADING_MACHINE, LAUNCH_MACHINE, LAUNCH_STATE, LAUNCH_FRESH, LAUNCH_CARD, LAUNCH_MEMORY, LAUNCH_SPEED, LAUNCH_OPTIMISATIONS,
    LAUNCH_HEADING_CONNECTIONS, LAUNCH_NET, LAUNCH_TCP, LAUNCH_USER_AGENT, LAUNCH_AGENT,
    LAUNCH_HEADING_DEBUGGING, LAUNCH_VERBOSE, LAUNCH_DEBUG_OUTPUT, LAUNCH_GDB, LAUNCH_GDB_PROCESS,
};

static const option_t LAUNCH_OPTIONS[] = {
    [LAUNCH_HEADING_MACHINE] = { NULL, NULL, "Machine", 0 },
    [LAUNCH_MACHINE] = { "machine", "NAME", "open the machine with this name (from Machine > Machines)", 0 },
    [LAUNCH_STATE] = { "state", "FILE", "load, save and autosave FILE instead of the ROM's own state", 0 },
    [LAUNCH_FRESH] = { "fresh", NULL, "ignore the saved state and cold boot", 0 },
    [LAUNCH_CARD] = { "card", "IMAGE", "insert a CompactFlash card backed by a raw disk image", 0 },
    [LAUNCH_MEMORY] = { "memory", "MB", "RAM for a ROM given on the command line: 16, 32 or 64", 0 },
    [LAUNCH_SPEED] = { "speed", "N", "CPU speed multiple: 1, 2, 4 or 8", 0 },
    [LAUNCH_OPTIMISATIONS] = { "optimisations", "on|off", "run CE's ROM compression natively and skip busy-waits on the clock, and remember that", 0 },
    [LAUNCH_HEADING_CONNECTIONS] = { NULL, NULL, "Connections", 0 },
    [LAUNCH_NET] = { "net", NULL, "plug COM1 into the PPP network (Devices > Serial Port), and remember that", 0 },
    [LAUNCH_TCP] = { "tcp", "[PORT]", "offer COM1 as raw bytes on a TCP port on all interfaces (9991, or PORT, kept in sh3emu.ini), with the cable connected while a client is attached, and remember that", 0 },
    [LAUNCH_USER_AGENT] = { "user-agent", "TEXT", "the web proxy's user agent, kept in sh3emu.ini; empty passes Pocket IE's own through", 0 },
    [LAUNCH_AGENT] = { "agent", "SOCKET", "pass messages between a guest agent's trapa #0xCE mailbox and one client on this Unix socket", 0 },
    [LAUNCH_HEADING_DEBUGGING] = { NULL, NULL, "Debugging", 0 },
    [LAUNCH_VERBOSE] = { "verbose", NULL, "log unmodelled hardware accesses to stderr", 0 },
    [LAUNCH_DEBUG_OUTPUT] = { "debug-output", NULL, "print CE's debug output (OutputDebugString, kernel messages) to stderr as well as debug.log", 0 },
    [LAUNCH_GDB] = { "gdb", "PORT", "listen for GDB on 127.0.0.1:PORT; it can attach and detach while the machine runs", 0 },
    [LAUNCH_GDB_PROCESS] = { "gdb-process", "NAME", "debug one process, e.g. maths.exe: breakpoints below 0x02000000 only stop there, and GDB stops when it starts", 0 },
};

static bool launch_option(void *context, int option, const char *value, char *error, size_t error_size) {
    launch_t *launch = context;
    settings_t *settings = launch->settings;
    long integer;
    (void)error;
    (void)error_size;
    switch (option) {
    case LAUNCH_MACHINE: launch->machine = value; return true;
    case LAUNCH_STATE: launch->state_file = value; return true;
    case LAUNCH_FRESH: launch->fresh = true; return true;
    case LAUNCH_CARD: launch->card = value; return true;
    case LAUNCH_MEMORY:
        if (!option_integer(value, 10, &integer) || (integer != 16 && integer != 32 && integer != 64)) return false;
        settings->memory = (uint32_t)integer;
        return true;
    case LAUNCH_SPEED:
        if (!option_integer(value, 10, &integer) || (integer != 1 && integer != 2 && integer != 4 && integer != 8)) return false;
        settings->speed = (uint32_t)integer;
        return true;
    case LAUNCH_OPTIMISATIONS:
        if (strcmp(value, "on") && strcmp(value, "off")) return false;
        settings->optimisations = !strcmp(value, "on");
        return true;
    case LAUNCH_VERBOSE: launch->verbose = true; return true;
    case LAUNCH_DEBUG_OUTPUT: launch->debug_output = true; return true;
    case LAUNCH_GDB:
        if (!option_integer(value, 10, &integer) || integer < 1 || integer > 65535) return false;
        launch->gdb_port = (int)integer;
        return true;
    case LAUNCH_GDB_PROCESS: launch->gdb_process = value; return true;
    case LAUNCH_NET: settings->serial = SERIAL_NETWORK; return true;
    case LAUNCH_TCP:
        if (value && (!option_integer(value, 10, &integer) || integer < 1 || integer > 65535)) return false;
        if (value) settings->serial_tcp_port = (uint32_t)integer;
        settings->serial = SERIAL_TCP;
        return true;
    case LAUNCH_USER_AGENT: snprintf(settings->user_agent, sizeof settings->user_agent, "%s", value); return true;
    case LAUNCH_AGENT: launch->agent_socket = value; return true;
    }
    return false;
}

static const option_spec_t LAUNCH_SPEC = {
    "sh3emu", "[OPTIONS] [ROM]",
    "Emulates the Casio Cassiopeia A-51, HP 300LX and HP 320LX Windows CE handhelds. With no ROM it opens the last machine used; machines are made with Machine > New Machine from the ROMs in the roms folder in its data folder. With a ROM it runs that ROM with its own saved state, outside the machine list.",
    LAUNCH_OPTIONS, (int)(sizeof LAUNCH_OPTIONS / sizeof LAUNCH_OPTIONS[0]),
    "headless runs the machine without a window, for tests and scripts.",
};

options_result_t launch_parse(launch_t *launch, settings_t *settings, int argc, char **argv) {
    *launch = (launch_t){ .settings = settings };
    const char *positional[1];
    int positional_count;
    options_result_t parsed = options_parse(&LAUNCH_SPEC, argc, argv, launch_option, launch, positional, 1, &positional_count);
    if (parsed == OPTIONS_OK && positional_count) launch->rom = positional[0];
    return parsed;
}
