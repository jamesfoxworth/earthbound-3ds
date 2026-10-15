#ifndef EB_3DS_TOUCH_H
#define EB_3DS_TOUCH_H

#include <stdbool.h>

/* Bottom-screen touch, in bottom-screen pixels (320x240).
 *
 * Port-local rather than part of platform.h: platform.h is the contract every
 * port implements, and a touch screen is specific to this one. The bottom
 * screen is this port's own UI, so nothing in src/ needs to see this.
 *
 * `pressed` and `released` are single-frame edges; `down` is the level. x/y
 * hold the last known position and stay valid on the release frame so a tap
 * can be resolved against where it started. */
typedef struct {
    int  x, y;
    bool down;
    bool pressed;
    bool released;
} TouchState;

const TouchState *touch_get(void);

/* Raise AUX_* bits for exactly one poll. game_main.c edge-detects the aux
 * level, so a single-frame pulse is one action -- the same path the desktop
 * port's F3/F6/F7 keys take. */
void input_pulse_aux(uint16_t bits);

/* Inject a one-frame pad press. The game edge-detects into core.pad1_pressed,
 * so this is indistinguishable from the player pressing the button -- which is
 * what lets the bottom-screen hub drive the real menus without the port
 * reaching into the mode stack itself. */
void input_pulse_pad(uint16_t bits);

/* True when (x,y) falls inside the rect; convenience for hit-testing. */
static inline bool touch_in(const TouchState *t, int x, int y, int w, int h) {
    return t->x >= x && t->x < x + w && t->y >= y && t->y < y + h;
}

#endif
