#include "platform/platform.h"
#include "include/pad.h"
#include "game/window.h"
#include "touch.h"
#include <3ds.h>


/* Global mode flags (declared extern in platform.h) */
bool platform_headless = false;
bool platform_skip_intro = false;
int platform_max_frames = 0;

static uint16_t pad_state;
static uint16_t pad_prev;
static uint16_t aux_state;
static bool quit_requested;
static TouchState touch;

const TouchState *touch_get(void) { return &touch; }

static bool fps_toggle_pulse;
void input_pulse_fps_toggle(void) { fps_toggle_pulse = true; }

static uint16_t pad_pulse;
void input_pulse_pad(uint16_t bits) { pad_pulse |= bits; }

bool platform_input_init(void) {
    pad_state = 0;
    pad_prev = 0;
    aux_state = 0;
    quit_requested = false;
    return true;
}

void platform_input_shutdown(void) {
    /* nothing to clean up */
}

void platform_input_poll(void) {
    pad_prev = pad_state;

    /* aptMainLoop() returning false means the OS wants us to close (HOME
     * menu close, power button, etc.) — same role as SDL_QUIT on desktop. */
    if (!aptMainLoop()) {
        quit_requested = true;
        return;
    }

    hidScanInput();
    u32 held = hidKeysHeld();

    uint16_t p = 0;
    if (held & KEY_A)      p |= PAD_A;
    if (held & KEY_B)      p |= PAD_B;
    if (held & KEY_X)      p |= PAD_X;
    if (held & KEY_Y)      p |= PAD_Y;
    if (held & KEY_L)      p |= PAD_L;
    if (held & KEY_R)      p |= PAD_R;
    if (held & KEY_START)  p |= PAD_START;
    if (held & KEY_SELECT) p |= PAD_SELECT;
    if (held & KEY_UP)     p |= PAD_UP;
    if (held & KEY_DOWN)   p |= PAD_DOWN;
    if (held & KEY_LEFT)   p |= PAD_LEFT;
    if (held & KEY_RIGHT)  p |= PAD_RIGHT;
    /* Virtual presses from the bottom-screen hub, held for exactly one poll
     * so the game sees a clean press edge. OR'd in after the real pad so a
     * physical press is never masked. */
    p |= pad_pulse;
    pad_pulse = 0;

    pad_state = p;

    /* Touching the bottom screen toggles the FPS/profiling overlay (the
     * desktop port's F3). Reported as a raw level; game_main.c does the edge
     * detection and toggling centrally, so one tap is one toggle.
     *
     * ZR dumps PPU state to the SD card (the desktop port's F1): screenshot,
     * VRAM, CGRAM, the PPU registers and the window tilemap buffer. New 3DS
     * only, which is fine for a debugging aid. */
    aux_state = 0;
    if (held & KEY_ZR) aux_state |= AUX_DEBUG_DUMP;

    /* Touch state for the bottom-screen UI. hidTouchRead only reports a
     * position while the screen is held, so the last one is kept for the
     * release frame -- a tap is resolved against where it started. */
    bool was_down = touch.down;
    touch.down = (held & KEY_TOUCH) != 0;
    if (touch.down) {
        touchPosition tp;
        hidTouchRead(&tp);
        touch.x = tp.px;
        touch.y = tp.py;
    }
    touch.pressed  = touch.down && !was_down;
    touch.released = !touch.down && was_down;

    /* The overlay used to toggle on a touch anywhere, then briefly on an
     * invisible corner patch. It lives in the options panel now, which
     * raises a one-frame pulse here; game_main.c edge-detects the level, so
     * one pulse is one toggle. */
    if (fps_toggle_pulse) {
        aux_state |= AUX_FPS_TOGGLE;
        fps_toggle_pulse = false;
    }
}

uint16_t platform_input_get_pad(void) {
    return pad_state;
}

uint16_t platform_input_get_pad_new(void) {
    return pad_state & (uint16_t)~pad_prev;
}

uint16_t platform_input_get_aux(void) {
    return aux_state;
}

bool platform_input_quit_requested(void) {
    return quit_requested;
}

void platform_request_quit(void) {
    quit_requested = true;
}
