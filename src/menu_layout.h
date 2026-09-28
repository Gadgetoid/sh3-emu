#pragma once
#include "menu.h"

typedef enum {
    MENU_ENTRY_MENU,
    MENU_ENTRY_END,
    MENU_ENTRY_ITEM,
    MENU_ENTRY_SEPARATOR,
} menu_entry_kind_t;

enum {
    MENU_KEY_PRIMARY = 1,
    MENU_KEY_SHIFT = 2,
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
    { MENU_ENTRY_ITEM, MENU_RESET, "Reset", 'r', MENU_KEY_PRIMARY },
    { MENU_ENTRY_SEPARATOR, 0, NULL, 0, 0 },
    { MENU_ENTRY_ITEM, MENU_SAVE_STATE, "Save State", 's', MENU_KEY_PRIMARY },
    { MENU_ENTRY_ITEM, MENU_LOAD_STATE, "Load State", 'l', MENU_KEY_PRIMARY },
    { MENU_ENTRY_END, 0, NULL, 0, 0 },

    { MENU_ENTRY_MENU, 0, "Card", 0, 0 },
    { MENU_ENTRY_ITEM, MENU_INSERT_CARD, "Insert Card Image\xe2\x80\xa6", 'o', MENU_KEY_PRIMARY },
    { MENU_ENTRY_ITEM, MENU_EJECT_CARD, "Eject Card", 'e', MENU_KEY_PRIMARY },
    { MENU_ENTRY_END, 0, NULL, 0, 0 },

    { MENU_ENTRY_MENU, 0, "Emulation", 0, 0 },
    { MENU_ENTRY_ITEM, MENU_BACKLIGHT, "Backlight", 'b', MENU_KEY_PRIMARY },
    { MENU_ENTRY_ITEM, MENU_SOUND, "Sound", 0, 0 },
    { MENU_ENTRY_END, 0, NULL, 0, 0 },
};

static const int MENU_ENTRY_COUNT = sizeof MENU_ENTRIES / sizeof MENU_ENTRIES[0];
