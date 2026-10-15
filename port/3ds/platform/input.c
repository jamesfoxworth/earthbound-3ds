#include "platform/platform.h"
#include "include/pad.h"
#include "game/window.h"
#include <3ds.h>

/* Defined in platform/video.c */
bool platform_bottom_hit_test(int touch_x, int touch_y, uint8_t *out_window_id,
                              int *out_tile_col, int *out_tile_row);

/* Global mode flags (declared extern in platform.h) */
bool platform_headless = false;
bool platform_skip_intro = false;
int platform_max_frames = 0;

static uint16_t pad_state;
static uint16_t pad_prev;
static uint16_t aux_state;
static bool quit_requested;

/*
 * Touch-to-select on the bottom screen.
 *
 * A tap picks the menu option under it, then the cursor is walked there by
 * feeding PAD_UP/PAD_DOWN through the normal pad path -- core.pad1_pressed is
 * edge-detected from whatever platform_input_get_pad() reports, so synthetic
 * directions are indistinguishable from real ones and no engine state is
 * poked. One step per frame, so a distant option takes a few frames.
 *
 * Deliberately does not confirm: moving the cursor is recoverable, a mis-tap
 * that selects is not. The player still presses A.
 */
static int  touch_target = -1;   /* menu option index, -1 = none pending */
static int  touch_budget;        /* frames left to converge, then give up */
static bool touch_was_down;

/* The option whose label sits at a tile position, or -1. Windows lay menu
 * items out at text_x/text_y in window tiles; a 2-column menu has two items
 * per row, so the column decides between them. */
static int option_at_tile(const WindowInfo *w, int col, int row) {
    int best = -1, best_dx = 1 << 30;
    for (int i = 0; i < w->menu_count && i < 24; i++) {
        const MenuItem *it = &w->menu_items[i];
        if (it->text_y != row)
            continue;
        int dx = col - (int)it->text_x;
        if (dx < 0) dx = -dx;
        if (dx < best_dx) {
            best_dx = dx;
            best = i;
        }
    }
    return best;
}

static const WindowInfo *window_by_id(uint8_t id) {
    if (id == WINDOW_ID_NONE)
        return NULL;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (win.windows[i].active && win.windows[i].id == id)
            return &win.windows[i];
    }
    return NULL;
}

/* Returns a synthetic pad direction that walks the cursor toward touch_target,
 * or 0 when there is nothing to do. */
static uint16_t touch_step(void) {
    if (touch_target < 0)
        return 0;

    const WindowInfo *w = window_by_id(win.current_focus_window);
    if (!w || touch_target >= w->menu_count || --touch_budget <= 0) {
        touch_target = -1;
        return 0;
    }

    int cur = w->current_option;
    if (cur == touch_target) {
        touch_target = -1;
        return 0;
    }
    /* Menus wrap, but stepping the short way is close enough and never gets
     * stuck: the budget above bounds it either way. */
    return (touch_target > cur) ? PAD_DOWN : PAD_UP;
}

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
     * detection and toggling centrally, so one tap is one toggle.
     *
     * ZR dumps PPU state to the SD card (the desktop port's F1): screenshot,
     * VRAM, CGRAM, the PPU registers and the window tilemap buffer. New 3DS
     * only, which is fine for a debugging aid. */
    aux_state = 0;
    if (held & KEY_ZR) aux_state |= AUX_DEBUG_DUMP;

    /* A new touch either picks a menu option on the bottom screen or, if it
     * misses the window, toggles the FPS overlay as before. */
    bool touch_down = (held & KEY_TOUCH) != 0;
    if (touch_down && !touch_was_down) {
        touchPosition tp;
        hidTouchRead(&tp);
        uint8_t hit_id = WINDOW_ID_NONE;
        int col = 0, row = 0;
        int option = -1;
        if (platform_bottom_hit_test(tp.px, tp.py, &hit_id, &col, &row)) {
            const WindowInfo *w = window_by_id(hit_id);
            if (w)
                option = option_at_tile(w, col, row);
        }
        if (option >= 0) {
            touch_target = option;
            touch_budget = 40;
        } else {
            aux_state |= AUX_FPS_TOGGLE;
        }
    }
    touch_was_down = touch_down;

    pad_state |= touch_step();
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
