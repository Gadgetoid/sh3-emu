#pragma once
#include <stdbool.h>
#include <stdint.h>

#define LCD_WIDTH    480
#define LCD_HEIGHT   240
#define LCD_MARGIN_X 4
#define LCD_MARGIN_Y 4

extern uint8_t lcd_framebuffer[LCD_WIDTH * LCD_HEIGHT];

void      lcd_compose_setup(int cell);
bool      lcd_compose(float seconds);
void      lcd_invalidate(void);
bool      lcd_needs_compose(void);
uint32_t *lcd_compose_pixels(void);
int       lcd_compose_width(void);
int       lcd_compose_height(void);
void      lcd_set_backlight(bool on);
bool      lcd_get_backlight(void);
void      lcd_set_power(bool on);
void      lcd_set_response(float scale);
void      lcd_set_contrast(int level);
int       lcd_get_contrast(void);
