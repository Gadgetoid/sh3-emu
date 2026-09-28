#include "menu.h"

void menu_install(void) {}
void menu_ensure(void) {}
int  menu_poll(void) { return -1; }
void menu_set_checked(int item, bool checked) { (void)item; (void)checked; }
void menu_set_enabled(int item, bool enabled) { (void)item; (void)enabled; }
int  menu_modifiers(void) { return 0; }
