#include "platform/platform.h"
#include "include/pad.h"
#include <3ds.h>

/* Global mode flags (declared extern in platform.h) */
bool platform_headless = false;
bool platform_skip_intro = false;
int platform_max_frames = 0;

static uint16_t pad_state;
static uint16_t pad_prev;
static uint16_t aux_state;
static bool quit_requested;

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
    pad_state = p;

    /* Touching the bottom screen toggles the FPS/profiling overlay (the
     * desktop port's F3). Reported as a raw level; game_main.c does the edge
     * detection and toggling centrally, so one tap is one toggle. Chosen over
     * ZL/ZR because those only exist on New 3DS. */
    aux_state = (held & KEY_TOUCH) ? AUX_FPS_TOGGLE : 0;
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
