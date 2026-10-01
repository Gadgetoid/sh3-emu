#pragma once
#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stdint.h>

typedef enum { VIEW_SIMULATED, VIEW_SHARP } view_display_t;

typedef struct view view_t;

view_t        *view_create(SDL_Window *window, SDL_Renderer *renderer, view_display_t display);
void           view_destroy(view_t *view);
void           view_set_display(view_t *view, view_display_t display);
view_display_t view_display(const view_t *view);
void           view_source_size(view_display_t display, int *width, int *height);
void           view_draw(view_t *view, float seconds, bool powered);
bool           view_screen_position(view_t *view, float window_x, float window_y, int *x, int *y);
const uint32_t *view_image(view_t *view, int *width, int *height);
