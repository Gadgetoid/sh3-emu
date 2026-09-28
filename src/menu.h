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
    MENU_SHOW_STATE,
    MENU_MEMORY_4,
    MENU_MEMORY_8,
    MENU_MEMORY_16,
    MENU_MEMORY_20,
    MENU_MEMORY_32,
    MENU_SPEED_1,
    MENU_SPEED_2,
    MENU_SPEED_4,
    MENU_SPEED_8,
    MENU_COUNT,
};

void menu_install(void);
void menu_ensure(void);
int  menu_poll(void);
void menu_set_checked(int item, bool checked);
void menu_set_enabled(int item, bool enabled);

enum { MENU_MOD_SHIFT = 1, MENU_MOD_CONTROL = 2, MENU_MOD_ALT = 4, MENU_MOD_KNOWN = 8 };
int  menu_modifiers(void);
