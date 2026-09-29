#pragma once
#include <stdbool.h>
#include <stddef.h>

typedef struct desktop desktop_t;

desktop_t *desktop_create(const char *socket_path, const char *manifest_path);
void       desktop_destroy(desktop_t *desktop);
bool       desktop_busy(desktop_t *desktop);
bool       desktop_take_status(desktop_t *desktop, char *text, size_t size);
bool       desktop_send(desktop_t *desktop, const char *const *files);
bool       desktop_fetch(desktop_t *desktop, const char *local_folder);
bool       desktop_sync(desktop_t *desktop, const char *folder);
