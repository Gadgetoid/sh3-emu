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
    MENU_COUNT,
};

void menu_install(void);
void menu_ensure(void);
int  menu_poll(void);
void menu_set_checked(int item, bool checked);
