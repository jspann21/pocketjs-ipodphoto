#ifndef POCKETJS_IPOD_PHOTO_INPUT_H
#define POCKETJS_IPOD_PHOTO_INPUT_H

#include <stdbool.h>
#include <stdint.h>

enum {
    PJS_BUTTON_SELECT = 1u << 0,
    PJS_BUTTON_RIGHT  = 1u << 1,
    PJS_BUTTON_LEFT   = 1u << 2,
    PJS_BUTTON_PLAY   = 1u << 3,
    PJS_BUTTON_MENU   = 1u << 4,
};

typedef struct {
    uint32_t buttons;
    int8_t wheel_delta;
    uint8_t wheel_position;
    bool wheel_touched;
    bool hold;
    bool valid_packet;
} PjsInputState;

void input_init(void);
void input_poll(PjsInputState *state);

/* Pure packet decoder used by target code and host-side tests. */
bool input_decode_packet(uint32_t packet, PjsInputState *state);

#endif
