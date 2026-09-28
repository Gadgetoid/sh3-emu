#pragma once
#include <stdbool.h>

enum {
    MENU_POWER,
    MENU_PAUSE,
    MENU_RESET,
    MENU_SAVE_STATE,
    MENU_LOAD_STATE,
    MENU_BACKLIGHT,
    MENU_SOUND,
    MENU_INSERT_CARD,
    MENU_EJECT_CARD,
    MENU_SERIAL_NETWORK,
    MENU_SERIAL_PTY,
    MENU_SERIAL_OFF,
    MENU_COUNT,
};

void menu_install(void);
void menu_ensure(void);
int  menu_poll(void);
void menu_set_checked(int item, bool checked);
void menu_set_enabled(int item, bool enabled);

enum { MENU_MOD_SHIFT = 1, MENU_MOD_CONTROL = 2, MENU_MOD_ALT = 4, MENU_MOD_KNOWN = 8 };
int  menu_modifiers(void);
