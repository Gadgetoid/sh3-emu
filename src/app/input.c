#include "app/input.h"

#include <math.h>

#include "core/key_text.h"

#define SCROLL_STEP    (MACHINE_CLOCK_HZ / 50)
#define SCROLL_PENDING 8
#define PEN_MIN_CYCLES (MACHINE_CLOCK_HZ * 6 / 100)
#define KEY_MIN_CYCLES (MACHINE_CLOCK_HZ / 50)

typedef struct {
    SDL_Keycode key;
    uint8_t scancode;
} key_binding_t;

static const key_binding_t key_bindings[] = {
    { SDLK_TAB, 0x0D }, { SDLK_BACKSPACE, 0x66 }, { SDLK_RETURN, 0x5A }, { SDLK_ESCAPE, 0x76 },
    { SDLK_LSHIFT, 0x12 }, { SDLK_RSHIFT, 0x59 }, { SDLK_LCTRL, 0x14 }, { SDLK_RCTRL, 0x94 },
    { SDLK_LALT, 0x11 }, { SDLK_RALT, 0x91 }, { SDLK_CAPSLOCK, 0x58 },
    { SDLK_LEFT, 0xEB }, { SDLK_UP, 0xF5 }, { SDLK_RIGHT, 0xF4 }, { SDLK_DOWN, 0xF2 },
    { SDLK_DELETE, 0xF1 }, { SDLK_INSERT, 0xF0 }, { SDLK_HOME, 0xEC }, { SDLK_END, 0xE9 },
    { SDLK_PAGEUP, 0xFD }, { SDLK_PAGEDOWN, 0xFA },
    { SDLK_F1, 0x05 }, { SDLK_F2, 0x06 }, { SDLK_F3, 0x04 }, { SDLK_F4, 0x0C }, { SDLK_F5, 0x03 }, { SDLK_F6, 0x0B },
    { SDLK_F7, 0x83 }, { SDLK_F8, 0x0A }, { SDLK_F9, 0x01 }, { SDLK_F10, 0x09 }, { SDLK_F11, 0x78 }, { SDLK_F12, 0x07 },
};

bool input_find_scancode(key_layout_t layout, SDL_Keycode key, uint8_t *scancode) {
    if (key >= 0x20 && key < 0x7F) {
        bool shifted;
        return key_text_find(layout, (char)key, scancode, &shifted);
    }
    for (size_t i = 0; i < sizeof key_bindings / sizeof key_bindings[0]; i++) {
        if (key_bindings[i].key == key) { *scancode = key_bindings[i].scancode; return true; }
    }
    return false;
}

void scroller_add(scroller_t *scroller, float vertical, float horizontal) {
    scroller->vertical += vertical;
    scroller->horizontal += horizontal;
    float *axis = fabsf(scroller->vertical) >= fabsf(scroller->horizontal) ? &scroller->vertical : &scroller->horizontal;
    if (fabsf(*axis) < 1.0f) return;
    uint8_t scancode = axis == &scroller->vertical ? (*axis > 0 ? 0xF5 : 0xF2) : (*axis > 0 ? 0xF4 : 0xEB);
    int steps = (int)fabsf(*axis);
    *axis -= *axis > 0 ? (float)steps : -(float)steps;
    if (scancode != scroller->scancode) {
        if (scroller->pressed) return;
        scroller->scancode = scancode;
        scroller->pending = 0;
    }
    scroller->pending += steps;
    if (scroller->pending > SCROLL_PENDING) scroller->pending = SCROLL_PENDING;
}

void scroller_step(scroller_t *scroller, machine_t *machine) {
    uint64_t now = machine_cycles(machine);
    if (scroller->next_at > now + SCROLL_STEP) scroller->next_at = now;
    if (!scroller->pending || now < scroller->next_at) return;
    machine_key(machine, scroller->scancode, scroller->pressed);
    if (scroller->pressed) scroller->pending--;
    scroller->pressed = !scroller->pressed;
    scroller->next_at = now + SCROLL_STEP;
}

static void input_rebase(input_queue_t *input, uint64_t now) {
    if (now < input->seen) {
        for (int i = 0; i < input->count; i++) input->events[i].at = now;
        input->last_at = now;
    }
    input->seen = now;
}

void input_add(input_queue_t *input, machine_t *machine, input_kind_t kind, bool down, int x, int y, uint8_t scancode) {
    if (input->count == INPUT_QUEUE) return;
    uint64_t now = machine_cycles(machine);
    input_rebase(input, now);
    uint64_t spacing = kind == INPUT_PEN ? PEN_MIN_CYCLES : KEY_MIN_CYCLES;
    uint64_t at = input->last_at + spacing > now ? input->last_at + spacing : now;
    input->events[input->count].at = at;
    input->events[input->count].kind = kind;
    input->events[input->count].down = down;
    input->events[input->count].x = x;
    input->events[input->count].y = y;
    input->events[input->count].scancode = scancode;
    input->count++;
    input->last_at = at;
}

void pen_move(input_queue_t *input, machine_t *machine, int x, int y) {
    if (input->count) return;
    machine_touch(machine, true, x, y);
}

void input_step(input_queue_t *input, machine_t *machine) {
    uint64_t now = machine_cycles(machine);
    input_rebase(input, now);
    int done = 0;
    while (done < input->count && input->events[done].at <= now) {
        if (input->events[done].kind == INPUT_PEN) machine_touch(machine, input->events[done].down, input->events[done].x, input->events[done].y);
        else machine_key(machine, input->events[done].scancode, !input->events[done].down);
        done++;
    }
    for (int i = done; i < input->count; i++) input->events[i - done] = input->events[i];
    input->count -= done;
}

void input_clear(input_queue_t *input) {
    input->count = 0;
    input->last_at = input->seen = 0;
}