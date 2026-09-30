#include "view.h"
#include "lcd.h"

#include <math.h>
#include <stdlib.h>

#define GRID_W (LCD_WIDTH + 2 * LCD_MARGIN_X)
#define GRID_H (LCD_HEIGHT + 2 * LCD_MARGIN_Y)

struct view {
    SDL_Window    *window;
    SDL_Renderer  *renderer;
    SDL_Texture   *texture;
    view_display_t display;
    int            texture_w, texture_h;
    int            output_w, output_h;
    bool           laid_out;
    SDL_FRect      dest;
    uint32_t       sharp[LCD_WIDTH * LCD_HEIGHT];
};

void view_source_size(view_display_t display, int *width, int *height) {
    *width = display == VIEW_SIMULATED ? GRID_W : LCD_WIDTH;
    *height = display == VIEW_SIMULATED ? GRID_H : LCD_HEIGHT;
}

view_t *view_create(SDL_Window *window, SDL_Renderer *renderer, view_display_t display) {
    view_t *view = calloc(1, sizeof *view);
    view->window = window;
    view->renderer = renderer;
    view->display = display;
    return view;
}

void view_destroy(view_t *view) {
    if (!view) return;
    if (view->texture) SDL_DestroyTexture(view->texture);
    free(view);
}

void view_set_display(view_t *view, view_display_t display) {
    if (view->display == display) return;
    view->display = display;
    view->laid_out = false;
}

view_display_t view_display(const view_t *view) {
    return view->display;
}

static void layout(view_t *view) {
    int output_w, output_h;
    SDL_GetRenderOutputSize(view->renderer, &output_w, &output_h);
    if (view->laid_out && output_w == view->output_w && output_h == view->output_h) return;
    view->output_w = output_w;
    view->output_h = output_h;
    view->laid_out = true;

    int source_w, source_h;
    view_source_size(view->display, &source_w, &source_h);
    float fit = fminf((float)output_w / source_w, (float)output_h / source_h);
    float scale = fit >= 1.0f ? floorf(fit) : fit;

    int texture_w = LCD_WIDTH, texture_h = LCD_HEIGHT;
    if (view->display == VIEW_SIMULATED) {
        lcd_compose_setup(scale >= 2.0f ? (int)scale : 2);
        lcd_invalidate();
        texture_w = lcd_compose_width();
        texture_h = lcd_compose_height();
    }
    if (!view->texture || texture_w != view->texture_w || texture_h != view->texture_h) {
        if (view->texture) SDL_DestroyTexture(view->texture);
        view->texture = SDL_CreateTexture(view->renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, texture_w, texture_h);
        SDL_SetTextureScaleMode(view->texture, fit >= 1.0f ? SDL_SCALEMODE_NEAREST : SDL_SCALEMODE_LINEAR);
        view->texture_w = texture_w;
        view->texture_h = texture_h;
        if (view->display == VIEW_SIMULATED) lcd_invalidate();
    }
    view->dest.w = source_w * scale;
    view->dest.h = source_h * scale;
    view->dest.x = floorf((output_w - view->dest.w) / 2);
    view->dest.y = floorf((output_h - view->dest.h) / 2);
}

static void fill_sharp(view_t *view, bool powered) {
    for (int i = 0; i < LCD_WIDTH * LCD_HEIGHT; i++) {
        uint32_t grey = powered ? 255u - lcd_framebuffer[i] * 17u : 255u;
        view->sharp[i] = grey | grey << 8 | grey << 16 | 0xff000000u;
    }
}

void view_draw(view_t *view, float seconds, bool powered) {
    layout(view);
    if (view->display == VIEW_SIMULATED) {
        if (lcd_compose(seconds)) SDL_UpdateTexture(view->texture, NULL, lcd_compose_pixels(), lcd_compose_width() * 4);
    } else {
        fill_sharp(view, powered);
        SDL_UpdateTexture(view->texture, NULL, view->sharp, LCD_WIDTH * 4);
    }
    SDL_SetRenderDrawColor(view->renderer, 0, 0, 0, 255);
    SDL_RenderClear(view->renderer);
    SDL_RenderTexture(view->renderer, view->texture, NULL, &view->dest);
    SDL_RenderPresent(view->renderer);
}

bool view_screen_position(view_t *view, float window_x, float window_y, int *x, int *y) {
    int window_w, window_h;
    SDL_GetWindowSize(view->window, &window_w, &window_h);
    if (!window_w || !window_h || view->dest.w <= 0 || view->dest.h <= 0) return false;
    float pixel_x = window_x * view->output_w / window_w, pixel_y = window_y * view->output_h / window_h;
    int source_w, source_h;
    view_source_size(view->display, &source_w, &source_h);
    int margin_x = view->display == VIEW_SIMULATED ? LCD_MARGIN_X : 0;
    int margin_y = view->display == VIEW_SIMULATED ? LCD_MARGIN_Y : 0;
    float lcd_x = (pixel_x - view->dest.x) / view->dest.w * source_w - margin_x;
    float lcd_y = (pixel_y - view->dest.y) / view->dest.h * source_h - margin_y;
    *x = (int)lcd_x;
    *y = (int)lcd_y;
    if (*x < 0) *x = 0;
    if (*y < 0) *y = 0;
    if (*x >= LCD_WIDTH) *x = LCD_WIDTH - 1;
    if (*y >= LCD_HEIGHT) *y = LCD_HEIGHT - 1;
    return lcd_x >= 0 && lcd_y >= 0 && lcd_x < LCD_WIDTH && lcd_y < LCD_HEIGHT;
}

const uint32_t *view_image(view_t *view, int *width, int *height) {
    if (view->display == VIEW_SIMULATED) {
        *width = lcd_compose_width();
        *height = lcd_compose_height();
        return lcd_compose_pixels();
    }
    *width = LCD_WIDTH;
    *height = LCD_HEIGHT;
    return view->sharp;
}
