#pragma once
#include "menu.h"

typedef enum {
    MENU_ENTRY_MENU,
    MENU_ENTRY_SUBMENU,
    MENU_ENTRY_END,
    MENU_ENTRY_ITEM,
    MENU_ENTRY_SEPARATOR,
} menu_entry_kind_t;

enum {
    MENU_KEY_PRIMARY = 1,
    MENU_KEY_SHIFT = 2,
    MENU_KEY_CONTROL = 4,
};

typedef struct {
    menu_entry_kind_t kind;
    int tag;
    const char *title;
    char key;
    int modifiers;
} menu_entry_t;

static const menu_entry_t MENU_ENTRIES[] = {
    { MENU_ENTRY_MENU, 0, "Run", 0, 0 },
    { MENU_ENTRY_ITEM, MENU_POWER, "Power Button", 'p', MENU_KEY_PRIMARY | MENU_KEY_SHIFT },
    { MENU_ENTRY_SEPARATOR, 0, NULL, 0, 0 },
    { MENU_ENTRY_ITEM, MENU_PAUSE, "Pause", 'p', MENU_KEY_PRIMARY },
    { MENU_ENTRY_ITEM, MENU_SOFT_RESET, "Soft Reset", 'r', MENU_KEY_PRIMARY },
    { MENU_ENTRY_ITEM, MENU_RESET, "Reset\xe2\x80\xa6", 'r', MENU_KEY_PRIMARY | MENU_KEY_SHIFT },
    { MENU_ENTRY_SEPARATOR, 0, NULL, 0, 0 },
    { MENU_ENTRY_ITEM, MENU_SAVE_STATE, "Save State", 's', MENU_KEY_PRIMARY },
    { MENU_ENTRY_ITEM, MENU_LOAD_STATE, "Load State", 'l', MENU_KEY_PRIMARY },
    { MENU_ENTRY_ITEM, MENU_SHOW_STATE, "Show Saved State in Finder", 0, 0 },
    { MENU_ENTRY_SEPARATOR, 0, NULL, 0, 0 },
    { MENU_ENTRY_ITEM, MENU_SAVE_SNAPSHOT, "Save Snapshot\xe2\x80\xa6", 's', MENU_KEY_PRIMARY | MENU_KEY_CONTROL },
    { MENU_ENTRY_ITEM, MENU_LOAD_SNAPSHOT, "Load Snapshot\xe2\x80\xa6", 'l', MENU_KEY_PRIMARY | MENU_KEY_CONTROL },
    { MENU_ENTRY_ITEM, MENU_SHOW_SNAPSHOTS, "Show Snapshots in Finder", 0, 0 },
    { MENU_ENTRY_END, 0, NULL, 0, 0 },

    { MENU_ENTRY_MENU, 0, "Edit", 0, 0 },
    { MENU_ENTRY_ITEM, MENU_COPY_SCREEN, "Copy Screen", 'c', MENU_KEY_PRIMARY },
    { MENU_ENTRY_ITEM, MENU_PASTE, "Paste as Typing", 'v', MENU_KEY_PRIMARY },
    { MENU_ENTRY_SEPARATOR, 0, NULL, 0, 0 },
    { MENU_ENTRY_ITEM, MENU_SAVE_SCREENSHOT, "Save Screenshot to Desktop", 's', MENU_KEY_PRIMARY | MENU_KEY_SHIFT },
    { MENU_ENTRY_END, 0, NULL, 0, 0 },

    { MENU_ENTRY_MENU, 0, "View", 0, 0 },
    { MENU_ENTRY_ITEM, MENU_SCALE_50, "50%", 0, 0 },
    { MENU_ENTRY_ITEM, MENU_SCALE_75, "75%", 0, 0 },
    { MENU_ENTRY_ITEM, MENU_SCALE_100, "Actual Size", '0', MENU_KEY_PRIMARY },
    { MENU_ENTRY_ITEM, MENU_SCALE_150, "150%", 0, 0 },
    { MENU_ENTRY_ITEM, MENU_SCALE_200, "200%", 0, 0 },
    { MENU_ENTRY_ITEM, MENU_ZOOM_IN, "Zoom In", '=', MENU_KEY_PRIMARY },
    { MENU_ENTRY_ITEM, MENU_ZOOM_OUT, "Zoom Out", '-', MENU_KEY_PRIMARY },
    { MENU_ENTRY_SEPARATOR, 0, NULL, 0, 0 },
    { MENU_ENTRY_ITEM, MENU_FULL_SCREEN, "Full Screen", 'f', MENU_KEY_PRIMARY | MENU_KEY_CONTROL },
    { MENU_ENTRY_SEPARATOR, 0, NULL, 0, 0 },
    { MENU_ENTRY_ITEM, MENU_DISPLAY_SIMULATED, "Simulated LCD", 0, 0 },
    { MENU_ENTRY_ITEM, MENU_DISPLAY_SHARP, "Sharp Pixels", 0, 0 },
    { MENU_ENTRY_END, 0, NULL, 0, 0 },

    { MENU_ENTRY_MENU, 0, "Card", 0, 0 },
    { MENU_ENTRY_ITEM, MENU_INSERT_CARD, "Insert Card Image\xe2\x80\xa6", 'o', MENU_KEY_PRIMARY },
    { MENU_ENTRY_ITEM, MENU_EJECT_CARD, "Eject Card", 'e', MENU_KEY_PRIMARY },
    { MENU_ENTRY_END, 0, NULL, 0, 0 },

    { MENU_ENTRY_MENU, 0, "Serial", 0, 0 },
    { MENU_ENTRY_ITEM, MENU_SERIAL_NETWORK, "Network (PPP)", 'n', MENU_KEY_PRIMARY | MENU_KEY_SHIFT },
    { MENU_ENTRY_ITEM, MENU_SERIAL_PTY, "Pseudo-terminal", 0, 0 },
    { MENU_ENTRY_ITEM, MENU_SERIAL_OFF, "Disconnect", 0, 0 },
    { MENU_ENTRY_SEPARATOR, 0, NULL, 0, 0 },
    { MENU_ENTRY_ITEM, MENU_CONNECT_AT_LAUNCH, "Connect Network at Launch", 0, 0 },
    { MENU_ENTRY_END, 0, NULL, 0, 0 },

    { MENU_ENTRY_MENU, 0, "Desktop", 0, 0 },
    { MENU_ENTRY_ITEM, MENU_SEND_FILES, "Send Files to Velo\xe2\x80\xa6", 0, 0 },
    { MENU_ENTRY_ITEM, MENU_FETCH_DOCUMENTS, "Copy My Documents to Mac\xe2\x80\xa6", 0, 0 },
    { MENU_ENTRY_SEPARATOR, 0, NULL, 0, 0 },
    { MENU_ENTRY_ITEM, MENU_SHARED_FOLDER, "Shared Folder\xe2\x80\xa6", 0, 0 },
    { MENU_ENTRY_ITEM, MENU_SYNC_NOW, "Sync Shared Folder Now", 0, 0 },
    { MENU_ENTRY_ITEM, MENU_STOP_SHARING, "Stop Sharing Folder", 0, 0 },
    { MENU_ENTRY_SEPARATOR, 0, NULL, 0, 0 },
    { MENU_ENTRY_ITEM, MENU_SET_PROXY, "Set Up Pocket IE Proxy", 0, 0 },
    { MENU_ENTRY_SUBMENU, 0, "Desktop Connection Speed", 0, 0 },
    { MENU_ENTRY_ITEM, MENU_BAUD_19200, "19200 (original)", 0, 0 },
    { MENU_ENTRY_ITEM, MENU_BAUD_38400, "38400", 0, 0 },
    { MENU_ENTRY_ITEM, MENU_BAUD_57600, "57600", 0, 0 },
    { MENU_ENTRY_ITEM, MENU_BAUD_115200, "115200", 0, 0 },
    { MENU_ENTRY_END, 0, NULL, 0, 0 },
    { MENU_ENTRY_END, 0, NULL, 0, 0 },

    { MENU_ENTRY_MENU, 0, "Emulation", 0, 0 },
    { MENU_ENTRY_ITEM, MENU_BACKLIGHT, "Backlight", 'b', MENU_KEY_PRIMARY },
    { MENU_ENTRY_ITEM, MENU_SOUND, "Sound", 0, 0 },
    { MENU_ENTRY_SEPARATOR, 0, NULL, 0, 0 },
    { MENU_ENTRY_ITEM, MENU_HOST_TIME, "Use Host Date/Time (after Reset)", 0, 0 },
    { MENU_ENTRY_SUBMENU, 0, "Memory (after Reset)", 0, 0 },
    { MENU_ENTRY_ITEM, MENU_MEMORY_4, "4 MB (original)", 0, 0 },
    { MENU_ENTRY_ITEM, MENU_MEMORY_8, "8 MB", 0, 0 },
    { MENU_ENTRY_ITEM, MENU_MEMORY_16, "16 MB", 0, 0 },
    { MENU_ENTRY_ITEM, MENU_MEMORY_20, "20 MB (4 MB + 16 MB DRAM card)", 0, 0 },
    { MENU_ENTRY_ITEM, MENU_MEMORY_32, "32 MB (16 MB + 16 MB DRAM card)", 0, 0 },
    { MENU_ENTRY_END, 0, NULL, 0, 0 },
    { MENU_ENTRY_SUBMENU, 0, "CPU Speed", 0, 0 },
    { MENU_ENTRY_ITEM, MENU_SPEED_1, "1x (original)", 0, 0 },
    { MENU_ENTRY_ITEM, MENU_SPEED_2, "2x", 0, 0 },
    { MENU_ENTRY_ITEM, MENU_SPEED_4, "4x", 0, 0 },
    { MENU_ENTRY_ITEM, MENU_SPEED_8, "8x", 0, 0 },
    { MENU_ENTRY_END, 0, NULL, 0, 0 },
    { MENU_ENTRY_END, 0, NULL, 0, 0 },
};

static const int MENU_ENTRY_COUNT = sizeof MENU_ENTRIES / sizeof MENU_ENTRIES[0];
