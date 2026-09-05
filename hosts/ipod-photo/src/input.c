#include "input.h"
#include "pp5020.h"
#include "timer.h"

bool input_decode_packet(uint32_t packet, PjsInputState *state)
{
    if ((packet & 0x800000ffu) != 0x8000001au) {
        return false;
    }

    uint32_t buttons = 0u;
    if ((packet & 0x00000100u) != 0u) buttons |= PJS_BUTTON_SELECT;
    if ((packet & 0x00000200u) != 0u) buttons |= PJS_BUTTON_RIGHT;
    if ((packet & 0x00000400u) != 0u) buttons |= PJS_BUTTON_LEFT;
    if ((packet & 0x00000800u) != 0u) buttons |= PJS_BUTTON_PLAY;
    if ((packet & 0x00001000u) != 0u) buttons |= PJS_BUTTON_MENU;

    state->buttons = buttons;
    state->wheel_touched = (packet & 0x40000000u) != 0u;
    state->wheel_position = (uint8_t)((packet >> 16) & 0x7fu);
    state->wheel_delta = 0;
    state->valid_packet = true;
    return true;
}

#ifndef PJS_HOST_TEST
static int previous_wheel = -1;
static PjsInputState last_state;

static void wheel_reprime(void)
{
    PP_WHEEL_CTRL0 &= ~0x60000000u;
    PP_WHEEL_CTRL1 |= 0x04000000u;
    PP_WHEEL_CTRL0 |= 0x60000000u;
}

void input_init(void)
{
    PP_DEV_EN |= PP_DEV_OPTO;
    PP_DEV_RS |= PP_DEV_OPTO;
    timer_delay_us(5u);
    PP_DEV_RS &= ~PP_DEV_OPTO;
    PP_DEV_INIT1 |= PP_INIT_BUTTONS;

    PP_WHEEL_CTRL0 = 0xc00a1f00u;
    PP_WHEEL_CTRL1 = 0x01000000u;
    wheel_reprime();

    /* Hold switch: GPIO A5, active low. */
    PP_GPIOA_ENABLE |= 0x20u;
    PP_GPIOA_OUTPUT_EN &= ~0x20u;
    previous_wheel = -1;
    last_state = (PjsInputState){0};
}

void input_poll(PjsInputState *state)
{
    PjsInputState next = last_state;
    next.wheel_delta = 0;
    next.hold = (PP_GPIOA_INPUT_VAL & 0x20u) == 0u;

    if ((PP_WHEEL_CTRL1 & 0x04000000u) != 0u) {
        uint32_t packet = PP_WHEEL_DATA;
        if (input_decode_packet(packet, &next)) {
            if (next.wheel_touched) {
                int current = (int)next.wheel_position;
                if (previous_wheel >= 0) {
                    int delta = current - previous_wheel;
                    if (delta < -48) delta += 96;
                    if (delta > 48) delta -= 96;
                    if (delta < -127) delta = -127;
                    if (delta > 127) delta = 127;
                    next.wheel_delta = (int8_t)delta;
                }
                previous_wheel = current;
            } else {
                previous_wheel = -1;
            }

            PP_WHEEL_CTRL1 |= 0x0c000000u;
            PP_WHEEL_CTRL0 = 0x400a1f00u;
        } else if ((packet & 0x800000ffu) != 0x8000003au && packet != 0xffffffffu) {
            next.valid_packet = false;
            timer_delay_us(2000u);
            wheel_reprime();
        }
    }

    last_state = next;
    *state = next;
}
#endif /* PJS_HOST_TEST */
