/* Provide operating-system dialogs, path actions, and host information on Windows. */
#include "app/host.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

bool host_confirm(SDL_Window *window, const char *title, const char *message, const char *action) {
    const SDL_MessageBoxButtonData buttons[] = {
        { SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "Cancel" },
        { SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, action },
    };
    const SDL_MessageBoxData dialog = { SDL_MESSAGEBOX_WARNING, window, title, message, (int)(sizeof buttons / sizeof buttons[0]), buttons, NULL };
    int chosen = 0;
    return SDL_ShowMessageBox(&dialog, &chosen) && chosen == 1;
}

void host_open_path(const char *path) {
    char url[4096] = "file:///";
    size_t length = strlen(url);
    for (const unsigned char *at = (const unsigned char *)path; *at && length + 4 < sizeof url; at++) {
        if (*at == '\\') url[length++] = '/';
        else if (isalnum(*at) || strchr("/-_.~:", *at)) url[length++] = (char)*at;
        else length += (size_t)snprintf(url + length, sizeof url - length, "%%%02X", *at);
    }
    url[length] = 0;
    SDL_OpenURL(url);
}

void host_reap_children(void) {
}

void host_reveal_file(const char *path) {
    char folder[1100];
    snprintf(folder, sizeof folder, "%s", path);
    char *slash = strrchr(folder, '/');
    char *backslash = strrchr(folder, '\\');
    if (backslash > slash) slash = backslash;
    if (slash && slash != folder) *slash = 0;
    host_open_path(folder);
}

void host_local_address(char *address, size_t size) {
    snprintf(address, size, "this computer");
}
